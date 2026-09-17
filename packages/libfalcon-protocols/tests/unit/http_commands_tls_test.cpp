/**
 * @file http_commands_tls_test.cpp
 * @brief V2 引擎 HTTPS/TLS 端到端测试（异步握手 + 证书校验硬断连 + SNI）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 运行时自签证书回环覆盖的核心不变量：
 * - https:// 请求经异步 TLS 握手（非阻塞 socket 上 WANT_* 置
 *   TLS_HANDSHAKING 等 socket 事件重入续推）后完整下载，成品一致
 * - verify_ssl=true 对自签证书必须硬失败——回归此前的 WARN-only
 *   行为（校验失败告警后照常收数据，TLS 形同虚设）
 * - SNI 随握手发出（服务器侧观测），verify_ssl=true 且信任自签 CA
 *   （SSL_CERT_FILE）+ SAN 主机名匹配时校验通过下载成功
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#define POLL(fd_ptr, count, timeout_ms) WSAPoll((fd_ptr), (count), (timeout_ms))
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>

#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "tls_cert_generator.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

#ifdef FALCON_ENABLE_OPENSSL

namespace {

#ifdef _WIN32
void ensure_winsock_for_tls_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int tls_test_getpid() { return _getpid(); }
using sock_len = int;
#else
inline int tls_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
#endif

std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

std::string read_file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

std::string temp_dir_for(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("falcon_v2_tls_") + tag + "_" +
             std::to_string(tls_test_getpid())))
        .string();
}

template <typename Pred>
bool wait_for(Pred&& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

/**
 * @brief 作用域环境变量（SSL_CERT_FILE 指定信任锚用；测试单线程
 *        顺序执行，无进程环境竞争）
 */
class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value) : name_(name) {
        const char* old = ::getenv(name);
        had_old_ = old != nullptr;
        if (had_old_) old_ = old;
#ifdef _WIN32
        _putenv_s(name, value.c_str());
#else
        ::setenv(name, value.c_str(), 1);
#endif
    }

    ~ScopedEnvVar() {
#ifdef _WIN32
        _putenv_s(name_.c_str(), had_old_ ? old_.c_str() : "");
#else
        if (had_old_) {
            ::setenv(name_.c_str(), old_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
#endif
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};


/**
 * @brief TLS 回环测试服务器
 *
 * 阻塞式 SSL_accept 处理请求（引擎侧握手由事件驱动推进，两侧时序
 * 天然兼容）；收到请求头后回 200 + Content-Length + Connection:close
 * 并直接关闭（客户端按长度判完成，无需等待 close_notify）。
 * 记录握手成功的 SNI 与连接数供断言。
 */
class TlsTestServer {
public:
    ~TlsTestServer() { stop(); }

    bool start(const std::string& key_path, const std::string& cert_path) {
#ifdef _WIN32
        ensure_winsock_for_tls_test();
#endif
        if (!falcon_test_tls::generate_self_signed_cert(key_path,
                                                    cert_path)) {
            return false;
        }

        server_ctx_ = SSL_CTX_new(TLS_server_method());
        if (!server_ctx_) {
            return false;
        }
        SSL_CTX_set_min_proto_version(server_ctx_, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_file(server_ctx_, cert_path.c_str(),
                                         SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(server_ctx_, key_path.c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(server_ctx_) != 1) {
            SSL_CTX_free(server_ctx_);
            server_ctx_ = nullptr;
            ERR_clear_error();
            return false;
        }

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            stop();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            stop();
            return false;
        }

        sockaddr_in bound{};
        sock_len len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound),
                          &len) != 0) {
            stop();
            return false;
        }
        port_ = ntohs(bound.sin_port);

        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        if (server_ctx_) {
            SSL_CTX_free(server_ctx_);
            server_ctx_ = nullptr;
            ERR_clear_error();
        }
    }

    int port() const { return port_; }

    std::string url(const std::string& host, const std::string& path) const {
        return "https://" + host + ":" + std::to_string(port_) + path;
    }

    void set_body(std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = std::move(body);
    }

    /// 握手成功时客户端报告的 SNI（无 SNI 为空串）
    std::string sni() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sni_;
    }

    int handshakes() const { return handshakes_.load(); }

