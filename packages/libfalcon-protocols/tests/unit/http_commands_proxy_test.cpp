/**
 * @file http_commands_proxy_test.cpp
 * @brief V2 引擎 HTTP 代理端到端测试（absolute-form 请求行 + CONNECT
 *        隧道 + Basic 认证）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖的核心不变量：
 * - parse_http_proxy 纯函数：None / 明文 HTTP 代理（含 userinfo 凭据
 *   与缺省端口）/ socks 与 TLS 代理明确判 Unsupported（M2 适配层据此
 *   回退 V1 curl）
 * - 明文 HTTP 经代理：请求行用 absolute-form（RFC 7230 §5.3.2），
 *   Host 头保持目标主机；客户端不解析上游主机名（解析不可达域成功
 *   即证明）
 * - Proxy-Authorization: Basic 凭据精确到达代理（userinfo 优先于
 *   独立字段）
 * - HTTPS 经代理：CONNECT 隧道（authority-form target）→ 2xx 后同
 *   连接 TLS 握手（隧道内请求行回 origin-form）→ 完整下载
 * - 代理拒绝 CONNECT（403）必须干净失败，半成品不顶最终名
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
#include <arpa/inet.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <falcon/detail/injection.hpp>
#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/http_commands.hpp>

#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>

#ifndef _WIN32
#include <pthread.h>
#include <csignal>
#endif

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

namespace {

#ifdef _WIN32
void ensure_winsock_for_proxy_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int proxy_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int proxy_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
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
            (std::string("falcon_v2_proxy_") + tag + "_" +
             std::to_string(proxy_test_getpid())))
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

/// 作用域环境变量（SSL_CERT_FILE 指定信任锚；测试单线程顺序执行）
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

/// 请求报文中提取单个头值（精确头名匹配；空 = 未出现）
std::string extract_header(const std::string& request,
                           const std::string& name) {
    std::size_t pos = 0;
    while (true) {
        const auto line_end = request.find("\r\n", pos);
        if (line_end == std::string::npos) return {};
        const std::string line = request.substr(pos, line_end - pos);
        if (line.rfind(name + ":", 0) == 0) {
            std::string value = line.substr(name.size() + 1);
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
            return value;
        }
        if (line.empty()) return {};  // 头区结束
        pos = line_end + 2;
    }
}

/// 请求报文第一行（请求行）
std::string request_line_of(const std::string& request) {
    const auto eol = request.find("\r\n");
    return eol == std::string::npos ? request : request.substr(0, eol);
}

bool send_all_plain(int conn, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const recv_ssize n = ::send(conn, data + sent,
#ifdef _WIN32
                                    static_cast<int>(size - sent),
#else
                                    size - sent,
#endif
#ifdef _WIN32
                                    0);
#else
                                    MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 读到 \r\n\r\n（明文；SO_RCVTIMEO 兜底防挂死）
bool read_headers_plain(int conn, std::string& out) {
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos &&
           out.size() < 64 * 1024) {
        const recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

#ifdef FALCON_ENABLE_OPENSSL

/**
 * @brief 代理行为测试服务器
 *
 * 三种模式：
 * - kPlainProxy：明文代理假实现——记录请求行与 Proxy-Authorization
 *   后直接回 200 + body（不转发上游；验证的是经代理的请求形态与
 *   客户端不解析上游主机名）
 * - kConnectAccept：收 CONNECT 记录 authority 与认证头，回 200 后
 *   原地 SSL_accept 变身 TLS 服务器，继续完成 HTTPS 下载语义
 * - kConnectReject：收 CONNECT 后回 403 并关连接
 */
class ProxyTestServer {
public:
    enum class Mode { kPlainProxy, kConnectAccept, kConnectReject };

    ~ProxyTestServer() { stop(); }

