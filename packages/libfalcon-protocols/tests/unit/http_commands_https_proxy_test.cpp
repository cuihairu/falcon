/**
 * @file http_commands_https_proxy_test.cpp
 * @brief V2 引擎 HTTPS 代理端到端测试（代理通道 TLS + CONNECT 隧道 +
 *        TLS-in-TLS 双层加密）
 * @author Falcon Team
 * @date 2026-11-26
 *
 * 覆盖的核心不变量：
 * - parse_http_proxy：https:// 代理缺省端口 443
 * - 代理通道 TLS 握手（verify_ssl 时校验代理证书）；CONNECT 携带
 *   Proxy-Authorization: Basic（凭据只交代理，隧道内请求不携带——
 *   不向目标泄漏）
 * - TLS-in-TLS：https 目标经 HTTPS 代理，origin TLS 密文装在代理
 *   TLS 通道里双层封装，成品逐字节一致
 * - 明文目标经 HTTPS 代理：absolute-form 请求行在代理 TLS 通道内
 *   原样上线
 * - CONNECT 拒绝 / 代理证书校验失败均干净失败，成品不顶最终名
 */

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <csignal>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/http_commands.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

#include "tls_cert_generator.hpp"

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_https_proxy_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int https_proxy_test_getpid() { return _getpid(); }
using sock_len = int;
#else
inline int https_proxy_test_getpid() {
    return static_cast<int>(::getpid());
}
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
            (std::string("falcon_v2_https_proxy_") + tag + "_" +
             std::to_string(https_proxy_test_getpid())))
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

void set_socket_timeout(int conn, int seconds) {
#ifdef _WIN32
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

/// RFC 4648 标准 base64（CONNECT Basic 凭据的期望值独立生成）
std::string base64_encode(const std::string& raw) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((raw.size() + 2) / 3 * 4);
    std::size_t i = 0;
    while (i + 3 <= raw.size()) {
        const uint32_t v = (static_cast<unsigned char>(raw[i]) << 16) |
                           (static_cast<unsigned char>(raw[i + 1]) << 8) |
                           static_cast<unsigned char>(raw[i + 2]);
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >> 6) & 0x3F];
        out += kTable[v & 0x3F];
        i += 3;
    }
    const std::size_t rem = raw.size() - i;
    if (rem == 1) {
        const uint32_t v = static_cast<unsigned char>(raw[i]) << 16;
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += "==";
    } else if (rem == 2) {
        const uint32_t v = (static_cast<unsigned char>(raw[i]) << 16) |
                           (static_cast<unsigned char>(raw[i + 1]) << 8);
        out += kTable[(v >> 18) & 0x3F];
        out += kTable[(v >> 12) & 0x3F];
        out += kTable[(v >> 6) & 0x3F];
        out += "=";
    }
    return out;
}

