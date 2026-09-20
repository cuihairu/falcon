// 自包含 TLS 回环测试基建（从 http_commands_tls_test.cpp 抽取，供
// TLS 端到端与故障注入等多套测试共用）
//
// 能力：运行时自签证书回环服务器（阻塞 SSL_accept + 请求头读取 +
// 200/Content-Length/Connection:close 应答）+ SNI/握手计数观测 +
// 引擎线程 RAII 守卫 + 终态等待辅助。

#pragma once

#include <gtest/gtest.h>

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
#else
#include <pthread.h>
#include <csignal>
#endif

#include <falcon/protocols/download_engine_v2.hpp>

namespace falcon::testtls {

#ifdef _WIN32
inline void ensure_winsock_for_tls_test() {
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

inline std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

inline std::string read_file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

inline std::string temp_dir_for(const char* tag) {
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


#ifdef FALCON_ENABLE_OPENSSL
/**
 * @brief TLS 回环测试服务器
 *
 * 阻塞式 SSL_accept 处理请求（引擎侧握手由事件驱动推进，两侧时序
 * 天然兼容）；收到请求头后回 200 + Content-Length + Connection:close
 * 并直接关闭（客户端按长度判完成，无需等待 close_notify）。
 * 记录握手成功的 SNI 与连接数供断言。客户端在 ClientHello 前失败
 * （创建类注入）时 SSL_accept 以 EOF 告败，handshakes() 保持 0——
 * 可用作"失败发生在握手之前"的观测证据。
 */
class TlsTestServer {
public:
    ~TlsTestServer() { stop(); }

    /// dual_stack=true：AF_INET6 + IPV6_V6ONLY=0 监听回环（同一端口
    /// 同时接受 [::1] 与 127.0.0.1）。环境无 IPv6（socket/bind 失败）
    /// 返回 false，调用方 GTEST_SKIP
    bool start(const std::string& key_path, const std::string& cert_path,
               bool dual_stack = false) {
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

        int v6only = 0;
        sockaddr_in6 addr6{};
        if (dual_stack) {
            listen_fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
            if (listen_fd_ < 0) {
                stop();
                return false;
            }
            ::setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY,
                         reinterpret_cast<const char*>(&v6only),
                         sizeof(v6only));
            addr6.sin6_family = AF_INET6;
            addr6.sin6_port = 0;
            addr6.sin6_addr = in6addr_loopback;
            if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr6),
                       sizeof(addr6)) != 0 ||
                ::listen(listen_fd_, 8) != 0) {
                stop();
                return false;
            }
        } else {
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
        }

        if (dual_stack) {
            sock_len len = sizeof(sockaddr_in6);
            sockaddr_in6 bound{};
            if (::getsockname(listen_fd_,
                              reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
                stop();
                return false;
            }
            port_ = ntohs(bound.sin6_port);
        } else {
            sockaddr_in bound{};
            sock_len len = sizeof(bound);
            if (::getsockname(listen_fd_,
                              reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
                stop();
                return false;
            }
            port_ = ntohs(bound.sin_port);
        }

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
#ifndef _WIN32
        // OpenSSL 内部写（TLS 1.3 握手后的 NewSessionTicket、SSL_free 的
        // close_notify）不经 MSG_NOSIGNAL：客户端提前关闭时 EPIPE 会以
        // SIGPIPE 杀死整个测试进程——accept 线程屏蔽之（send 改回 EPIPE
        // 错误码，OpenSSL 按写失败正常处理）
        sigset_t sigpipe_set;
        sigemptyset(&sigpipe_set);
        sigaddset(&sigpipe_set, SIGPIPE);
        pthread_sigmask(SIG_BLOCK, &sigpipe_set, nullptr);
#endif
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
#endif  // FALCON_ENABLE_OPENSSL

/// 引擎线程 RAII 守卫（同暂停/重定向测试模式）
class TlsEngineRunner {
public:
    explicit TlsEngineRunner(falcon::DownloadEngineV2& engine)
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
    falcon::DownloadEngineV2& engine_;
    std::thread thread_;
};

inline bool wait_group_terminal(falcon::DownloadEngineV2& engine,
                                falcon::RequestGroup* group, int timeout_ms) {
    (void)engine;
    return wait_for(
        [&] {
            const auto st = group->status();
            return st == falcon::RequestGroupStatus::COMPLETED ||
                   st == falcon::RequestGroupStatus::FAILED;
        },
        timeout_ms);
}

}  // namespace falcon::testtls