    bool start(Mode mode) {
        mode_ = mode;
#ifdef _WIN32
        ensure_winsock_for_proxy_test();
#endif

        if (mode_ == Mode::kConnectAccept) {
            // CONNECT 接受模式需要 TLS 上下文（隧道内原地变身 TLS 服务器）
            const std::string dir = temp_dir_for("ctx");
            std::filesystem::create_directories(dir);
            key_path_ = (std::filesystem::path(dir) / "key.pem").string();
            cert_path_ = (std::filesystem::path(dir) / "cert.pem").string();
            if (!falcon_test_tls::generate_self_signed_cert(key_path_,
                                                            cert_path_)) {
                return false;
            }
            tls_ctx_ = SSL_CTX_new(TLS_server_method());
            if (!tls_ctx_) {
                return false;
            }
            SSL_CTX_set_min_proto_version(tls_ctx_, TLS1_2_VERSION);
            if (SSL_CTX_use_certificate_file(tls_ctx_, cert_path_.c_str(),
                                             SSL_FILETYPE_PEM) != 1 ||
                SSL_CTX_use_PrivateKey_file(tls_ctx_, key_path_.c_str(),
                                            SSL_FILETYPE_PEM) != 1 ||
                SSL_CTX_check_private_key(tls_ctx_) != 1) {
                SSL_CTX_free(tls_ctx_);
                tls_ctx_ = nullptr;
                ERR_clear_error();
                return false;
            }
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
        accept_thread_ = std::thread([this] {
#ifndef _WIN32
            // OpenSSL 内部写（SSL_accept 失败的 fatal alert）不经
            // MSG_NOSIGNAL：客户端提前关闭时 EPIPE 以 SIGPIPE 杀死
            // 整个测试进程——accept 线程屏蔽之（与 tls_loopback_server
            // 同款防护）
            sigset_t sigpipe_set;
            sigemptyset(&sigpipe_set);
            sigaddset(&sigpipe_set, SIGPIPE);
            pthread_sigmask(SIG_BLOCK, &sigpipe_set, nullptr);
#endif
            accept_loop();
        });
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
        if (tls_ctx_) {
            SSL_CTX_free(tls_ctx_);
            tls_ctx_ = nullptr;
            ERR_clear_error();
        }
    }

    int port() const { return port_; }

    /// CONNECT 接受模式下生成的证书 PEM 路径（正向校验的信任锚）
    std::string cert_path() const { return cert_path_; }

    std::string proxy_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    void set_body(std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = std::move(body);
    }

    /// CONNECT 应答分片发送：先给半行，留出客户端 would-block 重入
    /// 窗口后再补发余下部分（验证半包重组）
    void set_split_connect_response(bool split) {
        split_connect_response_ = split;
    }

    std::string last_request_line() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_request_line_;
    }

    std::string last_proxy_auth() {
        std::lock_guard<std::mutex> lock(mutex_);
        return last_proxy_auth_;
    }

    std::string connect_authority() {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_authority_;
    }

    std::string tunnel_request_line() {
        std::lock_guard<std::mutex> lock(mutex_);
        return tunnel_request_line_;
    }

    int connect_count() const { return connect_count_.load(); }

    /// CONNECT 应答延迟（毫秒）：读到 CONNECT 请求后等这么久再发
    /// established——给客户端侧留出「挂起等应答」的稳定注入窗口
    void set_connect_reply_delay(int ms) {
        std::lock_guard<std::mutex> lock(mutex_);
        connect_reply_delay_ms_ = ms;
    }

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
            // 每用例至多一条连接，串行处理无竞争
            serve(conn);
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