bool ssl_write_all(SSL* ssl, const char* data, std::size_t size) {
    while (size > 0) {
        const int n = SSL_write(ssl, data, static_cast<int>(size));
        if (n <= 0) return false;
        data += n;
        size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool ssl_read_http_headers(SSL* ssl, std::string& out) {
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos &&
           out.size() < 64 * 1024) {
        const int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) return false;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

/// 大小写不敏感取请求头值（RFC 9110 §5.1：头名不区分大小写）
std::string header_value(const std::string& request,
                         const std::string& lower_name) {
    std::size_t pos = 0;
    while (true) {
        const auto line_start =
            pos == 0 ? 0 : request.find("\r\n", pos - 2) + 2;
        if (line_start == std::string::npos ||
            line_start >= request.size()) {
            break;
        }
        const auto line_end = request.find("\r\n", line_start);
        if (line_end == std::string::npos) break;
        const std::string line =
            request.substr(line_start, line_end - line_start);
        pos = line_end + 2;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name = line.substr(0, colon);
        for (char& c : name) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (name != lower_name) continue;
        std::string value = line.substr(colon + 1);
        // 头值 trim（OWS）
        const auto first = value.find_first_not_of(" \t");
        if (first == std::string::npos) return {};
        const auto last = value.find_last_not_of(" \t");
        return value.substr(first, last - first + 1);
    }
    return {};
}

std::string first_line(const std::string& request) {
    const auto eol = request.find("\r\n");
    return eol == std::string::npos ? request : request.substr(0, eol);
}

/**
 * @brief HTTPS 代理回环服务器：代理通道 TLS（自签证书）→ 读 CONNECT →
 *        回 200 → 隧道内要么直接走明文 HTTP（记录 absolute-form 请求
 *        行），要么模拟目标源站再做一层 TLS 握手（TLS-in-TLS）
 *
 * 单连接串行处理（Socks5TestServer 同款）；证书一把两用——代理与
 * 目标共用同一 SSL_CTX（SAN 覆盖 DNS:localhost + IP:127.0.0.1），
 * SSL_CERT_FILE 指向该证书即两层握手验签全过。
 */
class HttpsProxyTestServer {
public:
    ~HttpsProxyTestServer() { stop(); }

    bool start() {
#ifdef _WIN32
        ensure_winsock_for_https_proxy_test();
#else
        // 服务器存在的意义就是被客户端 abort（验证失败/CONNECT 拒绝
        // 后客户端关连接）——进程级 SIGPIPE 免疫（既有先例同法）
        signal(SIGPIPE, SIG_IGN);
#endif
        cert_dir_ = temp_dir_for("cert");
        std::filesystem::create_directories(cert_dir_);
        cert_path_ = (std::filesystem::path(cert_dir_) /
                      "localhost.pem").string();
        key_path_ = (std::filesystem::path(cert_dir_) /
                     "localhost-key.pem").string();
        if (!falcon_test_tls::generate_self_signed_cert(key_path_,
                                                        cert_path_)) {
            return false;
        }

        ctx_ = SSL_CTX_new(TLS_server_method());
        if (!ctx_) return false;
        SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_file(ctx_, cert_path_.c_str(),
                                         SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx_, key_path_.c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx_) != 1) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
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
        sock_len len = sizeof(addr);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                          &len) != 0) {
            stop();
            return false;
        }
        port_ = ntohs(addr.sin_port);
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        if (running_.exchange(false)) {
#ifdef _WIN32
            ::shutdown(listen_fd_, SD_BOTH);
#else
            ::shutdown(listen_fd_, SHUT_RDWR);
#endif
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        if (ctx_) {
            SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
        std::error_code ec;
        std::filesystem::remove_all(cert_dir_, ec);
    }

    std::string proxy_url() const {
        return "https://127.0.0.1:" + std::to_string(port_);
    }

    int port() const { return port_; }
    const std::string& cert_path() const { return cert_path_; }

    void set_body(std::string body) { body_ = std::move(body); }
    void set_tunnel_tls(bool on) { tunnel_tls_ = on; }
    void set_reject_connect(bool on) { reject_connect_ = on; }

    /// 代理通道 SSL_accept 尝试数（客户端验签失败也会计入）
    int handshake_attempts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handshake_attempts_;
    }
    /// 代理通道完成的握手数
    int handshakes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return handshakes_;
    }
    /// 隧道内目标 TLS 完成的握手数（TLS-in-TLS 才非零）
    int target_handshakes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return target_handshakes_;
    }
    std::string connect_request_line() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_request_line_;
    }
    std::string connect_proxy_auth() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_proxy_auth_;
    }
    std::string http_request_line() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return http_request_line_;
    }
    /// 隧道内完整请求（凭据不泄漏断言用）
    std::string http_request_full() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return http_request_full_;
    }

