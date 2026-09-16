/**
 * @file http_commands_edges_test.cpp
 * @brief V2 引擎 HTTP 命令真实缺口收敛（TLS 错误路径 / 段失败收口 /
 *        chunked 边界 / 重定向变体 / 断点续传调度 / 发布失败 / 解析
 *        容错）
 * @author Falcon Team
 * @date 2026-09-14
 *
 * 覆盖的核心不变量（均为既有引擎代码此前从未执行过的路径）：
 * - 传输中断：TLS 干净关闭（close_notify）截断必须 FAILED（总长未知
 *   断连不构成完成证据）、TLS 头阶段断连、明文 RST 截断、accept 即
 *   RST（发送失败）——半成品一律不顶最终名
 * - 分段失败收口：段响应非 206（Range 撒谎）组必 FAILED；TLS 分段
 *   握手失败经段失败收口聚合终态，另一段唤醒后不再覆盖终态
 * - 重定向五变体：query 剥除、相对路径 ../ 归一化到根、空 Location
 *   失败、协议相对 //host 跟随、304+Location（无跟随语义）失败
 * - 断点续传调度：零断点遇全量响应继续、Content-Range 不可解析放弃
 *   续传转全新下载、跨会话恢复段 k>0 由初始连接承载并重建分段状态
 * - chunked 分片边界：块大小行 CRLF 跨分片（回归：CR 单独到达曾被
 *   预消费进大小行导致静默错帧）、尾部终止序列跨分片（回归：CR 曾
 *   被丢弃导致终止 CRLF 永不可见）、CR/LF 错位必 FAILED、带 trailer
 *   头的双 CRLF 终止
 * - 发布失败：最终名被目录占用时改名失败按失败收尾，不假报完成
 * - 解析容错：头区裸 LF 行跳过后正常完成
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
#include <falcon/protocols/resume_control.hpp>

#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "tls_cert_generator.hpp"
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_edges_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int edges_test_getpid() { return _getpid(); }
using sock_len = int;
using conn_thread = std::thread;
#else
inline int edges_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using conn_thread = std::thread;
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

void write_file_content(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

std::string temp_dir_for(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("falcon_v2_edges_") + tag + "_" +
             std::to_string(edges_test_getpid())))
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

bool send_all_plain(int conn, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
#ifdef _WIN32
        const int n = ::send(conn, data + sent,
                             static_cast<int>(size - sent), 0);
#else
        const auto n = ::send(conn, data + sent,
                              static_cast<std::size_t>(size - sent),
                              MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            // 失败现场证据：错误码区分「对端已关连接」（EPIPE/ECONNRESET，
            // 客户端先失败）与「发送超时」（ETIMEDOUT/EAGAIN，服务端自己卡）
#ifdef _WIN32
            std::fprintf(stderr, "[edges] send 失败: wsa=%d sent=%zu/%zu\n",
                         ::WSAGetLastError(), sent, size);
#else
            std::fprintf(stderr, "[edges] send 失败: errno=%d(%s) sent=%zu/%zu\n",
                         errno, std::strerror(errno), sent, size);
#endif
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

/// 读到 \r\n\r\n（明文；SO_RCVTIMEO 兜底防挂死）
bool read_headers_plain(int conn, std::string& out) {
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos &&
           out.size() < 64 * 1024) {
        const auto n = ::recv(conn, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

void set_socket_timeout(int conn, int seconds) {
    timeval tv{};
    tv.tv_sec = seconds;
#ifdef _WIN32
    (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
    (void)setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    (void)setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/// 立即 RST 关闭（SO_LINGER{1,0} + close：对端读/写均得 ECONNRESET，
/// 无 FIN 时序）
void rst_close(int conn) {
#ifdef _WIN32
    BOOL on = TRUE;
    linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
    (void)setsockopt(conn, SOL_SOCKET, SO_LINGER,
                     reinterpret_cast<const char*>(&l), sizeof(l));
    (void)on;
#else
    linger l{};
    l.l_onoff = 1;
    l.l_linger = 0;
    (void)setsockopt(conn, SOL_SOCKET, SO_LINGER, &l, sizeof(l));
#endif
    CLOSE_SOCKET(conn);
}

/// 提取请求行的目标（method 与 HTTP 版本之间）
std::string request_target_of(const std::string& request) {
    const auto eol = request.find("\r\n");
    const std::string line =
        eol == std::string::npos ? request : request.substr(0, eol);
    const auto sp1 = line.find(' ');
    if (sp1 == std::string::npos) return {};
    const auto sp2 = line.find(' ', sp1 + 1);
    return sp2 == std::string::npos
               ? line.substr(sp1 + 1)
               : line.substr(sp1 + 1, sp2 - sp1 - 1);
}

/// 解析 Range 请求头（"bytes=123-" 开放末端 / "bytes=123-456" 封闭
/// 区间）；无 Range 返回 false。封闭末端时 end 精确，开放末端时 end
/// 置为 SIZE_MAX 由调用方 clamp 到文件尾
bool range_start_of(const std::string& request, std::size_t& start,
                    std::size_t& end) {
    const auto pos = request.find("Range: bytes=");
    if (pos == std::string::npos) return false;
    const auto begin = pos + std::string("Range: bytes=").size();
    const auto eol = request.find("\r\n", begin);
    const auto dash = request.find('-', begin);
    if (dash == std::string::npos || eol == std::string::npos) return false;
    start = static_cast<std::size_t>(
        std::stoull(request.substr(begin, dash - begin)));
    end = SIZE_MAX;  // 开放末端
    if (dash + 1 < eol) {
        end = static_cast<std::size_t>(
            std::stoull(request.substr(dash + 1, eol - dash - 1)));
    }
    return true;
}

class ScriptableServer;

/// 每条连接一段剧本：(conn, server)。弹出顺序 = 连接建立顺序
using ConnHandler = std::function<void(int, ScriptableServer&)>;

/**
 * @brief 编程式剧本服务器
 *
 * poll 接受循环 + 每连接一线程按剧本队列处理（先到连接先弹出）；
 * 剧本耗尽时新连接直接关闭。可选 TLS 上下文供隧道/HTTPS 剧本使用。
 * 记录每条连接的请求目标与连接总数供断言。
 */