    void serve(int conn) {
        set_socket_timeout(conn, 10);

        std::string request;
        if (!read_headers_plain(conn, request)) {
            CLOSE_SOCKET(conn);
            return;
        }

        if (mode_ == Mode::kPlainProxy) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                last_request_line_ = request_line_of(request);
                last_proxy_auth_ = extract_header(request, "Proxy-Authorization");
            }
            std::string body;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                body = body_;
            }
            const std::string header =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: application/octet-stream\r\n"
                "Connection: close\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            (void)send_all_plain(conn, header.data(), header.size());
            (void)send_all_plain(conn, body.data(), body.size());
            CLOSE_SOCKET(conn);
            return;
        }

        // CONNECT 模式：请求行 = "CONNECT authority HTTP/1.1"
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connect_count_.fetch_add(1);
            const std::string line = request_line_of(request);
            const auto sp1 = line.find(' ');
            const auto sp2 = line.find(' ', sp1 == std::string::npos ? 0 : sp1 + 1);
            connect_authority_ =
                (sp1 == std::string::npos || sp2 == std::string::npos)
                    ? ""
                    : line.substr(sp1 + 1, sp2 - sp1 - 1);
            last_proxy_auth_ = extract_header(request, "Proxy-Authorization");
        }

        if (connect_reply_delay_ms_ > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(connect_reply_delay_ms_));
        }

        if (mode_ == Mode::kConnectReject) {
            static const char kForbidden[] =
                "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
            (void)send_all_plain(conn, kForbidden, sizeof(kForbidden) - 1);
            CLOSE_SOCKET(conn);
            return;
        }

        static const char kEstablished[] =
            "HTTP/1.1 200 Connection established\r\n\r\n";
        bool established_sent = true;
        if (split_connect_response_) {
            const std::string first = std::string(kEstablished).substr(0, 15);
            const std::string rest = std::string(kEstablished).substr(15);
            established_sent =
                send_all_plain(conn, first.data(), first.size()) &&
                (std::this_thread::sleep_for(std::chrono::milliseconds(120)),
                 send_all_plain(conn, rest.data(), rest.size()));
        } else {
            established_sent = send_all_plain(conn, kEstablished,
                                              sizeof(kEstablished) - 1);
        }
        if (!established_sent) {
            CLOSE_SOCKET(conn);
            return;
        }

        serve_tls_in_tunnel(conn);
    }

    /// 隧道建立后原地变身 TLS 服务器（同一连接、同一 fd）
    void serve_tls_in_tunnel(int conn) {
        SSL* ssl = SSL_new(tls_ctx_);
        if (!ssl) {
            CLOSE_SOCKET(conn);
            return;
        }
        SSL_set_fd(ssl, conn);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            ERR_clear_error();
            return;
        }

        // 读隧道内请求头（origin-form）
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
            tunnel_request_line_ = request_line_of(request);
            body = body_;
        }

        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

        auto ssl_send_all = [ssl](const std::string& data) {
            std::size_t sent = 0;
            while (sent < data.size()) {
                const int n = SSL_write(ssl, data.data() + sent,
                                        static_cast<int>(data.size() - sent));
                if (n <= 0) {
                    ERR_clear_error();
                    return false;
                }
                sent += static_cast<std::size_t>(n);
            }
            return true;
        };
        const bool ok = ssl_send_all(header) && ssl_send_all(body);
        (void)ok;

        // 发完直接关闭：客户端按 Content-Length 判完成，不等待
        // close_notify
        SSL_free(ssl);
        CLOSE_SOCKET(conn);
        ERR_clear_error();
    }

    Mode mode_ = Mode::kPlainProxy;
    bool split_connect_response_ = false;
    int connect_reply_delay_ms_ = 0;

    mutable std::mutex mutex_;
    std::string body_;
    std::string last_request_line_;
    std::string last_proxy_auth_;
    std::string connect_authority_;
    std::string tunnel_request_line_;

    SSL_CTX* tls_ctx_ = nullptr;
    std::string key_path_;
    std::string cert_path_;

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> connect_count_{0};
    std::thread accept_thread_;
};

/// 引擎线程 RAII 守卫（同 TLS/重定向测试模式）
class ProxyEngineRunner {
public:
    explicit ProxyEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~ProxyEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    ProxyEngineRunner(const ProxyEngineRunner&) = delete;
    ProxyEngineRunner& operator=(const ProxyEngineRunner&) = delete;

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

DownloadOptions base_proxy_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    return options;
}

#endif  // FALCON_ENABLE_OPENSSL

//==============================================================================
// parse_http_proxy 纯函数（不依赖 OpenSSL，全配置可测）
//==============================================================================

TEST(ParseHttpProxy, EmptyConfigMeansNone) {
    DownloadOptions options;
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::None);
}

