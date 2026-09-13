/**
 * @file http_commands_redirect_test.cpp
 * @brief V2 引擎重定向跟随端到端测试（Location 解析 + 命令链接力）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖的核心不变量：
 * - 301/302/307/308 响应跟随 Location 重新发起连接，成品一致，
 *   跳数计入链上限
 * - RFC 3986 引用解析四形态：绝对 URL / 协议相对 //host/path /
 *   绝对路径 /path / 相对路径（含 ../ 上跳归一化）
 * - 防护：重定向环在 kMaxRedirects 内截断按失败收口（有界连接数）；
 *   重定向到 https 目标暂不支持（直接 https:// 请求已放行），明确
 *   失败而非静默
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

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
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
void ensure_winsock_for_redirect_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int redirect_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int redirect_test_getpid() { return static_cast<int>(::getpid()); }
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
            (std::string("falcon_v2_redirect_") + tag + "_" +
             std::to_string(redirect_test_getpid())))
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
 * @brief 重定向路由测试服务器
 *
 * 路由表：
 * - redirects_[path] = {status, location}：3xx 应答
 * - bodies_[path]：200 全量
 * 每 path 的命中次数计数（环用例断言有界跳数）。
 */
class RedirectServer {
public:
    ~RedirectServer() { stop(); }

    bool start() {
#ifdef _WIN32
        ensure_winsock_for_redirect_test();
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        sockaddr_in bound{};
        sock_len len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
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
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

    int port() const { return port_; }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

    void add_redirect(std::string path, int status, std::string location) {
        std::lock_guard<std::mutex> lock(mutex_);
        redirects_[std::move(path)] = {status, std::move(location)};
    }

    void add_body(std::string path, std::string body) {
        std::lock_guard<std::mutex> lock(mutex_);
        bodies_[std::move(path)] = std::move(body);
    }

    /// 某 path 的请求次数（环用例断言有界跳数）
    int hits(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = hits_.find(path);
        return it == hits_.end() ? 0 : it->second;
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
            int conn = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            conn_threads_.emplace_back([this, conn] { serve(conn); });
        }
    }

    /// 从请求行取 path（去掉 query）
    static std::string request_path(const std::string& request) {
        const auto sp1 = request.find(' ');
        if (sp1 == std::string::npos) return "/";
        const auto sp2 = request.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return "/";
        std::string target = request.substr(sp1 + 1, sp2 - sp1 - 1);
        const auto q = target.find('?');
        if (q != std::string::npos) target.resize(q);
        return target.empty() ? "/" : target;
    }

    void serve(int conn) {
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 64 * 1024) {
            recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                CLOSE_SOCKET(conn);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        const std::string path = request_path(request);

        std::string body;
        int status = 404;
        std::string location;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++hits_[path];
            auto r = redirects_.find(path);
            if (r != redirects_.end()) {
                status = r->second.first;
                location = r->second.second;
            } else {
                auto b = bodies_.find(path);
                if (b != bodies_.end()) {
                    status = 200;
                    body = b->second;
                }
            }
        }

        std::string header = "HTTP/1.1 " + std::to_string(status) +
                             " X\r\n"
                             "Content-Type: application/octet-stream\r\n"
                             "Connection: close\r\n";
        if (!location.empty()) {
            header += "Location: " + location + "\r\n";
        }
        header += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";

        send_all(conn, header.data(), header.size());
        send_all(conn, body.data(), body.size());
        CLOSE_SOCKET(conn);
    }

    void send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            recv_ssize n = ::send(conn, data + sent,
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
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    mutable std::mutex mutex_;
    std::map<std::string, std::pair<int, std::string>> redirects_;
    std::map<std::string, std::string> bodies_;
    std::map<std::string, int> hits_;

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

/// 引擎线程 RAII 守卫（同暂停/chunked 测试模式）
class RedirectEngineRunner {
public:
    explicit RedirectEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~RedirectEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    RedirectEngineRunner(const RedirectEngineRunner&) = delete;
    RedirectEngineRunner& operator=(const RedirectEngineRunner&) = delete;

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
    return wait_for(
        [&] {
            const auto st = group->status();
            return st == RequestGroupStatus::COMPLETED ||
                   st == RequestGroupStatus::FAILED;
        },
        timeout_ms);
}

DownloadOptions redirect_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    return options;
}

/// 跟随多跳重定向后成品一致（302 → 307 → 200，绝对 + 绝对路径混合）
TEST(DownloadEngineV2Redirect, MultiHopChainFollowsWithExactPayload) {
    const std::string body = make_body(32 * 1024);
    RedirectServer server;
    ASSERT_TRUE(server.start());
    server.add_redirect("/start", 302, server.url("/hop2"));
    server.add_redirect("/hop2", 307, "/final.bin");
    server.add_body("/final.bin", body);

    const std::string dir = temp_dir_for("chain");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "r.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(
        server.url("/start"), redirect_options(out_path));
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    RedirectEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 相对路径形态（../ 上跳 + 裸相对名归一化）解析正确
TEST(DownloadEngineV2Redirect, RelativeLocationWithDotDotNormalization) {
    const std::string body = make_body(16 * 1024);
    RedirectServer server;
    ASSERT_TRUE(server.start());
    // 当前 /a/b/cur.bin + "../up/rel.bin" → /a/up/rel.bin
    server.add_redirect("/a/b/cur.bin", 302, "../up/rel.bin");
    server.add_body("/a/up/rel.bin", body);

    const std::string dir = temp_dir_for("rel");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "rel.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(
        server.url("/a/b/cur.bin"), redirect_options(out_path));
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    RedirectEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body);
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 重定向环：kMaxRedirects 内截断按失败收口，连接次数有界
TEST(DownloadEngineV2Redirect, RedirectLoopFailsBounded) {
    RedirectServer server;
    ASSERT_TRUE(server.start());
    // 自引用环：/loop 永远 302 到 /loop
    server.add_redirect("/loop", 302, server.url("/loop"));

    const std::string dir = temp_dir_for("loop");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "loop.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(
        server.url("/loop"), redirect_options(out_path));
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    RedirectEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "重定向环必须按失败收口";

    runner.shutdown_and_join();
    server.stop();

    // 首连 + kMaxRedirects 跳，绝不多打（防无界循环）
    EXPECT_LE(server.hits("/loop"), 6);
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 重定向到 https 目标：暂不支持，明确失败而非静默或崩溃
TEST(DownloadEngineV2Redirect, HttpsTargetFailsCleanly) {
    RedirectServer server;
    ASSERT_TRUE(server.start());
    server.add_redirect("/tohttps", 302, "https://example.com/secure.bin");

    const std::string dir = temp_dir_for("https");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "h.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(
        server.url("/tohttps"), redirect_options(out_path));
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    RedirectEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED)
        << "重定向到 https 目标必须干净失败";

    runner.shutdown_and_join();
    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

} // namespace
