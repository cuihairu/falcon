/**
 * @file http_commands_socks_test.cpp
 * @brief V2 引擎 socks5 代理端到端测试（RFC 1928 握手 + RFC 1929 子协商
 *        + socks5h 语义目标地址交由代理解析）
 * @author Falcon Team
 * @date 2026-11-26
 *
 * 覆盖的核心不变量：
 * - parse_http_proxy：socks5:// / socks5h:// 的缺省端口与凭据解析；
 *   socks4/https 仍 Unsupported（适配层回退 V1 curl）
 * - 方法协商 0x00/0x02/0xFF 三态与子协商成功/失败；凭据精确上线
 * - CONNECT 目标编码三形态：ATYP 0x01（v4 字面量）/0x03（域名，
 *   socks5h 语义）/0x04（v6 字面量），端口大端
 * - 隧道建立后请求行回 origin-form（代理只做转发，不参与 HTTP）
 * - 代理拒绝/非法 ATYP/关闭连接均干净失败，成品不顶最终名
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
#include <csignal>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/http_commands.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
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
void ensure_winsock_for_socks_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int socks_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int socks_test_getpid() { return static_cast<int>(::getpid()); }
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
            (std::string("falcon_v2_socks_") + tag + "_" +
             std::to_string(socks_test_getpid())))
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

bool send_all(int conn, const char* data, std::size_t size) {
    std::size_t sent = 0;
    while (sent < size) {
        const recv_ssize n = ::send(conn, data + sent,
#ifdef _WIN32
                                    static_cast<int>(size - sent), 0);
#else
                                    size - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) return false;
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_exact(int conn, std::string& out, std::size_t size) {
    while (out.size() < size) {
        char buf[512];
        const auto want = std::min(sizeof(buf), size - out.size());
        const recv_ssize n = ::recv(conn, buf,
#ifdef _WIN32
                                    static_cast<int>(want),
#else
                                    want,
#endif
                                    0);
        if (n <= 0) return false;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

bool read_http_headers(int conn, std::string& out) {
    char buf[4096];
    while (out.find("\r\n\r\n") == std::string::npos &&
           out.size() < 64 * 1024) {
        const recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        out.append(buf, static_cast<std::size_t>(n));
    }
    return true;
}

std::string first_line(const std::string& request) {
    const auto eol = request.find("\r\n");
    return eol == std::string::npos ? request : request.substr(0, eol);
}

/**
 * @brief socks5 回环代理：脚本化协商结果，观察握手报文与目标编码
 *
 * 每用例一条连接，serve 串行执行（accept 线程单条处理），脚本字段
 * 在 start 前设置、观察字段在连接结束后读取，无需锁；mutex 仅用于
 * 等待观察值时的线程安全读取。
 */
class Socks5TestServer {
public:
    enum class Method { kNoAuth, kUserPass, kRejectAll };

    ~Socks5TestServer() { stop(); }