private:
    void accept_loop() {
        while (running_) {
            const int conn = ::accept(listen_fd_, nullptr, nullptr);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            handle_connection(conn);
        }
    }

    bool flush_wbio(SSL* target, BIO* wbio, SSL* proxy) {
        const int pending = BIO_pending(wbio);
        if (pending <= 0) return true;
        std::string buf(static_cast<std::size_t>(pending), '\0');
        const int got = BIO_read(wbio, buf.data(), pending);
        if (got <= 0) return false;
        buf.resize(static_cast<std::size_t>(got));
        return ssl_write_all(proxy, buf.data(), buf.size());
    }

    /// 从代理通道读一段密文喂进目标 rbio（阻塞：目标 TLS 的输入只能
    /// 来自客户端经隧道发来的密文）
    bool feed_rbio(BIO* rbio, SSL* proxy) {
        char buf[16384];
        const int n = SSL_read(proxy, buf, sizeof(buf));
        if (n <= 0) return false;
        return BIO_write(rbio, buf, n) > 0;
    }

    bool tunnel_handshake(SSL* target, BIO* rbio, BIO* wbio, SSL* proxy) {
        for (;;) {
            if (SSL_accept(target) == 1) {
                return flush_wbio(target, wbio, proxy);
            }
            const int err = SSL_get_error(target, -1);
            if (err == SSL_ERROR_WANT_READ) {
                // TLS 1.3 握手中途（如已消费 ClientHello）常伴有已产生
                // 但未冲刷的 wbio 输出（ServerHello flight）——先冲刷再
                // 阻塞读，否则两侧互等（客户端等 ServerHello、本端等
                // 客户端 Finished）构成死锁
                if (!flush_wbio(target, wbio, proxy)) return false;
                if (!feed_rbio(rbio, proxy)) return false;
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (!flush_wbio(target, wbio, proxy)) return false;
                continue;
            }
            ERR_clear_error();
            return false;
        }
    }

    bool tunnel_write(SSL* target, BIO* wbio, SSL* proxy,
                      const std::string& data) {
        const char* p = data.data();
        std::size_t left = data.size();
        while (left > 0) {
            const int n = SSL_write(target, p, static_cast<int>(left));
            if (n <= 0) {
                const int err = SSL_get_error(target, -1);
                if (err == SSL_ERROR_WANT_WRITE) {
                    if (!flush_wbio(target, wbio, proxy)) return false;
                    continue;
                }
                ERR_clear_error();
                return false;
            }
            p += n;
            left -= static_cast<std::size_t>(n);
            if (!flush_wbio(target, wbio, proxy)) return false;
        }
        return true;
    }

    /// 隧道内读 HTTP 头（目标 TLS 视角；rbio 空则从代理通道取密文）
    bool tunnel_read_headers(SSL* target, BIO* rbio, BIO* wbio, SSL* proxy,
                             std::string& out) {
        char buf[4096];
        while (out.find("\r\n\r\n") == std::string::npos &&
               out.size() < 64 * 1024) {
            const int n = SSL_read(target, buf, sizeof(buf));
            if (n > 0) {
                out.append(buf, static_cast<std::size_t>(n));
                continue;
            }
            const int err = SSL_get_error(target, -1);
            if (err == SSL_ERROR_WANT_READ) {
                // 与 tunnel_handshake 同理：读前先冲刷 wbio（TLS 1.3
                // 会话票据等输出可能在读侧仍待发送）
                if (!flush_wbio(target, wbio, proxy)) return false;
                if (!feed_rbio(rbio, proxy)) return false;
                continue;
            }
            if (err == SSL_ERROR_WANT_WRITE) {
                if (!flush_wbio(target, wbio, proxy)) return false;
                continue;
            }
            ERR_clear_error();
            return false;
        }
        return true;
    }

    void handle_connection(int conn) {
        set_socket_timeout(conn, 30);
        SSL* ssl = SSL_new(ctx_);
        if (!ssl) {
            CLOSE_SOCKET(conn);
            return;
        }
        SSL_set_fd(ssl, conn);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++handshake_attempts_;
        }
        if (SSL_accept(ssl) != 1) {
            ERR_clear_error();
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++handshakes_;
        }

        // 首请求分流：CONNECT = 隧道流（https 目标经 CONNECT 交割凭据）；
        // 其余 = 明文目标的 absolute-form GET 直发（生产对明文目标不发
        // CONNECT，代理 TLS 握手完成后直接上线该请求）
        std::string request;
        if (!ssl_read_http_headers(ssl, request)) {
            ERR_clear_error();
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }
        const std::string req_line = first_line(request);
        const bool is_connect = req_line.rfind("CONNECT ", 0) == 0;

        const std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body_.size()) + "\r\n\r\n";

        if (!is_connect) {
            // 明文目标：absolute-form 请求行经代理 TLS 通道原样上线
            {
                std::lock_guard<std::mutex> lock(mutex_);
                http_request_line_ = req_line;
                http_request_full_ = request;
            }
            const std::string response = header + body_;
            (void)ssl_write_all(ssl, response.data(), response.size());
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            connect_request_line_ = req_line;
            connect_proxy_auth_ = header_value(request, "proxy-authorization");
        }

        if (reject_connect_) {
            const char* reply =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Length: 0\r\n\r\n";
            (void)ssl_write_all(ssl, reply, std::strlen(reply));
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }

        const char* ok_reply = "HTTP/1.1 200 Connection established\r\n\r\n";
        if (!ssl_write_all(ssl, ok_reply, std::strlen(ok_reply))) {
            ERR_clear_error();
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }

        if (!tunnel_tls_) {
            // CONNECT 已放行但未启用隧道 TLS：回一个响应体后收口
            // （客户端在其上做目标握手必然失败——mock 不支持该形态）
            const std::string response = header + body_;
            (void)ssl_write_all(ssl, response.data(), response.size());
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }

        // 模拟目标源站：同一证书的第二个 SSL 对象骑在 mem BIO 对上，
        // 密文经代理通道收发（TLS-in-TLS 双层封装的服务端形态）
        BIO* rbio = BIO_new(BIO_s_mem());
        BIO* wbio = BIO_new(BIO_s_mem());
        SSL* target = SSL_new(ctx_);
        if (!rbio || !wbio || !target) {
            if (rbio) BIO_free(rbio);
            if (wbio) BIO_free(wbio);
            if (target) SSL_free(target);
            ERR_clear_error();
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }
        // SSL 读 rbio 为空时按"等更多输入"处理（非 EOF）
        BIO_set_mem_eof_return(rbio, -1);
        SSL_set_bio(target, rbio, wbio);
        SSL_set_accept_state(target);
        if (!tunnel_handshake(target, rbio, wbio, ssl)) {
            SSL_free(target);
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++target_handshakes_;
        }
        std::string req;
        if (!tunnel_read_headers(target, rbio, wbio, ssl, req)) {
            SSL_free(target);
            SSL_free(ssl);
            CLOSE_SOCKET(conn);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            http_request_line_ = first_line(req);
            http_request_full_ = req;
        }
        const std::string response = header + body_;
        (void)tunnel_write(target, wbio, ssl, response);
        SSL_free(target);

        SSL_free(ssl);
        CLOSE_SOCKET(conn);
    }

    SSL_CTX* ctx_ = nullptr;
    std::string cert_dir_;
    std::string cert_path_;
    std::string key_path_;

    std::string body_;
    bool tunnel_tls_ = false;
    bool reject_connect_ = false;

    mutable std::mutex mutex_;
    int handshake_attempts_ = 0;
    int handshakes_ = 0;
    int target_handshakes_ = 0;
    std::string connect_request_line_;
    std::string connect_proxy_auth_;
    std::string http_request_line_;
    std::string http_request_full_;

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
};