TEST(ParseHttpProxy, HttpUrlWithHostAndPort) {
    DownloadOptions options;
    options.proxy = "http://proxy.local:8080";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.host, "proxy.local");
    EXPECT_EQ(cfg.port, 8080);
    EXPECT_TRUE(cfg.username.empty());
}

TEST(ParseHttpProxy, DefaultPortIs80) {
    DownloadOptions options;
    options.proxy = "http://proxy.local";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.host, "proxy.local");
    EXPECT_EQ(cfg.port, 80);
}

TEST(ParseHttpProxy, SchemelessHostTreatedAsHttpProxy) {
    DownloadOptions options;
    options.proxy = "10.0.0.2:3128";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.host, "10.0.0.2");
    EXPECT_EQ(cfg.port, 3128);
}

TEST(ParseHttpProxy, UserinfoCredentialsWinOverFields) {
    DownloadOptions options;
    options.proxy = "http://alice:s3cret@proxy.local:8080";
    options.proxy_username = "ignored";
    options.proxy_password = "ignored";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.username, "alice");
    EXPECT_EQ(cfg.password, "s3cret");
}

TEST(ParseHttpProxy, CredentialFieldsUsedWithoutUserinfo) {
    DownloadOptions options;
    options.proxy = "http://proxy.local:8080";
    options.proxy_username = "bob";
    options.proxy_password = "pw";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.username, "bob");
    EXPECT_EQ(cfg.password, "pw");
}

TEST(ParseHttpProxy, SocksIsUnsupported) {
    for (const char* raw :
         {"socks5://proxy:1080", "socks4://proxy:1080", "socks://proxy:1080"}) {
        DownloadOptions options;
        options.proxy = raw;
        EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported)
            << raw;
    }
}

TEST(ParseHttpProxy, HttpsProxyIsUnsupported) {
    DownloadOptions options;
    options.proxy = "https://proxy.local:443";
    EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported);
}

TEST(ParseHttpProxy, ProxyTypeSocksFieldIsUnsupported) {
    DownloadOptions options;
    options.proxy = "http://proxy.local:8080";
    options.proxy_type = "SOCKS5";
    EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported);
}

TEST(ParseHttpProxy, UnknownSchemeIsUnsupported) {
    DownloadOptions options;
    options.proxy = "ftp://proxy.local:21";
    EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported);
}

TEST(ParseHttpProxy, InvalidPortIsUnsupported) {
    for (const char* raw :
         {"http://proxy.local:0", "http://proxy.local:99999",
          "http://proxy.local:abc", "http://proxy.local:"}) {
        DownloadOptions options;
        options.proxy = raw;
        EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported)
            << raw;
    }
}

/// 剥路径前的前导斜杠（"http:///path"：空 authority + 绝对路径）
/// 不能被当成合法 host，判 Unsupported
TEST(ParseHttpProxy, LeadingSlashInsteadOfHostIsUnsupported) {
    DownloadOptions options;
    options.proxy = "http:///path";
    EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported);
}

/// IPv6 字面量代理：V2 数据面为 AF_INET，明确判 Unsupported（不静默
/// 截断 "[::1]" 之类畸形 authority）
TEST(ParseHttpProxy, Ipv6LiteralAuthorityIsUnsupported) {
    for (const char* raw :
         {"http://[::1]:8080", "http://[2001:db8::1]:3128", "[::1]:3128"}) {
        DownloadOptions options;
        options.proxy = raw;
        EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported)
            << raw;
    }
}

/// userinfo 无冒号（仅用户名）：密码回落独立凭据字段
TEST(ParseHttpProxy, UserinfoWithoutColonFallsBackToPasswordField) {
    DownloadOptions options;
    options.proxy = "http://alice@proxy.local:8080";
    options.proxy_username = "ignored";
    options.proxy_password = "fieldpw";
    const auto cfg = parse_http_proxy(options);
    ASSERT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.username, "alice");
    EXPECT_EQ(cfg.password, "fieldpw");
}