    bool start() {
#ifndef _WIN32
        signal(SIGPIPE, SIG_IGN);
#endif
#ifdef _WIN32
        ensure_winsock_for_socks_test();
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;
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
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    int port() const { return port_; }

    std::string proxy_url(const std::string& scheme = "socks5") const {
        return scheme + "://127.0.0.1:" + std::to_string(port_);
    }

    void set_method(Method method) { method_ = method; }
    void set_auth_status(unsigned char status) { auth_status_ = status; }
    void set_connect_rep(unsigned char rep) { connect_rep_ = rep; }
    void set_reply_atyp(unsigned char atyp) { reply_atyp_ = atyp; }
    void set_body(std::string body) { body_ = std::move(body); }

    int connections() const { return connections_.load(); }
    std::string greeting() { return snapshot(greeting_); }
    std::string auth_username() { return snapshot(auth_username_); }
    std::string auth_password() { return snapshot(auth_password_); }
    std::string connect_address() { return snapshot(connect_address_); }
    unsigned char connect_atyp() {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_atyp_;
    }
    uint16_t connect_port() {
        std::lock_guard<std::mutex> lock(mutex_);
        return connect_port_;
    }
    std::string http_request_line() { return snapshot(http_request_line_); }

private:
    std::string snapshot(const std::string& value) {
        std::lock_guard<std::mutex> lock(mutex_);
        return value;
    }

    void accept_loop() {
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (POLL(&pfd, 1, 500) <= 0) continue;
            sockaddr_in peer{};
            sock_len peer_len = sizeof(peer);
            const int conn = ::accept(listen_fd_,
                                      reinterpret_cast<sockaddr*>(&peer),
                                      &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            connections_.fetch_add(1);
            serve(conn);
        }
    }

    void serve(int conn) {
        set_socket_timeout(conn, 10);

        // Greeting：VER NMETHODS METHODS...
        std::string greeting;
        if (!read_exact(conn, greeting, 2)) {
            CLOSE_SOCKET(conn);
            return;
        }
        const auto nmethods = static_cast<unsigned char>(greeting[1]);
        if (!read_exact(conn, greeting, 2 + nmethods)) {
            CLOSE_SOCKET(conn);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            greeting_ = greeting;
        }

        unsigned char selected = 0x00;
        if (method_ == Method::kRejectAll) {
            selected = 0xff;
        } else if (method_ == Method::kUserPass) {
            selected = 0x02;
        }
        const char method_reply[] = {'\x05', static_cast<char>(selected)};
        if (!send_all(conn, method_reply, sizeof(method_reply))) {
            CLOSE_SOCKET(conn);
            return;
        }
        if (selected == 0xff) {
            CLOSE_SOCKET(conn);
            return;
        }

        // RFC 1929 子协商：VER ULEN UNAME PLEN PASSWD
        if (selected == 0x02) {
            std::string head;
            if (!read_exact(conn, head, 2)) {
                CLOSE_SOCKET(conn);
                return;
            }
            const auto ulen = static_cast<unsigned char>(head[1]);
            std::string auth;
            if (!read_exact(conn, auth, 1 + ulen)) {
                CLOSE_SOCKET(conn);
                return;
            }
            const auto plen = static_cast<unsigned char>(auth[ulen]);
            if (!read_exact(conn, auth,
                            static_cast<std::size_t>(ulen) + 1 + plen)) {
                CLOSE_SOCKET(conn);
                return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auth_username_ = auth.substr(0, ulen);
                auth_password_ = auth.substr(ulen + 1, plen);
            }
            const char auth_reply[] = {'\x01',
                                       static_cast<char>(auth_status_)};
            if (!send_all(conn, auth_reply, sizeof(auth_reply)) ||
                auth_status_ != 0x00) {
                CLOSE_SOCKET(conn);
                return;
            }
        }

        // CONNECT 请求：VER CMD RSV ATYP ADDR PORT
        std::string req;
        if (!read_exact(conn, req, 4)) {
            CLOSE_SOCKET(conn);
            return;
        }
        const auto atyp = static_cast<unsigned char>(req[3]);
        // 地址体起始在 req 中的偏移：IPv4/IPv6 紧跟 4 字节头，域名先读 1 字节长度
        std::size_t addr_start = 4;
        std::size_t addr_len = 0;
        if (atyp == 0x01) {
            addr_len = 4;
        } else if (atyp == 0x04) {
            addr_len = 16;
        } else if (atyp == 0x03) {
            if (!read_exact(conn, req, 5)) {
                CLOSE_SOCKET(conn);
                return;
            }
            addr_start = 5;  // req[4] 是域名长度域
            addr_len = static_cast<unsigned char>(req[4]);
        } else {
            CLOSE_SOCKET(conn);
            return;
        }
        if (!read_exact(conn, req, addr_start + addr_len + 2)) {
            CLOSE_SOCKET(conn);
            return;
        }
        const std::string address = req.substr(addr_start, addr_len);
        const uint16_t port = static_cast<uint16_t>(
            (static_cast<unsigned char>(req[addr_start + addr_len]) << 8) |
            static_cast<unsigned char>(req[addr_start + addr_len + 1]));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            connect_atyp_ = atyp;
            connect_address_ = address;
            connect_port_ = port;
        }

        // CONNECT 应答：VER REP RSV ATYP BND.ADDR BND.PORT
        std::string reply;
        reply += '\x05';
        reply += static_cast<char>(connect_rep_);
        reply += '\x00';
        reply += static_cast<char>(reply_atyp_);
        if (reply_atyp_ == 0x01) {
            reply.append(4, '\x00');
        } else if (reply_atyp_ == 0x04) {
            reply.append(16, '\x00');
        } else if (reply_atyp_ == 0x03) {
            reply += '\x01';
            reply += '\x00';
        } else {
            // 非法 ATYP：客户端必须拒绝成功收口
            reply.append(4, '\x00');
        }
        reply.append(2, '\x00');
        if (!send_all(conn, reply.data(), reply.size()) ||
            connect_rep_ != 0x00) {
            CLOSE_SOCKET(conn);
            return;
        }

        // 隧道内 HTTP 请求（origin-form）
        std::string request;
        if (!read_http_headers(conn, request)) {
            CLOSE_SOCKET(conn);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            http_request_line_ = first_line(request);
        }
        std::string header =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/octet-stream\r\n"
            "Connection: close\r\n"
            "Content-Length: " + std::to_string(body_.size()) + "\r\n\r\n";
        const bool ok = send_all(conn, header.data(), header.size()) &&
                        send_all(conn, body_.data(), body_.size());
        (void)ok;
        CLOSE_SOCKET(conn);
    }

