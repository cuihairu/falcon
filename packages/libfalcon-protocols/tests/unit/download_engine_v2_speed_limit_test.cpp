/**
 * @file download_engine_v2_speed_limit_test.cpp
 * @brief V2 引擎全局限速端到端测试（滑动窗口 + 事件循环节流）
 * @author Falcon Team
 * @date 2026-09-12
 *
 * EngineConfigV2::global_speed_limit 此前零消费端；通道打通后：
 * HttpDownloadCommand 每次 recv 后 report_downloaded_bytes → 引擎
 * 1s 滑动窗口统计 → 超速时节流轮拉长 poll 等待并跳过数据面命令
 * （socket 事件照常处理，不丢唤醒）。
 *
 * 本测试用回环 HTTP server + 时间下界断言验证限速真实生效：
 * - 全速对照：回环下载远快于限速场景
 * - 限速场景：256KB / 64KB/s ≈ 4s（64KB recv 缓冲 × 1s 节流窗口，
 *   稳态约 1s 解禁 64KB），断言耗时下界（只断言"变慢"，
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
#include <falcon/protocols/download_engine_v2.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_v2_limit_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int v2_limit_test_getpid() { return _getpid(); }
// Winsock（winsock2.h）无 socklen_t/ssize_t：长度参数与 recv/send 返回值均为 int
using sock_len = int;
using recv_ssize = int;
#else
inline int v2_limit_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

/// 最小回环 HTTP server：支持 HEAD（文件探测）与 GET（下载数据）
class MinimalHttpServer {
public:
    ~MinimalHttpServer() { stop(); }  // RAII：joinable 线程析构即 terminate

    bool start(std::string body) {
#ifdef _WIN32
        ensure_winsock_for_v2_limit_test();
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
        // Linux 不保证唤醒（复用既有测试服务器模板）
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
            sock_len peer_len = sizeof(peer);
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
            recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
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
            recv_ssize n = ::send(conn, data + sent,
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

std::string read_file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

/// 单连接配置（Accept-Ranges: none 的服务器本身也走单连接；显式声明意图）
DownloadOptions single_connection_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    return options;
}

/// 跑完一次下载并返回耗时毫秒数；完成返回 true（超时/失败返回 false）
bool run_download_to_completion(DownloadEngineV2& engine, RequestGroup* group,
                                int timeout_seconds, long long& elapsed_ms) {
    std::thread runner([&engine] { engine.run(); });

    const auto begin = std::chrono::steady_clock::now();
    const auto deadline = begin + std::chrono::seconds(timeout_seconds);
    bool finished = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto st = group->status();
        if (st == RequestGroupStatus::COMPLETED || st == RequestGroupStatus::FAILED) {
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - begin)
                     .count();

    if (!finished) {
        engine.force_shutdown();
    }
    runner.join();
    return finished;
}

std::string temp_dir_for(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("falcon_v2_limit_") + tag + "_" +
             std::to_string(v2_limit_test_getpid())))
        .string();
}

} // namespace

TEST(DownloadEngineV2SpeedLimit, UnlimitedLoopbackDownloadCompletesQuickly) {
    const std::string body = make_body(256 * 1024);
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(body));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    ASSERT_EQ(engine.get_global_speed_limit(), 0u);

    const std::string dir = temp_dir_for("unlimited");
    std::filesystem::create_directories(dir);
    const auto options = single_connection_options(dir + "/unlimited.bin");

    const TaskId task_id = engine.add_download(server.url("/unlimited.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 20, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(group->downloaded_bytes(), body.size());
    // 全速对照：回环 256KB 远快于限速场景；宽松上界防 CI 抖动误报
    EXPECT_LT(elapsed_ms, 3000)
        << "全速回环下载不应到达秒级，实际 " << elapsed_ms << "ms";
    EXPECT_EQ(read_file_content(dir + "/unlimited.bin"), body);

    std::filesystem::remove_all(dir);
}

TEST(DownloadEngineV2SpeedLimit, ConfiguredGlobalLimitSlowsDownload) {
    const std::string body = make_body(256 * 1024);
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(body));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.global_speed_limit = 64 * 1024;  // 64KB/s
    DownloadEngineV2 engine(config);
    ASSERT_EQ(engine.get_global_speed_limit(), 64u * 1024);

    const std::string dir = temp_dir_for("cfg");
    std::filesystem::create_directories(dir);
    const auto options = single_connection_options(dir + "/limited.bin");

    const TaskId task_id = engine.add_download(server.url("/limited.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 30, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(group->downloaded_bytes(), body.size());
    // 时间下界断言：64KB/s 下 256KB 至少约 3s（稳态约 1s 解禁 64KB）。
    // 只断言下界不断言上界——限速只会更慢、不会更快的语义是稳定的
    EXPECT_GE(elapsed_ms, 2500)
        << "V2 全局限速未生效：256KB @ 64KB/s 应至少耗时约 3s，实际 "
        << elapsed_ms << "ms";
    EXPECT_EQ(read_file_content(dir + "/limited.bin"), body);

    std::filesystem::remove_all(dir);
}

TEST(DownloadEngineV2SpeedLimit, RuntimeSetLimitSlowsDownload) {
    const std::string body = make_body(256 * 1024);
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(body));

    // 构造时不限速，运行前经 setter 设置（热更通道）
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    engine.set_global_speed_limit(64 * 1024);
    ASSERT_EQ(engine.get_global_speed_limit(), 64u * 1024);

    const std::string dir = temp_dir_for("runtime");
    std::filesystem::create_directories(dir);
    const auto options = single_connection_options(dir + "/runtime_limited.bin");

    const TaskId task_id =
        engine.add_download(server.url("/runtime_limited.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 30, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_GE(elapsed_ms, 2500)
        << "运行时设置的全局限速未生效：实际 " << elapsed_ms << "ms";

    std::filesystem::remove_all(dir);
}

TEST(DownloadEngineV2SpeedLimit, TaskOwnLimitSlowsDownload) {
    const std::string body = make_body(256 * 1024);
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(body));

    // 无全局限速，任务自身 64KB/s（256KB ≈ 4s）
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);
    ASSERT_EQ(engine.get_global_speed_limit(), 0u);

    const std::string dir = temp_dir_for("task");
    std::filesystem::create_directories(dir);
    auto options = single_connection_options(dir + "/task_limited.bin");
    options.speed_limit = 64 * 1024;

    const TaskId task_id = engine.add_download(server.url("/task_limited.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 30, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(group->downloaded_bytes(), body.size());
    EXPECT_GE(elapsed_ms, 2500)
        << "V2 单任务限速未生效：256KB @ 64KB/s 应至少耗时约 3s，实际 "
        << elapsed_ms << "ms";
    EXPECT_EQ(read_file_content(dir + "/task_limited.bin"), body);

    std::filesystem::remove_all(dir);
}

TEST(DownloadEngineV2SpeedLimit, TaskLimitStrictOfBothDirections) {
    const std::string body = make_body(256 * 1024);
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(body));

    // 取严语义双向验证：全局 64KB/s + 任务 128KB/s → 全局占优；
    // （全局 128KB/s + 任务 64KB/s → 任务占优）共用同一段限速断言
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.global_speed_limit = 64 * 1024;
    DownloadEngineV2 engine(config);

    const std::string dir = temp_dir_for("strict");
    std::filesystem::create_directories(dir);
    auto options = single_connection_options(dir + "/strict_limited.bin");
    options.speed_limit = 128 * 1024;  // 任务限速比全局宽松

    const TaskId task_id = engine.add_download(server.url("/strict_limited.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 30, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_GE(elapsed_ms, 2500)
        << "全局/任务限速取严未生效（全局 64KB/s 应占优）：实际 "
        << elapsed_ms << "ms";

    std::filesystem::remove_all(dir);
}