/// userinfo 用户名为空（":pass@"）：用户名回落独立凭据字段
TEST(ParseHttpProxy, EmptyUsernameInUserinfoFallsBackToUsernameField) {
    DownloadOptions options;
    options.proxy = "http://:secretpw@proxy.local:8080";
    options.proxy_username = "fielduser";
    options.proxy_password = "ignored";
    const auto cfg = parse_http_proxy(options);
    ASSERT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.username, "fielduser");
    EXPECT_EQ(cfg.password, "secretpw");
}

/// userinfo 之后紧跟路径（无 authority）：同前导斜杠判 Unsupported
TEST(ParseHttpProxy, UserinfoWithLeadingSlashPathIsUnsupported) {
    DownloadOptions options;
    options.proxy = "http://user@/path";
    EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported);
}

/// 6 位端口文本（超出 5 字符长度上限）：与 5 位超范围值（99999）不同
/// 的拒绝方向
TEST(ParseHttpProxy, OverlongPortTextIsUnsupported) {
    DownloadOptions options;
    options.proxy = "http://proxy.local:123456";
    EXPECT_EQ(parse_http_proxy(options).kind, HttpProxyKind::Unsupported);
}

/// 端口合法上界 65535 被接受
TEST(ParseHttpProxy, PortMaxValue65535Accepted) {
    DownloadOptions options;
    options.proxy = "http://proxy.local:65535";
    const auto cfg = parse_http_proxy(options);
    ASSERT_EQ(cfg.kind, HttpProxyKind::HttpProxy);
    EXPECT_EQ(cfg.port, 65535);
}

//==============================================================================
// 明文 HTTP 代理：absolute-form 请求行 + Basic 认证
//==============================================================================

#ifdef FALCON_ENABLE_OPENSSL

/// 明文 HTTP 经代理：absolute-form 请求行 + Host 头保持目标主机；
/// 上游域名不可解析（.invalid 保留域）而任务成功 = 客户端从未尝试
/// 解析上游，全凭代理转发
TEST(DownloadEngineV2Proxy, PlainProxyUsesAbsoluteFormRequestLine) {
    const std::string body = make_body(32 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kPlainProxy));
    server.set_body(body);

    const std::string dir = temp_dir_for("plain");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();

    // RFC 6761 保留域：可解析性由代理负责，客户端不得触碰
    const TaskId task_id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.last_request_line(),
              "GET http://upstream.invalid:81/f.bin HTTP/1.1");
    // Host 头为 origin-form 语义（目标主机），不含代理信息
    // （由 absolute-form 请求行承载路由）；无凭据时不得出现认证头
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// Proxy-Authorization: Basic 凭据精确到达代理（userinfo 形态）
TEST(DownloadEngineV2Proxy, PlainProxySendsBasicAuthFromUserinfo) {
    const std::string body = make_body(16 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kPlainProxy));
    server.set_body(body);

    const std::string dir = temp_dir_for("auth");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    // base64("alice:secret") == "YWxpY2U6c2VjcmV0"（RFC 4648 向量，
    // 独立于实现预计算）
    options.proxy = "http://alice:secret@127.0.0.1:" +
                    std::to_string(server.port());

    const TaskId task_id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.last_proxy_auth(), "Basic YWxpY2U6c2VjcmV0");
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// IPv6 字面量目标经明文代理：absolute-form 请求行保留方括号形态
/// （RFC 3986 §3.2.2——authority 中的 v6 字面量不因代理转发丢失括号）
TEST(DownloadEngineV2Proxy, Ipv6TargetViaPlainProxyKeepsBrackets) {
    const std::string body = make_body(16 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kPlainProxy));
    server.set_body(body);

    const std::string dir = temp_dir_for("v6_target");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();

    const TaskId task_id = engine.add_download(
        "http://[::1]:81/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.last_request_line(),
              "GET http://[::1]:81/f.bin HTTP/1.1");
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// HTTPS 经代理：CONNECT 隧道 → 同连接 TLS 切换
//==============================================================================