    Method method_ = Method::kNoAuth;
    unsigned char auth_status_ = 0x00;
    unsigned char connect_rep_ = 0x00;
    unsigned char reply_atyp_ = 0x01;
    std::string body_;

    mutable std::mutex mutex_;
    std::string greeting_;
    std::string auth_username_;
    std::string auth_password_;
    unsigned char connect_atyp_ = 0;
    std::string connect_address_;
    uint16_t connect_port_ = 0;
    std::string http_request_line_;

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> connections_{0};
    std::thread accept_thread_;
};

class SocksEngineRunner {
public:
    explicit SocksEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~SocksEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    SocksEngineRunner(const SocksEngineRunner&) = delete;
    SocksEngineRunner& operator=(const SocksEngineRunner&) = delete;

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

DownloadOptions base_socks_options(const std::string& out_path) {
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

}  // namespace

//==============================================================================
// parse_http_proxy：socks5 参数矩阵（纯函数，零服务器）
//==============================================================================

TEST(ParseHttpProxySocks5, Socks5SchemeAndDefaultPort) {
    DownloadOptions options;
    options.proxy = "socks5://proxy.local";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::Socks5);
    EXPECT_EQ(cfg.host, "proxy.local");
    EXPECT_EQ(cfg.port, 1080);
}

TEST(ParseHttpProxySocks5, Socks5hSchemeAndExplicitPort) {
    DownloadOptions options;
    options.proxy = "socks5h://proxy.local:1081";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::Socks5);
    EXPECT_EQ(cfg.host, "proxy.local");
    EXPECT_EQ(cfg.port, 1081);
}

TEST(ParseHttpProxySocks5, UserinfoCredentialsWinOverFields) {
    DownloadOptions options;
    options.proxy = "socks5://alice:pw@proxy.local:1080";
    options.proxy_username = "ignored";
    options.proxy_password = "ignored";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.kind, HttpProxyKind::Socks5);
    EXPECT_EQ(cfg.username, "alice");
    EXPECT_EQ(cfg.password, "pw");
}

TEST(ParseHttpProxySocks5, CredentialFieldsUsedWithoutUserinfo) {
    DownloadOptions options;
    options.proxy = "socks5://proxy.local:1080";
    options.proxy_username = "bob";
    options.proxy_password = "secret";
    const auto cfg = parse_http_proxy(options);
    EXPECT_EQ(cfg.username, "bob");
    EXPECT_EQ(cfg.password, "secret");
}

//==============================================================================
// 隧道端到端：握手、认证、目标编码与失败收口
//==============================================================================