private:
    void accept_loop() {
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (POLL(&pfd, 1, 500) <= 0) {
                continue;
            }
            sockaddr_in peer{};
            sock_len peer_len = sizeof(peer);
            int conn = ::accept(listen_fd_,
                                reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            // 顺序处理：本文件每个用例至多一条连接，串行无竞争
            serve_tls(conn);
        }
    }

    static void set_socket_timeout(int conn, int seconds) {
#ifdef _WIN32
        // Winsock 的 SO_RCVTIMEO/SO_SNDTIMEO 取 DWORD 毫秒而非 timeval
        // （传 timeval 会被按前 4 字节解读为 N 毫秒，兜底超时形同虚设）
        const DWORD ms = static_cast<DWORD>(seconds) * 1000u;
        (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&ms), sizeof(ms));
        (void)setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO,
                         reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = seconds;
        (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        (void)setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
    }

    void serve_tls(int conn) {
        set_socket_timeout(conn, 10);

        SSL* ssl = SSL_new(server_ctx_);
        if (!ssl) {
            CLOSE_SOCKET(conn);
            return;
        }
        SSL_set_fd(ssl, conn);

        // 阻塞握手：引擎侧异步推进，直到完成或告警中止
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            ERR_clear_error();
            return;
        }
        handshakes_.fetch_add(1);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            const char* server_name =
                SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
            sni_ = server_name != nullptr ? server_name : "";
        }

        // 读请求头
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) {
                SSL_free(ssl);
                CLOSE_SOCKET(conn);
                ERR_clear_error();
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body = body_;
        }

        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

        // 响应发完直接关闭：客户端按 Content-Length 判完成，不依赖
        // close_notify（也避免阻塞等待对端关闭）
        if (ssl_write_all(ssl, header.data(), header.size()) &&
            ssl_write_all(ssl, body.data(), body.size())) {
            served_.fetch_add(1);
        }

        SSL_free(ssl);
        CLOSE_SOCKET(conn);
    }

    static bool ssl_write_all(SSL* ssl, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            const int n = SSL_write(
                ssl, data + sent,
                static_cast<int>(size - sent > 0x7fffffff
                                     ? 0x7fffffff
                                     : size - sent));
            if (n <= 0) {
                ERR_clear_error();
                return false;
            }
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    mutable std::mutex mutex_;
    std::string body_;
    std::string sni_;

    SSL_CTX* server_ctx_ = nullptr;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> handshakes_{0};
    std::atomic<int> served_{0};  // 响应完整发出的连接数
    std::thread accept_thread_;
};

/// 引擎线程 RAII 守卫（同暂停/重定向测试模式）
class TlsEngineRunner {
public:
    explicit TlsEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~TlsEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    TlsEngineRunner(const TlsEngineRunner&) = delete;
    TlsEngineRunner& operator=(const TlsEngineRunner&) = delete;

    void shutdown_and_join() {
        engine_.shutdown();
        if (thread_.joinable()) thread_.join();
    }

private:
    DownloadEngineV2& engine_;
    std::thread thread_;
};

bool wait_group_terminal(DownloadEngineV2& engine, RequestGroup* group,
                         int timeout_ms) {
    (void)engine;
    return wait_for(
        [&] {
            const auto st = group->status();
            return st == RequestGroupStatus::COMPLETED ||
                   st == RequestGroupStatus::FAILED;
        },
        timeout_ms);
}

/// 非阻塞异步握手全链路：WANT_* 置 TLS_HANDSHAKING 等 socket 事件
/// 重入续推，完成后明文 HTTP 语义照常工作
TEST(DownloadEngineV2Tls, HttpsAsyncHandshakeDownloadSucceeds) {
    const std::string body = make_body(128 * 1024);

    const std::string dir = temp_dir_for("handshake");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = false;  // 自签证书：跳过校验

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    EXPECT_EQ(server.handshakes(), 1);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// verify_ssl=true 对自签证书必须硬失败（回归 WARN-only：旧行为
/// 校验失败仅告警即继续收数据，成品照常落盘）
TEST(DownloadEngineV2Tls, VerifySslFailsHardOnSelfSigned) {
    const std::string body = make_body(32 * 1024);

    const std::string dir = temp_dir_for("verify");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = true;  // 自签证书不在信任锚内，必须拒绝

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "自签证书必须硬失败，不得 WARN 后继续";

    runner.shutdown_and_join();
    server.stop();

    // 半成品不顶最终名
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// SNI 随握手发出 + 正向校验：信任自签 CA（SSL_CERT_FILE）且 SAN
/// 主机名匹配（SSL_set1_host 绑定 localhost）时校验通过、下载成功
TEST(DownloadEngineV2Tls, SniSentAndPositiveVerificationSucceeds) {
    const std::string body = make_body(64 * 1024);

    const std::string dir = temp_dir_for("sni");
    std::filesystem::create_directories(dir);
    const std::string key_path = (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();

    TlsTestServer server;
    ASSERT_TRUE(server.start(key_path, cert_path));
    server.set_body(body);

    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    // 客户端信任锚 = 自签证书（默认验证路径会读取 SSL_CERT_FILE）
    ScopedEnvVar trusted_ca("SSL_CERT_FILE", cert_path);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.verify_ssl = true;

    const TaskId task_id = engine.add_download(
        server.url("localhost", "/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    TlsEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    // SNI 与证书 CN/SAN 同源（SSL_set_tlsext_host_name(host_)）
    EXPECT_EQ(server.sni(), "localhost");
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

} // namespace

#else  // !FALCON_ENABLE_OPENSSL

// 无 OpenSSL 构建：TLS 不可用，本文件无测试（避免空翻译单元告警）
namespace falcon_tls_placeholder {
inline int placeholder() { return 0; }
}  // namespace falcon_tls_placeholder

#endif  // FALCON_ENABLE_OPENSSL