class EngineRunner {
public:
    explicit EngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~EngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    EngineRunner(const EngineRunner&) = delete;
    EngineRunner& operator=(const EngineRunner&) = delete;

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

DownloadOptions base_https_proxy_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    return options;
}

std::string output_path_for(const char* tag) {
    const std::string dir = temp_dir_for(tag);
    std::filesystem::create_directories(dir);
    return (std::filesystem::path(dir) / "out.bin").string();
}

void remove_output(const std::string& path) {
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(path).parent_path(), ec);
}

/// SSL_CERT_FILE 指向自签证书：两层 TLS（代理 + 目标）共用同一信任锚
class ScopedCertFileEnv {
public:
    explicit ScopedCertFileEnv(const std::string& cert_path) {
        const char* old = ::getenv("SSL_CERT_FILE");
        had_old_ = old != nullptr;
        if (had_old_) old_ = old;
#ifdef _WIN32
        _putenv_s("SSL_CERT_FILE", cert_path.c_str());
#else
        ::setenv("SSL_CERT_FILE", cert_path.c_str(), 1);
#endif
    }
    ~ScopedCertFileEnv() {
#ifdef _WIN32
        _putenv_s("SSL_CERT_FILE", had_old_ ? old_.c_str() : "");
#else
        if (had_old_) {
            ::setenv("SSL_CERT_FILE", old_.c_str(), 1);
        } else {
            ::unsetenv("SSL_CERT_FILE");
        }
#endif
    }
    ScopedCertFileEnv(const ScopedCertFileEnv&) = delete;
    ScopedCertFileEnv& operator=(const ScopedCertFileEnv&) = delete;

private:
    std::string old_;
    bool had_old_ = false;
};

}  // namespace