TEST(DownloadEngineV2Socks5, NoAuthDownloadsWithOriginFormRequestLine) {
    const std::string body = make_body(32 * 1024);
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string out = output_path_for("noauth");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = server.proxy_url();

    // RFC 6761 保留域：解析器不可解析——成功即证明目标交由代理解析
    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();
    server.stop();

    // Greeting: VER=5 NMETHODS=1 METHODS=[0]
    EXPECT_EQ(server.greeting(), std::string("\x05\x01\x00", 3));
    // socks5h 语义：域名按 ATYP 0x03 原样发代理，不在此端解析
    EXPECT_EQ(server.connect_atyp(), 0x03);
    EXPECT_EQ(server.connect_address(), "upstream.invalid");
    EXPECT_EQ(server.connect_port(), 81);
    // 隧道内为 origin-form（代理只转发，客户端不再用 absolute-form）
    EXPECT_EQ(server.http_request_line(), "GET /f.bin HTTP/1.1");
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, AuthSubnegotiationSendsExactCredentials) {
    const std::string body = make_body(16 * 1024);
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_method(Socks5TestServer::Method::kUserPass);
    server.set_body(body);

    const std::string out = output_path_for("auth_creds");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = "socks5://carol:pw@127.0.0.1:" +
                    std::to_string(server.port());

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.greeting(), std::string("\x05\x02\x00\x02", 4));
    EXPECT_EQ(server.auth_username(), "carol");
    EXPECT_EQ(server.auth_password(), "pw");
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, Ipv4LiteralUsesAtyp1) {
    const std::string body = make_body(8 * 1024);
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string out = output_path_for("v4");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = server.proxy_url();

    const TaskId id = engine.add_download(
        "http://192.0.2.7:8080/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(server.connect_atyp(), 0x01);
    EXPECT_EQ(server.connect_address(), std::string("\xc0\x00\x02\x07", 4));
    EXPECT_EQ(server.connect_port(), 8080);
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, Ipv6LiteralUsesAtyp4) {
    const std::string body = make_body(8 * 1024);
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_body(body);

    const std::string out = output_path_for("v6");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = server.proxy_url();

    const TaskId id = engine.add_download(
        "http://[::1]:8080/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    runner.shutdown_and_join();
    server.stop();

    in6_addr loopback = {};
    loopback.s6_addr[15] = 1;
    EXPECT_EQ(server.connect_atyp(), 0x04);
    EXPECT_EQ(server.connect_address(),
              std::string(reinterpret_cast<const char*>(&loopback),
                          sizeof(loopback)));
    EXPECT_EQ(server.connect_port(), 8080);
    EXPECT_EQ(read_file_content(out), body);
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, MethodRejectionFailsCleanly) {
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_method(Socks5TestServer::Method::kRejectAll);

    const std::string out = output_path_for("reject_method");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = server.proxy_url();

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("no auth method"),
              std::string::npos);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_FALSE(std::filesystem::exists(out));
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, AuthRejectionFailsCleanly) {
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_method(Socks5TestServer::Method::kUserPass);
    server.set_auth_status(0x01);

    const std::string out = output_path_for("reject_auth");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = "socks5://carol:pw@127.0.0.1:" +
                    std::to_string(server.port());

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("auth failed"), std::string::npos);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_FALSE(std::filesystem::exists(out));
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, ConnectRejectionFailsCleanly) {
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_connect_rep(0x05);  // connection refused by proxy

    const std::string out = output_path_for("reject_connect");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = server.proxy_url();

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("REP=5"), std::string::npos);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_FALSE(std::filesystem::exists(out));
    remove_output(out);
}

TEST(DownloadEngineV2Socks5, InvalidReplyAtypFailsCleanly) {
    Socks5TestServer server;
    ASSERT_TRUE(server.start());
    server.set_reply_atyp(0x07);  // 非法 ATYP，REP 仍为 0

    const std::string out = output_path_for("bad_atyp");
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    auto options = base_socks_options(out);
    options.proxy = server.proxy_url();

    const TaskId id = engine.add_download(
        "http://upstream.invalid:81/f.bin", options);
    ASSERT_GT(id, 0u);
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    SocksEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("truncated"), std::string::npos);
    runner.shutdown_and_join();
    server.stop();

    EXPECT_FALSE(std::filesystem::exists(out));
    remove_output(out);
}