/// CONNECT 隧道建立（authority-form target + 认证头）后同一连接
/// 完成 TLS 握手与下载；隧道内请求行回 origin-form，SNI/证书校验
/// 按目标主机照常生效
TEST(DownloadEngineV2Proxy, ConnectTunnelThenTlsDownloadSucceeds) {
    const std::string body = make_body(64 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));
    server.set_body(body);

    const std::string dir = temp_dir_for("connect");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    // 客户端信任锚 = 服务器自签证书（SAN DNS:localhost 匹配目标主机）
    ScopedEnvVar trusted_ca("SSL_CERT_FILE", server.cert_path());

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = "http://carol:pw@127.0.0.1:" + std::to_string(server.port());
    options.verify_ssl = true;

    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << "CONNECT 隧道 + 同连接 TLS 握手必须端到端成功";

    runner.shutdown_and_join();
    server.stop();

    // CONNECT 的 authority-form target（RFC 7231 §4.3.6）+ 隧道凭据
    EXPECT_EQ(server.connect_authority(),
              "localhost:" + std::to_string(server.port()));
    EXPECT_EQ(server.connect_count(), 1);
    // 隧道内 HTTP 请求回 origin-form（代理不见内部请求细节）
    EXPECT_EQ(server.tunnel_request_line(), "GET /f.bin HTTP/1.1");
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// CONNECT 发送 WANT_WRITE（模拟内核发送缓冲满——回环不可自然构造）：
/// 注入置位期间 CONNECT 发送被拦挂起（代理侧 CONNECT 计数保持 0 即
/// 注入生效铁证），清注入后 socket 恒可写唤醒重入续推，隧道建立后
/// TLS 下载自然完成（进行中语义非失败，恰一次 CONNECT 无重连）
TEST(DownloadEngineV2Proxy, ConnectSendWantWriteSuspendsThenRecovers) {
    const std::string body = make_body(32 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));
    server.set_body(body);

    const std::string dir = temp_dir_for("connww");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    // 客户端信任锚 = 服务器自签证书（同隧道用例）
    ScopedEnvVar trusted_ca("SSL_CERT_FILE", server.cert_path());

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();
    options.verify_ssl = true;

    // 注入先于任务创建置位：CONNECT 发送必然被拦
    ::falcon::detail::set_injection(
        ::falcon::detail::InjectPoint::HttpSendWantWrite, true);
    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    // 回环连接毫秒级完成，CONNECTING→CONNECT 发送在下一轮 poll 唤醒
    // 即发生；静置窗口保证首个 CONNECT send 已被拦下
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(server.connect_count(), 0)
        << "注入置位期间 CONNECT 必须被拦下（WANT_WRITE 挂起中）";
    ::falcon::detail::set_injection(
        ::falcon::detail::InjectPoint::HttpSendWantWrite, false);

    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.connect_count(), 1);
    EXPECT_EQ(server.connect_authority(),
              "localhost:" + std::to_string(server.port()));
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 代理路径的事件注册失败（fd 上限/ENOMEM）：命令不得带着
/// socket_wait_map_ 条目挂起等永远不会来的唤醒——注册失败立即按
/// 连接失败收口。注入全局生效，可达注册点有两个（服务器 accept 时序
/// 决定命中哪个，均为有效覆盖）：connect in-progress 等待注册点
/// （EINPROGRESS 形态）与 CONNECT 发完后的应答等待注册点（connect
/// 立即完成形态）；两者都走「注册失败 → 连接失败收口」
TEST(DownloadEngineV2Proxy, ConnectPathRegistrationFailFailsCleanly) {
    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));

    const std::string dir = temp_dir_for("connreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();
    options.verify_ssl = true;

    {
        ::falcon::detail::ScopedInjection add_fail(
            ::falcon::detail::InjectPoint::EventPollAddFail);
        const TaskId task_id = engine.add_download(
            "https://localhost:" + std::to_string(server.port()) + "/f.bin",
            options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        ProxyEngineRunner runner(engine);
        ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// CONNECT 发送 WANT_WRITE 挂起轮的事件注册失败：HttpSendWantWrite
// 先拦下 CONNECT（挂起等可写），挂起稳定窗内补挂 EventPollAddFail
// ——可写事件唤醒后 send 仍报 EAGAIN，重注册命中注入按连接失败
// 收口（与 ConnectPathRegistrationFailFailsCleanly 的 connect 注册
// 形态区分：本用例锚定 send 挂起轮的注册点）
TEST(DownloadEngineV2Proxy, ConnectSendWantWriteRegistrationFailFailsCleanly) {
    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));

    const std::string dir = temp_dir_for("connsendreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();
    options.verify_ssl = true;

    {
        ::falcon::detail::ScopedInjection send_ww(
            ::falcon::detail::InjectPoint::HttpSendWantWrite);
        const TaskId task_id = engine.add_download(
            "https://localhost:" + std::to_string(server.port()) + "/f.bin",
            options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        ProxyEngineRunner runner(engine);
        // 锚点：CONNECT 被拦（connect_count 保持 0 = 请求未上线），
        // 静置窗口保证首个 send 的 WRITE 注册已成功、命令挂起中
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(server.connect_count(), 0)
            << "注入置位期间 CONNECT 必须被拦下（WANT_WRITE 挂起中）";
        ::falcon::detail::ScopedInjection add_fail(
            ::falcon::detail::InjectPoint::EventPollAddFail);

        ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// CONNECT 发完后等代理应答的注册失败：HttpSendWantWrite 拦下首个
// send 产生挂起稳定窗，窗内补挂 EventPollAddFail 后放行——send 续
// 推完成，紧随的应答等待注册命中注入收口。应答延迟 2s 保证收口
// （若发生）先于代理应答，失败不可能是后续阶段冒名
TEST(DownloadEngineV2Proxy, ConnectResponseWaitRegistrationFailFailsCleanly) {
    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));
    server.set_connect_reply_delay(2000);

    const std::string dir = temp_dir_for("connwaitreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();
    options.verify_ssl = true;

    {
        ::falcon::detail::ScopedInjection send_ww(
            ::falcon::detail::InjectPoint::HttpSendWantWrite);
        const TaskId task_id = engine.add_download(
            "https://localhost:" + std::to_string(server.port()) + "/f.bin",
            options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        ProxyEngineRunner runner(engine);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        EXPECT_EQ(server.connect_count(), 0)
            << "首个 send 必须被拦下（挂起稳定窗）";
        // 先置注册失败再放行 send：续推完成后的应答等待注册必然命中
        ::falcon::detail::ScopedInjection add_fail(
            ::falcon::detail::InjectPoint::EventPollAddFail);
        ::falcon::detail::set_injection(::falcon::detail::InjectPoint::HttpSendWantWrite, false);

        ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

// 代理应答 recv 的 would-block 重注册失败：应答延迟 + 分片形态让
// 客户端收下前缀后再次 recv 得 EAGAIN——重注册命中注入收口。置位
// 早于应答（300ms < 800ms），首个应答等待注册发生在置位前不命中
TEST(DownloadEngineV2Proxy, ConnectResponseRecvRegistrationFailFailsCleanly) {
    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));
    server.set_connect_reply_delay(800);
    server.set_split_connect_response(true);

    const std::string dir = temp_dir_for("connrecvreg");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();
    options.verify_ssl = true;

    {
        const TaskId task_id = engine.add_download(
            "https://localhost:" + std::to_string(server.port()) + "/f.bin",
            options);
        ASSERT_GT(task_id, 0u);
        auto* group = engine.request_group_man()->find_group(task_id);
        ASSERT_NE(group, nullptr);

        ProxyEngineRunner runner(engine);
        // 锚点：CONNECT 已上线（connect_count==1）且应答等待注册已
        // 成功（发生在置位前）；命令挂起等应答中
        ASSERT_TRUE(wait_for([&] { return server.connect_count() >= 1; },
                             10000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        ::falcon::detail::ScopedInjection add_fail(
            ::falcon::detail::InjectPoint::EventPollAddFail);

        ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
        ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
        runner.shutdown_and_join();
    }
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 代理拒绝 CONNECT（403）：干净失败收口，半成品不顶最终名
TEST(DownloadEngineV2Proxy, ConnectRefusedFailsCleanly) {
    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectReject));

    const std::string dir = temp_dir_for("refuse");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();

    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "代理拒绝 CONNECT 必须干净失败";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.connect_count(), 1);
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// CONNECT 认证头的 base64 填充分支：凭据 "user:pw" 共 7 字节（非 3
/// 的倍数），编码必须带 "==" 填充（RFC 4648 标准向量：dXNlcjpwdw==）。
/// 既有用例的凭据都恰好是 3 的倍数，padding 分支从未被执行过
TEST(DownloadEngineV2Proxy, ConnectAuthBase64PaddingForShortCredentials) {
    const std::string body = make_body(16 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));
    server.set_body(body);

    const std::string dir = temp_dir_for("padding");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = "http://user:pw@127.0.0.1:" + std::to_string(server.port());
    options.verify_ssl = false;  // 自签证书：本用例焦点在凭据形态

    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    // 7 字节凭据 → 3×2 循环 + 1 字节余数（"==" 填充分支）
    EXPECT_EQ(server.last_proxy_auth(), "Basic dXNlcjpwdw==");
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// CONNECT 应答跨 TCP 分片：代理先发半行应答再补余下部分。客户端
/// 头缓冲不完整时必须注册 READ 挂起（would-block 重入），补发到达
/// 后重组出完整应答继续隧道，半包绝不丢失或错位
TEST(DownloadEngineV2Proxy, ConnectResponseSplitAcrossSegmentsSucceeds) {
    const std::string body = make_body(48 * 1024);

    ProxyTestServer server;
    ASSERT_TRUE(server.start(ProxyTestServer::Mode::kConnectAccept));
    server.set_body(body);
    server.set_split_connect_response(true);

    const std::string dir = temp_dir_for("split");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = server.proxy_url();
    options.verify_ssl = false;

    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << "CONNECT 应答半包必须被完整重组";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.connect_count(), 1);
    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

#endif  // FALCON_ENABLE_OPENSSL

/// CONNECT 发送硬错误（ECONNRESET 注入）：代理 accept 前客户端 send
/// 失败，走「Failed to send CONNECT to proxy」干净收口——组 FAILED、
/// 错误消息指向 CONNECT。回环上 RST 抢在客户端首个 send 之前到达是
/// 亚毫秒竞态，注入点确定性命中
TEST(DownloadEngineV2Proxy, ConnectSendHardErrorFailsCleanly) {
#ifdef _WIN32
    // 本用例不经 ProxyTestServer（裸 socket 前无任何 start()），
    // Winsock 必须显式初始化，否则 socket() 恒返 INVALID_SOCKET
    ensure_winsock_for_proxy_test();
#endif
    // 纯监听 socket：连接在 accept 队列完成握手即可，无需 accept——
    // 客户端 send 注入失败发生在任何代理应答之前
    const int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listen_fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)), 0);
    ASSERT_EQ(::listen(listen_fd, 4), 0);
    sockaddr_in bound{};
    sock_len len = sizeof(bound);
    ASSERT_EQ(::getsockname(listen_fd, reinterpret_cast<sockaddr*>(&bound),
                            &len), 0);
    const int proxy_port = ntohs(bound.sin_port);
    struct ListenGuard {
        int fd;
        ~ListenGuard() { CLOSE_SOCKET(fd); }
    } listen_guard{listen_fd};

    const std::string dir = temp_dir_for("connfail");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_proxy_options(out_path);
    options.proxy = "http://127.0.0.1:" + std::to_string(proxy_port);

    // https 目标：CONNECT 隧道只用于 HTTPS 经代理（明文 HTTP 走
    // absolute-form 直发请求，不经 send_proxy_connect，注入点不可达）
    const TaskId task_id = engine.add_download(
        "https://upstream.invalid/x.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    ::falcon::detail::ScopedInjection injection(
        ::falcon::detail::InjectPoint::ProxyConnectSendFail);

    ProxyEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 10000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("CONNECT"), std::string::npos)
        << "错误消息应指向 CONNECT 发送失败";

    runner.shutdown_and_join();

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

} // namespace
