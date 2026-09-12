/**
 * @file global_speed_limit_test.cpp
 * @brief V1 引擎全局限速端到端测试（经 IEventListener::query_speed_limit 通道）
 * @author Falcon Team
 * @date 2026-09-12
 *
 * 引擎 set_global_speed_limit 此前只是存储值，下载路径零消费端；
 * 通道打通后：TaskManager 实现 query_speed_limit（全局按并发槽位均摊、
 * 与任务自身限速取严），HttpHandler 在 curl 进度回调里查询并应用。
 *
 * 本测试用回环 HTTP server + 时间下界断言验证限速真实生效：
 * - 全速对照：回环下载远快于限速场景
 * - 限速场景：256KB / 64KB/s ≈ 4s，断言耗时下界（只断言"变慢"，
 *   不设上界，避免 CI 负载抖动误报）
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
#include <falcon/download_engine.hpp>

#include "../../plugins/http/http_handler.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_limit_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int test_getpid() { return _getpid(); }
#else
inline int test_getpid() { return static_cast<int>(::getpid()); }
#endif

/// 最小回环 HTTP server：支持 HEAD（文件探测）与 GET（下载数据）
class MinimalHttpServer {
public:
    bool start(std::string body) {
#ifdef _WIN32
        ensure_winsock_for_limit_test();
#endif
        body_ = std::move(body);

        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
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
        // 排水：关闭残留的连接线程
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

    int port() const { return port_; }
    const std::string& body() const { return body_; }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }

private:
    void accept_loop() {
        // poll 带超时轮询 running_：阻塞 accept 上直接 close(fd) 在
        // Linux 不保证唤醒（复用 http_commands_coverage_test 的模板）
        while (running_) {
            struct pollfd pfd;
            pfd.fd = listen_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;
            const int ready = POLL(&pfd, 1, 500);
            if (ready <= 0) {
                continue;  // 超时或错误：重新检查 running_
            }
            sockaddr_in peer{};
            socklen_t peer_len = sizeof(peer);
            int conn = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            conn_threads_.emplace_back([this, conn] { serve(conn); });
        }
    }

    void serve(int conn) {
        // 读请求头直到 \r\n\r\n
        std::string request;
        char buf[2048];
        while (request.find("\r\n\r\n") == std::string::npos &&
               request.size() < 16 * 1024) {
            ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                CLOSE_SOCKET(conn);
                return;
            }
            request.append(buf, static_cast<std::size_t>(n));
        }

        const bool is_head = request.rfind("HEAD", 0) == 0;

        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                             "Accept-Ranges: none\r\n"
                             "Connection: close\r\n\r\n";
        send_all(conn, header.data(), header.size());
        if (!is_head) {
            send_all(conn, body_.data(), body_.size());
        }
        CLOSE_SOCKET(conn);
    }

    void send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            ssize_t n = ::send(conn, data + sent,
#ifdef _WIN32
                               static_cast<int>(size - sent),
#else
                               size - sent,
#endif
                               0);
            if (n <= 0) return;
            sent += static_cast<std::size_t>(n);
        }
    }

    std::string body_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

/// 生成测试 body（确定性填充）
std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body.push_back(static_cast<char>('a' + (i % 26)));
    }
    return body;
}

/// 配置单槽位引擎（限速全部分摊给单个任务，测试时长可控）
EngineConfig single_task_config() {
    EngineConfig config;
    config.max_concurrent_tasks = 1;
    return config;
}

std::string temp_output_dir() {
    return (std::filesystem::temp_directory_path() /
            ("falcon_limit_test_" + std::to_string(test_getpid())))
        .string();
}

} // namespace

TEST(GlobalSpeedLimitTest, UnlimitedLoopbackDownloadCompletesQuickly) {
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(make_body(256 * 1024)));

    DownloadEngine engine(single_task_config());
    engine.register_handler(protocols::create_http_handler());

    DownloadOptions options;
    options.output_directory = temp_output_dir();
    options.output_filename = "unlimited.bin";
    options.max_connections = 1;  // 单连接路径
    std::filesystem::create_directories(options.output_directory);

    auto task = engine.add_task(server.url("/unlimited.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(20)));
    const auto elapsed = std::chrono::steady_clock::now() - begin;

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    // 全速对照：回环下载 256KB 远快于限速场景；只设宽松上界防 CI 抖动误报
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 3000);

    engine.cancel_all();
    server.stop();
    std::filesystem::remove_all(options.output_directory);
}

TEST(GlobalSpeedLimitTest, GlobalLimitSlowsDownload) {
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(make_body(256 * 1024)));

    DownloadEngine engine(single_task_config());
    engine.register_handler(protocols::create_http_handler());

    // 全局 64KB/s，单槽位 → 任务限速 64KB/s → 256KB 约 4s
    engine.set_global_speed_limit(64 * 1024);

    DownloadOptions options;
    options.output_directory = temp_output_dir();
    options.output_filename = "limited.bin";
    options.max_connections = 1;
    std::filesystem::create_directories(options.output_directory);

    auto task = engine.add_task(server.url("/limited.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    const auto begin = std::chrono::steady_clock::now();
    // 理论 4s；上界放宽到 30s 防限速精度与 CI 负载带来的偏差
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(30)));
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    // 时间下界断言：限速生效时下载必然被拉长到秒级（全速时毫秒级完成）。
    // 只断言下界不断言上界——限速只会更慢，不会更快的语义是稳定的
    EXPECT_GE(elapsed_ms, 2500)
        << "限速未生效：256KB @ 64KB/s 应至少耗时约 4s，实际 " << elapsed_ms << "ms";

    engine.cancel_all();
    server.stop();
    std::filesystem::remove_all(options.output_directory);
}

TEST(GlobalSpeedLimitTest, TaskOwnLimitAppliesWithoutGlobal) {
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(make_body(256 * 1024)));

    DownloadEngine engine(single_task_config());
    engine.register_handler(protocols::create_http_handler());

    // 无全局限速，任务自身限速 64KB/s（既有通道，回归验证取严逻辑不破坏它）
    DownloadOptions options;
    options.output_directory = temp_output_dir();
    options.output_filename = "own_limited.bin";
    options.max_connections = 1;
    options.speed_limit = 64 * 1024;
    std::filesystem::create_directories(options.output_directory);

    auto task = engine.add_task(server.url("/own_limited.bin"), options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    const auto begin = std::chrono::steady_clock::now();
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(30)));
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin).count();

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_GE(elapsed_ms, 2500)
        << "任务自身限速回归：应至少耗时约 4s，实际 " << elapsed_ms << "ms";

    engine.cancel_all();
    server.stop();
    std::filesystem::remove_all(options.output_directory);
}