class ScriptableServer {
public:
    ~ScriptableServer() { stop(); }

    bool start() {
#ifdef _WIN32
        ensure_winsock_for_edges_test();
#else
        // 服务器线程（本进程内）写已被客户端关闭的连接默认触发
        // SIGPIPE 杀死测试进程——引擎侧 send 已带 MSG_NOSIGNAL，这里
        // 兜底保护 TLS 写与服务器侧写（daemon 生产进程同法忽略）
        ::signal(SIGPIPE, SIG_IGN);
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);  // 回环字节序怪癖规避
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

#ifdef FALCON_ENABLE_OPENSSL
    /// 启用 TLS（HTTPS/隧道剧本的前置；证书运行时生成）
    bool enable_tls(const std::string& key_path, const std::string& cert_path) {
        if (!falcon_test_tls::generate_self_signed_cert(key_path, cert_path)) {
            return false;
        }
        tls_ctx_ = SSL_CTX_new(TLS_server_method());
        if (!tls_ctx_) {
            return false;
        }
        SSL_CTX_set_min_proto_version(tls_ctx_, TLS1_2_VERSION);
        if (SSL_CTX_use_certificate_file(tls_ctx_, cert_path.c_str(),
                                         SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(tls_ctx_, key_path.c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(tls_ctx_) != 1) {
            SSL_CTX_free(tls_ctx_);
            tls_ctx_ = nullptr;
            ERR_clear_error();
            return false;
        }
        cert_path_ = cert_path;
        return true;
    }

    SSL_CTX* tls_ctx() const { return tls_ctx_; }
    const std::string& cert_path() const { return cert_path_; }
#endif

    void script(std::vector<ConnHandler> handlers) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& h : handlers) {
            handlers_.push_back(std::move(h));
        }
    }

    void stop() {
        running_ = false;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_,
#ifdef _WIN32
                       SD_BOTH
#else
                       SHUT_RDWR
#endif
            );
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
#ifdef FALCON_ENABLE_OPENSSL
        if (tls_ctx_) {
            SSL_CTX_free(tls_ctx_);
            tls_ctx_ = nullptr;
            ERR_clear_error();
        }
#endif
    }

    int port() const { return port_; }

    std::string http_url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    std::string proxy_url_for_edges() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::vector<std::string> request_targets() {
        std::lock_guard<std::mutex> lock(mutex_);
        return request_targets_;
    }

    int connection_count() const { return connection_count_.load(); }

    void record_target(const std::string& target) {
        std::lock_guard<std::mutex> lock(mutex_);
        request_targets_.push_back(target);
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
            connection_count_.fetch_add(1);
            ConnHandler handler;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!handlers_.empty()) {
                    handler = std::move(handlers_.front());
                    handlers_.pop_front();
                }
            }
            conn_threads_.emplace_back([this, conn,
                                        handler = std::move(handler)]() mutable {
                set_socket_timeout(conn, 10);
                if (handler) {
                    handler(conn, *this);
                }
                CLOSE_SOCKET(conn);  // 剧本未关的连接统一收口
            });
        }
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> connection_count_{0};

    std::mutex mutex_;
    std::deque<ConnHandler> handlers_;
    std::vector<std::string> request_targets_;

    std::thread accept_thread_;
    std::vector<conn_thread> conn_threads_;

#ifdef FALCON_ENABLE_OPENSSL
    SSL_CTX* tls_ctx_ = nullptr;
    std::string cert_path_;
#endif
};

//==============================================================================
// 剧本工厂（明文 HTTP）
//==============================================================================

/// 读请求头（记录目标）后应答：status + extra + Content-Length + body
ConnHandler serve_http(int status, std::string extra_headers,
                       std::string body) {
    return [status, extra = std::move(extra_headers),
            body = std::move(body)](int conn, ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));

        const char* reason = status == 200 ? "OK"
                             : status == 206 ? "Partial Content"
                             : status == 302 ? "Found"
                             : status == 304 ? "Not Modified"
                                             : "Status";
        std::string header = "HTTP/1.1 " + std::to_string(status) + " " +
                             reason + "\r\n" + extra +
                             "Content-Length: " +
                             std::to_string(body.size()) +
                             "\r\nConnection: close\r\n\r\n";
        (void)send_all_plain(conn, header.data(), header.size());
        if (!body.empty()) {
            (void)send_all_plain(conn, body.data(), body.size());
        }
    };
}

/// 原样发送响应头（畸形头容错用例：裸 LF 行等），头后接 body
ConnHandler serve_raw(std::string raw_headers, std::string body) {
    return [raw = std::move(raw_headers),
            body = std::move(body)](int conn, ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));
        (void)send_all_plain(conn, raw.data(), raw.size());
        if (!body.empty()) {
            (void)send_all_plain(conn, body.data(), body.size());
        }
    };
}