//==============================================================================
// parse_http_proxy：https 代理参数（纯函数，零服务器）
//==============================================================================

TEST(ParseHttpProxyHttps, HttpsSchemeDefaultPort443) {
    DownloadOptions options;
    options.proxy = "https://proxy.local";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::HttpsProxy);
    EXPECT_EQ(cfg.host, "proxy.local");
    EXPECT_EQ(cfg.port, 443);
}

//==============================================================================
// 端到端：TLS-in-TLS、明文 absolute-form、凭据、失败收口
//==============================================================================

TEST(DownloadEngineV2HttpsProxy, PlainTargetUsesAbsoluteFormThroughProxy) {
    const std::string body = make_body(16 * 1024);
    HttpsProxyTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string out = output_path_for("plain");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_https_proxy_options(out);
    options.proxy = server.proxy_url();
    ScopedCertFileEnv trust(server.cert_path());

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.handshake_attempts(), 1);
    EXPECT_EQ(server.handshakes(), 1);
    EXPECT_EQ(server.target_handshakes(), 0);
    // 明文目标不经 CONNECT——代理 TLS 握手完成后 absolute-form GET 直接
    // 上线（CONNECT 隧道只属于 https 目标）
    EXPECT_EQ(server.connect_request_line(), "");
    // absolute-form 请求行在代理 TLS 通道内上线
    EXPECT_EQ(server.http_request_line(),
              "GET http://upstream.invalid:81/f.bin HTTP/1.1");
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2HttpsProxy, TlsInTlsDownloadCompletesByteIdentical) {
    const std::string body = make_body(32 * 1024);
    HttpsProxyTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);
    server.set_tunnel_tls(true);

    const std::string out = output_path_for("tls");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_https_proxy_options(out);
    options.proxy = server.proxy_url();
    ScopedCertFileEnv trust(server.cert_path());

    // 目标主机名 localhost 与证书 SAN 匹配（DNS:localhost）——目标层
    // 验签在代理通道内完成
    const TaskId id = engine.add_download(
        "https://localhost:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();
    server.stop();

    // 双层握手：代理通道 1 次 + 隧道内目标 1 次
    EXPECT_EQ(server.handshake_attempts(), 1);
    EXPECT_EQ(server.handshakes(), 1);
    EXPECT_EQ(server.target_handshakes(), 1);
    EXPECT_EQ(server.connect_request_line(),
              "CONNECT localhost:81 HTTP/1.1");
    // 隧道内目标层请求行回 origin-form（目标只见 HTTP 语义，不见代理）
    EXPECT_EQ(server.http_request_line(), "GET /f.bin HTTP/1.1");
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2HttpsProxy, ConnectCarriesBasicCredentialsOnlyToProxy) {
    const std::string body = make_body(16 * 1024);
    HttpsProxyTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);
    server.set_tunnel_tls(true);

    const std::string out = output_path_for("auth");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_https_proxy_options(out);
    options.proxy = "https://carol:pw@127.0.0.1:" +
                    std::to_string(server.port());
    ScopedCertFileEnv trust(server.cert_path());

    // https 目标：CONNECT 在代理 TLS 通道内发出，凭据随 CONNECT 交割
    const TaskId id = engine.add_download(
        "https://localhost:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();
    server.stop();

    // 凭据随 CONNECT 上线（代理 TLS 通道内 Basic）
    EXPECT_EQ(server.connect_request_line(),
              "CONNECT localhost:81 HTTP/1.1");
    EXPECT_EQ(server.connect_proxy_auth(),
              "Basic " + base64_encode("carol:pw"));
    // 隧道内目标层请求回 origin-form，且绝不携带 Proxy-Authorization
    //（不向目标泄漏）
    EXPECT_EQ(server.http_request_line(), "GET /f.bin HTTP/1.1");
    EXPECT_EQ(header_value(server.http_request_full(),
                           "proxy-authorization"),
              "");
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2HttpsProxy, PlainTargetProxyAuthRidesGetToProxy) {
    const std::string body = make_body(8 * 1024);
    HttpsProxyTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string out = output_path_for("plainauth");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_https_proxy_options(out);
    options.proxy = "https://dave:secret@127.0.0.1:" +
                    std::to_string(server.port());
    ScopedCertFileEnv trust(server.cert_path());

    // 明文目标：无 CONNECT，凭据随 absolute-form GET 交割（hop-by-hop，
    // 代理消费后不转发给目标）
    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.handshakes(), 1);
    EXPECT_EQ(server.target_handshakes(), 0);
    EXPECT_EQ(server.connect_request_line(), "");
    EXPECT_EQ(server.http_request_line(),
              "GET http://upstream.invalid:81/f.bin HTTP/1.1");
    EXPECT_EQ(header_value(server.http_request_full(),
                           "proxy-authorization"),
              "Basic " + base64_encode("dave:secret"));
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2HttpsProxy, ConnectRejectedFailsCleanly) {
    HttpsProxyTestServer server;
    ASSERT_TRUE(server.start());
    server.set_reject_connect(true);
    server.set_body("should-not-be-served");

    const std::string out = output_path_for("reject");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_https_proxy_options(out);
    options.proxy = server.proxy_url();
    ScopedCertFileEnv trust(server.cert_path());

    // https 目标：CONNECT 门禁被拒（403）
    const TaskId id = engine.add_download(
        "https://localhost:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();

    // 拒绝发生在 CONNECT 之后：代理通道握手已完成
    EXPECT_EQ(server.handshake_attempts(), 1);
    EXPECT_EQ(server.handshakes(), 1);
    EXPECT_EQ(server.connect_request_line(),
              "CONNECT localhost:81 HTTP/1.1");
    EXPECT_FALSE(std::filesystem::exists(out));
    remove_output(out);
}

TEST(DownloadEngineV2HttpsProxy, ProxyCertVerificationFailureFailsCleanly) {
    HttpsProxyTestServer server;
    ASSERT_TRUE(server.start());
    server.set_body("should-not-be-served");

    const std::string out = output_path_for("verify");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_https_proxy_options(out);
    options.proxy = server.proxy_url();
    // 信任锚指向不存在的文件：默认 CA 加载失败 + 自签证书不可信 →
    // 代理通道验签必然失败（确定性，不依赖系统证书库内容）
    ScopedCertFileEnv trust(temp_dir_for("nonexistent") + "/no.pem");

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    runner.shutdown_and_join();
    server.stop();

    // 失败发生在代理 TLS 握手阶段：CONNECT 从未发出
    EXPECT_GE(server.handshake_attempts(), 1);
    EXPECT_EQ(server.connect_request_line(), "");
    EXPECT_FALSE(std::filesystem::exists(out));
    remove_output(out);
}