/// Range 忠实服务器：解析 Range 区间回 206 + 精确区间载荷（多段续传
/// 的段请求是封闭区间 bytes=start-end，载荷必须与请求末端一致——
/// 多发到文件尾会因长度不符被段校验拒绝）。默认附 Accept-Ranges 头
ConnHandler serve_range_suffix(const std::string& full_body,
                               std::string extra_headers = {},
                               std::string content_range_override = {}) {
    return [full_body, extra = std::move(extra_headers),
            override_cr = std::move(content_range_override)](
               int conn, ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));

        std::size_t start = 0;
        std::size_t end = full_body.size() - 1;
        if (range_start_of(request, start, end)) {
            if (end >= full_body.size()) end = full_body.size() - 1;
        } else {
            start = 0;
            end = full_body.size() - 1;
        }
        const std::string payload =
            full_body.substr(start, end - start + 1);
        const std::string content_range =
            override_cr.empty()
                ? "Content-Range: bytes " + std::to_string(start) + "-" +
                      std::to_string(end) + "/" +
                      std::to_string(full_body.size()) + "\r\n"
                : override_cr;
        std::string header = "HTTP/1.1 206 Partial Content\r\n";
        if (extra.find("Accept-Ranges") == std::string::npos &&
            override_cr.empty()) {
            header += "Accept-Ranges: bytes\r\n";
        }
        header += content_range + extra +
                  "Content-Length: " + std::to_string(payload.size()) +
                  "\r\nConnection: close\r\n\r\n";
        (void)send_all_plain(conn, header.data(), header.size());
        (void)send_all_plain(conn, payload.data(), payload.size());
    };
}

/// Range 撒谎服务器：忽略 Range 回 200 全量（分段响应校验用例）
ConnHandler serve_range_liar(std::string full_body) {
    return [body = std::move(full_body)](int conn, ScriptableServer& srv) {
        serve_http(200, "Accept-Ranges: bytes\r\n", body)(conn, srv);
    };
}

/// 读请求头后立即干净关闭（FIN）——对端读到 EOF
ConnHandler close_after_head() {
    return [](int conn, ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));
    };
}

/// accept 即 RST（对端发送阶段得 ECONNRESET/EPIPE）
ConnHandler rst_on_accept() {
    return [](int conn, ScriptableServer&) { rst_close(conn); };
}

/// 读请求头后 RST
ConnHandler rst_after_head() {
    return [](int conn, ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));
        rst_close(conn);
    };
}

/// 发出 headers + 半截 body 后 RST（传输中断路径）
ConnHandler partial_then_rst(std::string headers, std::string partial_body) {
    return [headers = std::move(headers),
            partial = std::move(partial_body)](int conn,
                                               ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));
        (void)send_all_plain(conn, headers.data(), headers.size());
        (void)send_all_plain(conn, partial.data(), partial.size());
        rst_close(conn);
    };
}

/// chunked 响应：头 + 依序分片发送（每片间隔 gap_ms）——分片边界
/// 精确可控，专测状态机跨分片行为
ConnHandler chunked_parts(std::vector<std::string> parts, int gap_ms) {
    return [parts = std::move(parts), gap_ms](int conn,
                                              ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) return;
        srv.record_target(request_target_of(request));
        static const char kHeader[] =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n\r\n";
        (void)send_all_plain(conn, kHeader, sizeof(kHeader) - 1);
        for (const auto& part : parts) {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
            if (!send_all_plain(conn, part.data(), part.size())) return;
        }
    };
}

#ifdef FALCON_ENABLE_OPENSSL

//==============================================================================
// 剧本工厂（TLS）
//==============================================================================

/// TLS 全量服务：握手 → 读头 → 200 + body（body 可延迟）
ConnHandler tls_serve(std::string body, int delay_before_body_ms = 0) {
    return [body = std::move(body),
            delay_ms = delay_before_body_ms](int conn,
                                             ScriptableServer& srv) {
        SSL* ssl = SSL_new(srv.tls_ctx());
        if (!ssl) return;
        SSL_set_fd(ssl, conn);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            ERR_clear_error();
            return;
        }

        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) {
                SSL_free(ssl);
                ERR_clear_error();
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        srv.record_target(request_target_of(request));

        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Accept-Ranges: bytes\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        auto write_all = [ssl](const std::string& data) {
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
        (void)write_all(header);
        if (delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        (void)write_all(body);
        SSL_shutdown(ssl);  // close_notify：客户端按长度判完成不受影响
        SSL_free(ssl);
        ERR_clear_error();
    };
}

/// TLS 半截 body 后干净关闭（SSL_shutdown 发 close_notify → 客户端
/// SSL_read 得 ZERO_RETURN，截断判定路径）
ConnHandler tls_partial_body_then_shutdown(std::string headers,
                                           std::string partial_body) {
    return [headers = std::move(headers),
            partial = std::move(partial_body)](int conn,
                                               ScriptableServer& srv) {
        SSL* ssl = SSL_new(srv.tls_ctx());
        if (!ssl) return;
        SSL_set_fd(ssl, conn);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            ERR_clear_error();
            return;
        }
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) {
                SSL_free(ssl);
                ERR_clear_error();
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        srv.record_target(request_target_of(request));

        auto write_all = [ssl](const std::string& data) {
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
        (void)write_all(headers);
        (void)write_all(partial);
        SSL_shutdown(ssl);  // 干净关闭：TLS 层 EOF（非底层断连）
        SSL_free(ssl);
        ERR_clear_error();
    };
}

/// TLS 半截响应头后干净关闭（头阶段 SSL_read → ZERO_RETURN）
ConnHandler tls_partial_headers_then_shutdown(std::string partial_headers) {
    return [partial = std::move(partial_headers)](int conn,
                                                  ScriptableServer& srv) {
        SSL* ssl = SSL_new(srv.tls_ctx());
        if (!ssl) return;
        SSL_set_fd(ssl, conn);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            ERR_clear_error();
            return;
        }
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) {
                SSL_free(ssl);
                ERR_clear_error();
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        srv.record_target(request_target_of(request));

        std::size_t sent = 0;
        while (sent < partial.size()) {
            const int n = SSL_write(ssl, partial.data() + sent,
                                    static_cast<int>(partial.size() - sent));
            if (n <= 0) break;
            sent += static_cast<std::size_t>(n);
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ERR_clear_error();
    };
}

/// TLS 半截 body 后注入非法 record（TLS 层外裸发未知类型字节）：
/// 客户端 SSL_read 的 record 层解析失败 → SSL_ERROR_SSL 硬失败
/// （区别于干净关闭的 ZERO_RETURN 与底层断连的 SYSCALL——两者按
/// EOF 交给截断判定，协议违规直接判败）
ConnHandler tls_partial_body_then_garbage_record(std::string headers,
                                                 std::string partial_body) {
    return [headers = std::move(headers),
            partial = std::move(partial_body)](int conn,
                                               ScriptableServer& srv) {
        SSL* ssl = SSL_new(srv.tls_ctx());
        if (!ssl) return;
        SSL_set_fd(ssl, conn);
        if (SSL_accept(ssl) != 1) {
            SSL_free(ssl);
            ERR_clear_error();
            return;
        }
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            const int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) {
                SSL_free(ssl);
                ERR_clear_error();
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }
        srv.record_target(request_target_of(request));

        auto write_all = [ssl](const std::string& data) {
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
        (void)write_all(headers);
        (void)write_all(partial);
        // 直接 SSL_free（不发 close_notify）——TLS 会话废弃；随后在
        // 原始 socket 上注入未知 record 类型 0xff：对端 SSL_read 在
        // record 层即报 SSL_ERROR_SSL（协议违规，非 EOF 语义）。
        // TCP 有序保证垃圾字节先于框架收尾的 FIN 到达对端
        SSL_free(ssl);
        ERR_clear_error();
        static const char kGarbageRecord[] = "\xff\xff\xff\xff\xff";
        (void)send_all_plain(conn, kGarbageRecord, sizeof(kGarbageRecord) - 1);
    };
}

#endif  // FALCON_ENABLE_OPENSSL

//==============================================================================
// 测试基建
//==============================================================================

class EdgesEngineRunner {
public:
    explicit EdgesEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~EdgesEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    EdgesEngineRunner(const EdgesEngineRunner&) = delete;
    EdgesEngineRunner& operator=(const EdgesEngineRunner&) = delete;

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

DownloadOptions base_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    return options;
}

/// 手工构造断点续传现场：控制文件 + 临时文件（默认 .falcon.tmp）
struct ResumeScene {
    std::string ctrl_path;
    std::string temp_path;

    static ResumeScene create(const std::string& out_path,
                              const std::string& url, Bytes total,
                              const std::string& etag,
                              std::vector<ResumeSegment> segments,
                              const std::string& temp_data) {
        ResumeScene scene;
        scene.ctrl_path = out_path + kResumeControlExtension;
        scene.temp_path = out_path + ".falcon.tmp";

        ResumeControl control;
        control.url = url;
        control.total = total;
        control.etag = etag;
        control.segments = std::move(segments);
        EXPECT_TRUE(save_resume_control(scene.ctrl_path, control));
        write_file_content(scene.temp_path, temp_data);  // 可为空文件
        return scene;
    }
};

//==============================================================================
// 传输中断与发送失败
//==============================================================================

/// 保留域 .invalid 本地快速解析失败：组终态 FAILED（初始连接失败
/// 收口），不悬挂
TEST(DownloadEngineV2Edges, UnresolvableHostFailsTerminal) {
    const std::string dir = temp_dir_for("resolve");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        "http://nonexistent-falcon-test.invalid/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 仅 IPv6 的主机名（ip6-localhost → ::1）：V2 数据面 AF_INET 解析
/// 不到可用地址，干净失败（无连接悬挂）
TEST(DownloadEngineV2Edges, Ipv6OnlyHostnameFailsClean) {
    const std::string dir = temp_dir_for("v6only");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id =
        engine.add_download("http://ip6-localhost:9/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 明文体阶段 RST（SO_LINGER{1,0}）：已收 4KB ≠ 声明 96KB，截断必须
/// FAILED，半成品不顶最终名
TEST(DownloadEngineV2Edges, PlainRstMidBodyFails) {
    const std::string body = make_body(96 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({partial_then_rst(
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n",
        body.substr(0, 4096))});

    const std::string dir = temp_dir_for("rstmid");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "RST 截断必须失败收口";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// accept 即 RST：客户端发送请求即遇 ECONNRESET（或首个 send 成功后
/// 读到 RST）——两条路径任一发生都必须终态 FAILED
TEST(DownloadEngineV2Edges, SendFailureAfterRstFailsTerminal) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({rst_on_accept()});

    const std::string dir = temp_dir_for("sendrst");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 分段失败收口
//==============================================================================

/// 段连接（非首段）响应非 206：Range 撒谎服务器回 200 全量——响应
/// 校验必须拒绝该段并把组收口为 FAILED（损坏数据绝不落盘）
TEST(DownloadEngineV2Edges, SegmentResponseNot206FailsGroup) {
    const std::string body = make_body(256 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(200, "Accept-Ranges: bytes\r\n", body),
                   serve_range_liar(body)});

    const std::string dir = temp_dir_for("liar");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.min_segment_size = 64 * 1024;
    options.max_connections = 2;
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "段响应非 206 必须拒绝";
    EXPECT_NE(group->error_message().find("response rejected"),
              std::string::npos)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 大流量让出（NEED_RETRY）
//==============================================================================

/// 5MB 突发：单次 execute 至多读 64×64KB=4MB，到上限主动让出（NEED_
/// RETRY）后由引擎例行循环续读至完成——分批让出绝不丢数据或卡死
TEST(DownloadEngineV2Edges, BigBurstYieldsThenCompletes) {
    const std::string body = make_body(5u * 1024 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(200, "Accept-Ranges: bytes\r\n", body)});

    const std::string dir = temp_dir_for("burst");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 60000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 发布失败（temp_extension 改名）
//==============================================================================

/// 最终名已被目录占用（overwrite 显式授权过门禁）：下载完成但改名
/// 失败必须按失败收尾——绝不假报 COMPLETED，临时文件保留可续传
TEST(DownloadEngineV2Edges, PublishRenameFailsWhenTargetIsDirectory) {
    const std::string body = make_body(64 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(200, "", body)});

    const std::string dir = temp_dir_for("publish");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "busy.bin").string();
    // 最终名是已存在目录：rename(临时文件 → 目录) 必然失败
    std::filesystem::create_directory(out_path);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.overwrite_existing = true;  // 过覆盖门禁，聚焦发布路径
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "改名失败不得假报完成";
    EXPECT_NE(group->error_message().find("发布失败"), std::string::npos)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    // 最终名目录原封不动；临时文件保留（未来续传挂点）
    EXPECT_TRUE(std::filesystem::is_directory(out_path));
    EXPECT_TRUE(std::filesystem::exists(out_path + ".falcon.tmp"));

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 重定向变体
//==============================================================================

/// Location 带 query：解析时剥除（基于纯 path 跟随）
TEST(DownloadEngineV2Edges, RedirectQueryStrippedFromLocation) {
    const std::string body = make_body(32 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(302, "Location: /target?q=1\r\n", ""),
                   serve_http(200, "", body)});

    const std::string dir = temp_dir_for("redirq");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/r"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    const auto targets = server.request_targets();
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[1], "/target");  // query 已剥除

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 相对 Location ".."：基于当前目录归一化，弹出全部分段后归一化为
/// "/"（空结果回退根路径）
TEST(DownloadEngineV2Edges, RedirectRelativeDotsNormalizeToRoot) {
    const std::string body = make_body(32 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(302, "Location: ..\r\n", ""),
                   serve_http(200, "", body)});

    const std::string dir = temp_dir_for("redirectdots");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/x/y.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    const auto targets = server.request_targets();
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[1], "/");  // /x/ + .. → 弹出 x → 根

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 空 Location：无法解析 → 干净失败（不悬挂、不崩）
TEST(DownloadEngineV2Edges, RedirectEmptyLocationFailsCleanly) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(302, "Location:\r\n", "")});

    const std::string dir = temp_dir_for("redirectempty");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/r"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("Failed to follow redirect"),
              std::string::npos)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 协议相对 Location（//host:port/path）：沿用当前 scheme 跟随
TEST(DownloadEngineV2Edges, RedirectProtocolRelativeSucceeds) {
    const std::string body = make_body(32 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(302, "Location: //127.0.0.1:" +
                                         std::to_string(server.port()) +
                                         "/target\r\n", ""),
                   serve_http(200, "", body)});

    const std::string dir = temp_dir_for("redirectproto");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/r"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    const auto targets = server.request_targets();
    ASSERT_EQ(targets.size(), 2u);
    EXPECT_EQ(targets[1], "/target");

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 304 + Location：304 无跟随语义（is_redirect_status 不含 304），
/// 必须失败收口而非盲跟
TEST(DownloadEngineV2Edges, RedirectStatus304FailsCleanly) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(304, "Location: /target\r\n", "")});

    const std::string dir = temp_dir_for("redirect304");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/r"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    // 只打了一连接：304 未被跟随
    EXPECT_EQ(server.connection_count(), 1);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 断点续传调度
//==============================================================================

/// 零断点（控制文件存在、各段 downloaded=0、空临时文件）：初始请求
/// 无 Range，全量 200 响应与计划总长一致 → 按续传调度继续完成
TEST(DownloadEngineV2Edges, ResumeZeroProgressFullResponseContinues) {
    const std::string body = make_body(64 * 1024);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(200, "ETag: \"v1\"\r\n", body)});

    const std::string dir = temp_dir_for("resumezero");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    const std::string url = server.http_url("/f.bin");
    ResumeScene::create(out_path, url, body.size(), "v1",
                        {/*seg0: 全新零进度*/ {0, body.size(), 0}},
                        /*temp_data=*/"");

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(url, options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    // 完成后控制文件删除
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension));

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// Content-Range 不可解析（"bytes garbage"）：206 + 长度一致但起点
/// 校验失败 → 放弃续传（控制文件删除）→ 重新发起无 Range 全新下载
/// 并完整覆盖临时文件——绝不把旧断点接续到来历不明的响应上
TEST(DownloadEngineV2Edges, ResumeGarbageContentRangeAbandonsAndRestarts) {
    const std::string body = make_body(96 * 1024);
    const Bytes pivot = 32 * 1024;

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_range_suffix(body, "", "Content-Range: bytes garbage\r\n"),
                   serve_http(200, "", body)});

    const std::string dir = temp_dir_for("resumegarbage");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    const std::string url = server.http_url("/f.bin");
    ResumeScene::create(out_path, url, body.size(), "v1",
                        {{0, body.size(), pivot}},
                        /*temp_data=*/body.substr(0, pivot));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(url, options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    // 全新重下逐字节一致；控制文件已随放弃删除
    EXPECT_EQ(read_file_content(out_path), body);
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension));
    EXPECT_EQ(server.connection_count(), 2);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 跨会话恢复段 k>0：三段计划 [完成, 部分进度, 零进度]——初始连接
/// 承载段 1 的续传 Range（段 0 已完成被跳过），响应在此重建组级分
/// 段状态并发起其余分段；成品逐字节一致
TEST(DownloadEngineV2Edges, ResumeMultiSegmentInitialCarriesPartialSegment) {
    const Bytes seg = 64 * 1024;
    const Bytes partial = 16 * 1024;
    const std::string body = make_body(static_cast<std::size_t>(seg * 3));

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_range_suffix(body),   // 初始连接：段 1 续传
                   serve_range_suffix(body)}); // 段 2 连接

    const std::string dir = temp_dir_for("resumemulti");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    const std::string url = server.http_url("/f.bin");
    ResumeScene::create(
        out_path, url, body.size(), "v1",
        {{0, seg, seg},               // 段 0 已完成：不再建连接
         {seg, seg, partial},         // 段 1 部分进度：初始连接承载
         {seg * 2, seg, 0}},          // 段 2 零进度：恢复连接从头下
        /*temp_data=*/body.substr(0, seg + partial));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.max_connections = 2;  // 段 2 需要第二连接
    const TaskId task_id = engine.add_download(url, options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    EXPECT_FALSE(std::filesystem::exists(out_path + kResumeControlExtension));

    // 初始 + 段 2：恰好两连接（段 0 已完成不建连接）
    EXPECT_EQ(server.connection_count(), 2);
    const auto targets = server.request_targets();
    ASSERT_EQ(targets.size(), 2u);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// chunked 分片边界
//==============================================================================

/// 块大小行 CRLF 跨分片："5" / "\r" / "\nhello\r\n" 三片到达——CR
/// 单独在缓冲末尾时必须等待 LF 补判而非预消费（回归：预消费曾把 LF
/// 与块数据并进大小行，静默错帧后 CR/LF 错位失败）
TEST(DownloadEngineV2Edges, ChunkedSplitSizeLineSucceeds) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts(
        {"5", "\r", "\nhello\r\n", "4\r\nworl", "\r\n0\r\n", "X-T",
         "r: falcon\r", "\n"},
        120)});

    const std::string dir = temp_dir_for("chunksplit");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), "helloworl");

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 尾部终止序列跨分片：trailer 行内容之后 "\r" 单独到达（缓冲末尾）
/// 置位等待，下一分片的 "\n" 补齐终止——必须置位等待而非丢弃 CR
/// （回归：丢弃曾让终止 CRLF 永不可见，挂到 EOF 判截断）
TEST(DownloadEngineV2Edges, ChunkedTrailerSplitTerminalSucceeds) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts({"4\r\nabcd\r\n", "0\r\nX-Trailer: falcon",
                                  "\r", "\n"},
                                 120)});

    const std::string dir = temp_dir_for("chunktrail");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), "abcd");

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 尾部区先等待（无任何 CR）再收双 CRLF 终止：带 trailer 头的完整
/// 终止块（"\r\n\r\n" 双 CRLF 分支）
TEST(DownloadEngineV2Edges, ChunkedTrailerHeaderBlockCompletes) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts({"4\r\nabcd\r\n", "0\r\nX-T",
                                  "railer: falcon\r\n\r\n"},
                                 120)});

    const std::string dir = temp_dir_for("chunkd_crlf");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), "abcd");

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 数据后期待 CR 却来其他字节：分块帧损坏 → FAILED（半成品不顶名）
TEST(DownloadEngineV2Edges, ChunkedMalformedCrAfterDataFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts({"4\r\nabc", "dX\r\n0\r\n\r\n"}, 120)});

    const std::string dir = temp_dir_for("chunkbadcr");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "期望 CR 得其他字节必须判帧错误";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// CR 后期待 LF 却来其他字节：分块帧损坏 → FAILED
TEST(DownloadEngineV2Edges, ChunkedMalformedLfAfterCrFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts({"4\r\nabcd\r", "X\r\n0\r\n\r\n"}, 120)});

    const std::string dir = temp_dir_for("chunkbadlf");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "期望 LF 得其他字节必须判帧错误";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 解析容错
//==============================================================================

/// 头区裸 LF 行（getline 得空行）：跳过该行继续解析，后续头与 body
/// 正常——响应解析对杂散 LF 容忍
TEST(DownloadEngineV2Edges, MalformedHeaderBareLfLineSkipped) {
    const std::string body = make_body(4096);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_raw("HTTP/1.1 200 OK\n\nContent-Length: " +
                                 std::to_string(body.size()) +
                                 "\r\nConnection: close\r\n\r\n",
                             body)});

    const std::string dir = temp_dir_for("barelf");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// TLS 错误路径与代理 CONNECT 失败（需 OpenSSL）
//==============================================================================

#ifdef FALCON_ENABLE_OPENSSL

/// TLS 干净关闭截断：声明 96KB 只到 4KB 后 SSL_shutdown（close_notify
/// → 客户端 SSL_read 得 ZERO_RETURN → 按 EOF 交给截断判定）→ 必须
/// FAILED——总长未收满，干净关闭不构成完成证据
TEST(DownloadEngineV2Edges, TlsCleanCloseTruncationFails) {
    const std::string body = make_body(96 * 1024);

    const std::string dir = temp_dir_for("tlstrunc");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.enable_tls(key_path, cert_path));
    server.script({tls_partial_body_then_shutdown(
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n",
        body.substr(0, 4096))});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.verify_ssl = false;
    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "TLS 干净关闭截断必须失败";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// TLS 响应头阶段干净关闭：半截头 + close_notify → SSL_read 得
/// ZERO_RETURN（非 WANT_*）→ 头接收失败收口
TEST(DownloadEngineV2Edges, TlsPartialHeadersCleanCloseFails) {
    const std::string dir = temp_dir_for("tlsparthead");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.enable_tls(key_path, cert_path));
    server.script({tls_partial_headers_then_shutdown(
        "HTTP/1.1 200 OK\r\nContent-Le")});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.verify_ssl = false;
    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// TLS 数据阶段收到非法 record（协议违规）：SSL_read 得
/// SSL_ERROR_SSL 硬失败——与 ZERO_RETURN/SYSCALL 的 EOF 语义分流，
/// 直接按传输错误收口（区别于截断判定路径）
TEST(DownloadEngineV2Edges, TlsGarbageRecordFails) {
    const std::string body = make_body(96 * 1024);

    const std::string dir = temp_dir_for("tlsgarbage");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.enable_tls(key_path, cert_path));
    server.script({tls_partial_body_then_garbage_record(
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
            "\r\nConnection: close\r\n\r\n",
        body.substr(0, 4096))});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.verify_ssl = false;
    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "协议违规（非法 record）必须按硬失败收口";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// TLS 多分段：段 1 连接在 TLS 握手前被服务器裸关（无 TLS 字节）→
/// 握手失败经段失败收口（finish_segment + 组 FAILED）聚合终态；段 0
/// 的下载命令在体延迟到达后唤醒，观察到组已 FAILED 静默退出，不覆
/// 盖终态
TEST(DownloadEngineV2Edges, TlsMultiSegmentHandshakeFailureFailsGroup) {
    const std::string body = make_body(256 * 1024);

    const std::string dir = temp_dir_for("tlsmultifail");
    std::filesystem::create_directories(dir);
    const std::string key_path =
        (std::filesystem::path(dir) / "key.pem").string();
    const std::string cert_path =
        (std::filesystem::path(dir) / "cert.pem").string();
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    ASSERT_TRUE(server.enable_tls(key_path, cert_path));
    server.script({tls_serve(body, /*delay_before_body_ms=*/400),
                   [](int conn, ScriptableServer&) {
                       // 段 1 连接：accept 后立即裸关——TLS 握手在
                       // ClientHello 后读到 EOF（SYSCALL），必失败
                       CLOSE_SOCKET(conn);
                   }});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.min_segment_size = 64 * 1024;
    options.max_connections = 2;
    options.verify_ssl = false;
    const TaskId task_id = engine.add_download(
        "https://localhost:" + std::to_string(server.port()) + "/f.bin",
        options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "段握手失败必须把组收口为 FAILED";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 代理在 CONNECT 应答完成前干净关闭（FIN → recv==0）：隧道未建立
/// 即失败收口
TEST(DownloadEngineV2Edges, ProxyClosesBeforeConnectResponseFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({close_after_head()});

    const std::string dir = temp_dir_for("proxyclose");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.proxy = server.proxy_url_for_edges();
    const TaskId task_id = engine.add_download(
        "https://localhost:9/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 代理在 CONNECT 后 RST：recv 得 ECONNRESET（非 would-block）→ 失
/// 败收口
TEST(DownloadEngineV2Edges, ProxyRstAfterConnectFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({rst_after_head()});

    const std::string dir = temp_dir_for("proxyrst");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.proxy = server.proxy_url_for_edges();
    const TaskId task_id = engine.add_download(
        "https://localhost:9/f.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 代理场景 + 空 authority 目标 URL（http:///empty.bin）：连接阶段只
/// 解析代理地址（目标 host 延迟），absolute-form 请求行原样发代理；
/// 代理回 302 + 非绝对 Location → resolve_redirect_location 从请求
/// URL 提取的 authority 为空 → 解析失败按失败收口（不发起跟随连接）
TEST(DownloadEngineV2Edges, ProxyRedirectFromEmptyAuthorityUrlFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(302, "Location: /moved.bin\r\n", "")});

    const std::string dir = temp_dir_for("proxyemptyauth");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.proxy = server.proxy_url_for_edges();
    const TaskId task_id = engine.add_download("http:///empty.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "空 authority 的重定向基准 URL 必须按失败收口";

    runner.shutdown_and_join();
    server.stop();

    // 恰一次连接（跟随未发起），请求行为 absolute-form 原样透传
    EXPECT_EQ(server.connection_count(), 1);
    const auto targets = server.request_targets();
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets.front(), "http:///empty.bin");

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

//==============================================================================
// 批次 R：零字节/超大响应头/多段回退/chunked 大小行变体/NEED_RETRY 让出链
//==============================================================================

/// Content-Length: 0：EOF 即完成证据（length_==0），receive_data 置
/// 完成并返回 OK，execute 同轮收口（关 fd + 完成组），成品为空文件
TEST(DownloadEngineV2Edges, ZeroByteDownloadCompletes) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(200, "", "")});

    const std::string dir = temp_dir_for("zerolen");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 响应头超 1MB 仍无终止空行（裸 LF 行永不构成头终止）：解析终止，
/// 组 FAILED，不产生成品
TEST(DownloadEngineV2Edges, OversizedResponseHeadersFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    std::string garbage;
    garbage.reserve(1100u * 1024u);
    while (garbage.size() < 1100u * 1024u) {
        garbage += "x: y\n";
    }
    server.script({serve_raw(garbage, "")});

    const std::string dir = temp_dir_for("bighdr");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "超过 1MB 的响应头必须终止解析";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_FALSE(std::filesystem::exists(out_path));

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 拆分计划不足两段（content/min_segment_size = 1）：多段门禁内部
/// 回退单连接，成品逐字节一致
TEST(DownloadEngineV2Edges, SegmentPlanTooSmallFallsBackToSingleConnection) {
    const std::string body = make_body(1500);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({serve_http(200, "Accept-Ranges: bytes\r\n", body)});

    const std::string dir = temp_dir_for("segplan1");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.max_connections = 4;
    options.min_segment_size = 1024;  // 1500/1024 = 1 段 → 拆分不可行
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 大小行 CR 单独到达后，下批首字节非 LF：帧错误 → FAILED
TEST(DownloadEngineV2Edges, ChunkedSizeLineCrPendingThenGarbageFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts(
        {"4\r\nabcd\r\n", "5\r", "Z\r\n0\r\n\r\n"}, 120)});

    const std::string dir = temp_dir_for("chunkcrsz");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "大小行 CR 后非 LF 必须判帧错误";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 大小行 CR 单独到达、LF 补齐后十六进制大小无效：帧错误 → FAILED
TEST(DownloadEngineV2Edges, ChunkedSizeLineCrPendingInvalidHexFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts(
        {"4\r\nabcd\r\n", "ZZ\r", "\n0\r\n\r\n"}, 120)});

    const std::string dir = temp_dir_for("chunkcrhex");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "CR 补齐 LF 后的无效十六进制大小必须判帧错误";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 同缓冲内大小行 CR 后非 LF（CRLF 未拆开）：帧错误 → FAILED
TEST(DownloadEngineV2Edges, ChunkedSizeLineCrFollowedByGarbageSameBufferFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts(
        {"4\r\nabcd\r\n5\rX0\r\n\r\n"}, 120)});

    const std::string dir = temp_dir_for("chunkszcrx");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "同缓冲大小行 CR 后非 LF 必须判帧错误";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 同缓冲内大小行 CRLF 完整但十六进制大小无效：帧错误 → FAILED
TEST(DownloadEngineV2Edges, ChunkedSizeLineInvalidHexSameBufferFails) {
    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script({chunked_parts(
        {"4\r\nabcd\r\nZZ\r\n0\r\n\r\n"}, 120)});

    const std::string dir = temp_dir_for("chunkszhex");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "同缓冲无效十六进制大小必须判帧错误";

    runner.shutdown_and_join();
    server.stop();

    EXPECT_TRUE(read_file_content(out_path).empty());

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 灌入-静默-再灌入服务器：头部后先灌 first_burst 字节，静默 gap_ms
/// 再灌余量。配合客户端直写（enable_disk_cache=false，每轮 recv+落盘
/// 远慢于服务器灌入），内核积压单调增长，单次 execute 必然读满 64 轮
/// 触发 NEED_RETRY——静默后的再灌入让再次让出发生在 1s 之后，顺带
/// 覆盖 update_progress 的速度计算分支。sent_out 非空时记录服务端
/// 成功发出的字节总数（失败鉴证：区分「服务端没发完（客户端先失败
/// 杀流）」与「服务端发完仍失败（截断）」）
ConnHandler burst_silence_burst(Bytes total, Bytes first_burst, int gap_ms,
                                std::shared_ptr<std::atomic<Bytes>> sent_out =
                                    {}) {
    return [total, first_burst, gap_ms,
            sent_out](int conn, ScriptableServer& srv) {
        std::string request;
        if (!read_headers_plain(conn, request)) {
            std::fprintf(stderr, "[edges] burst: 读请求头失败\n");
            return;
        }
        srv.record_target(request_target_of(request));

        std::string unit(4096, '\0');
        for (std::size_t i = 0; i < unit.size(); ++i) {
            unit[i] = static_cast<char>((i * 31 + 7) & 0xFF);
        }
        std::string burst;
        burst.reserve(256 * 1024);
        while (burst.size() < 256 * 1024) {
            burst += unit;
        }

        auto note = [&](Bytes n) {
            if (sent_out) sent_out->fetch_add(n);
        };

        std::string head = "HTTP/1.1 200 OK\r\nContent-Length: " +
                           std::to_string(total) +
                           "\r\nConnection: close\r\n\r\n";
        if (!send_all_plain(conn, head.data(), head.size())) return;
        note(static_cast<Bytes>(head.size()));

        auto send_n = [&](Bytes n) {
            while (n > 0) {
                const std::size_t chunk = static_cast<std::size_t>(
                    std::min<Bytes>(burst.size(), n));
                if (!send_all_plain(conn, burst.data(), chunk)) return false;
                note(static_cast<Bytes>(chunk));
                n -= chunk;
            }
            return true;
        };
        if (!send_n(first_burst)) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
        (void)send_n(total - first_burst);
    };
}

/// 32MB 突发-静默-再突发：每段 16MB 深灌（积压单调增长），读满
/// 64×64KB=4MB 主动让出（NEED_RETRY，execute 尾部 update_progress +
/// 回队）；静默 1200ms 后的第二段让出距命令构造已超 1s，速度计算
/// 分支执行——最终成品逐块一致
TEST(DownloadEngineV2Edges, BurstSilenceBurstNeedRetryYieldsAndCompletes) {
    constexpr Bytes kTotal = Bytes{32u} * 1024u * 1024u;
    constexpr Bytes kFirstBurst = Bytes{16u} * 1024u * 1024u;
    // 服务端视角证据：断言失败时输出「服务端已发 / 落盘文件大小 /
    // 组记账进度」三方数据，用于区分服务端发送失败（客户端先死杀流）
    // 与服务端发完仍失败（客户端 EOF 截断/写盘失败）
    const auto server_sent = std::make_shared<std::atomic<Bytes>>(0);

    ScriptableServer server;
    ASSERT_TRUE(server.start());
    server.script(
        {burst_silence_burst(kTotal, kFirstBurst, 1200, server_sent)});

    const std::string dir = temp_dir_for("burstretry");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "out.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.enable_disk_cache = false;  // 直写拖慢客户端，制造内核积压
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    const TaskId task_id = engine.add_download(
        server.http_url("/f.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EdgesEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 60000));
    {
        // V2 数据落在 <最终名>.falcon.tmp（temp_extension 默认值），
        // 失败时最终名可能不存在——两个路径都查
        std::error_code size_ec;
        const auto out_size =
            std::filesystem::file_size(out_path, size_ec);
        const std::string tmp_size_str = [&] {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(
                out_path + ".falcon.tmp", ec);
            return ec ? std::string("n/a") : std::to_string(sz);
        }();
        ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
            << group->error_message() << " [server_sent="
            << server_sent->load() << " out_size="
            << (size_ec ? std::string("n/a") : std::to_string(out_size))
            << " tmp_size=" << tmp_size_str << " group_downloaded="
            << group->downloaded_bytes() << "]";
    }

    runner.shutdown_and_join();
    server.stop();

    std::string unit(4096, '\0');
    for (std::size_t i = 0; i < unit.size(); ++i) {
        unit[i] = static_cast<char>((i * 31 + 7) & 0xFF);
    }
    const std::string data = read_file_content(out_path);
    ASSERT_EQ(data.size(), static_cast<std::size_t>(kTotal))
        << "[server_sent=" << server_sent->load() << "]";
    for (const Bytes off : {Bytes{0}, kFirstBurst - 4096, kFirstBurst,
                            kTotal - 4096}) {
        EXPECT_EQ(data.substr(static_cast<std::size_t>(off), 4096), unit)
            << "偏移 " << off << " 处的 4KB 块不一致 [server_sent="
            << server_sent->load() << "]";
    }

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

#endif  // FALCON_ENABLE_OPENSSL

} // namespace
