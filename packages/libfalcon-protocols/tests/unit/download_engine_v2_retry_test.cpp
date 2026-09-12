/**
 * @file download_engine_v2_retry_test.cpp
 * @brief V2 引擎连接级重试链端到端测试（max_retries + retry_delay_seconds）
 * @author Falcon Team
 * @date 2026-09-12
 *
 * 修复的两个缺陷：
 * - HttpRetryCommand 此前在引擎事件循环线程里 sleep_for——一个任务
 *   重试时整个引擎停摆；且该命令是孤儿（生产路径零创建），V2 HTTP
 *   下载失败根本没有重试，max_retries/retry_delay_seconds 零消费
 * - 初始连接失败后无人给任务组标终态——任务悬空 Downloading、
 *   all_completed 永不成立、run() 永不退出
 *
 * 重试链：连接失败（connect/响应头阶段断连）→ HttpRetryCommand 以
 * NEED_RETRY 回队轮询到 retry_delay_seconds 到点 → 重新 initiate；
 * 多连接意图的任务不参与（分段失败语义不同）；重试耗尽收口 FAILED。
 *
 * 测试服务器：前 N 次连接立即关闭（瞬时故障），之后正常响应；
 * accept 计数可精确断言重试次数。时间断言沿用"只断言下界"模式
 * 防 CI 抖动误报。
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
void ensure_winsock_for_retry_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int retry_test_getpid() { return _getpid(); }
// Winsock（winsock2.h）无 socklen_t/ssize_t：长度参数与 recv/send 返回值均为 int
using sock_len = int;
using recv_ssize = int;
#else
inline int retry_test_getpid() { return static_cast<int>(::getpid()); }
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

/// 测试 HTTP 服务器：前 fail_first_n 次连接立即关闭（瞬时故障模拟），
/// 之后正常响应。accepted() 返回连接建立总数（含被立即关闭的），
/// 用于断言重试次数
class FlakyServer {
public:
    explicit FlakyServer(std::size_t fail_first_n = 0)
        : fail_first_n_(fail_first_n) {}

    ~FlakyServer() { stop(); }  // RAII：joinable 线程析构即 terminate

    bool start(std::string body) {
#ifdef _WIN32
        ensure_winsock_for_retry_test();
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
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

    int port() const { return port_; }
    int accepted() const { return accepted_.load(); }
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
            accepted_.fetch_add(1);
            conn_threads_.emplace_back([this, conn] { serve(conn); });
        }
    }

    void serve(int conn) {
        // 瞬时故障窗口内的连接：立即关闭（引擎侧表现为响应头阶段断连）
        if (accepted_.load() <= static_cast<int>(fail_first_n_)) {
            CLOSE_SOCKET(conn);
            return;
        }

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

        std::string header = "HTTP/1.1 200 OK\r\n"
                             "Content-Type: application/octet-stream\r\n"
                             "Content-Length: " + std::to_string(body_.size()) + "\r\n"
                             "Accept-Ranges: none\r\n"
                             "Connection: close\r\n\r\n";
        send_all(conn, header.data(), header.size());
        send_all(conn, body_.data(), body_.size());
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

    std::size_t fail_first_n_;
    std::string body_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> accepted_{0};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

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

/// 单连接下载选项（连接级重试只覆盖单连接任务）
DownloadOptions single_connection_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    return options;
}

/// 跑完一次下载；返回是否在时限内观察到终态（超时强制停机）
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
            (std::string("falcon_v2_retry_") + tag + "_" +
             std::to_string(retry_test_getpid())))
        .string();
}

} // namespace

/// 瞬时故障后恢复：第 1 次连接被服务器立即关闭，第 2 次成功下载
/// （重试链的真实价值——此前 V2 一次失败即终态）
TEST(DownloadEngineV2Retry, RetrySucceedsAfterTransientFailure) {
    const std::string body = make_body(32 * 1024);
    FlakyServer server(/*fail_first_n=*/1);
    ASSERT_TRUE(server.start(body));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const std::string dir = temp_dir_for("recover");
    std::filesystem::create_directories(dir);
    auto options = single_connection_options(dir + "/recover.bin");
    options.max_retries = 3;
    options.retry_delay_seconds = 0;  // 聚焦重试链路本身，不计延迟

    const TaskId task_id = engine.add_download(server.url("/recover.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 20, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(group->downloaded_bytes(), body.size());
    // 恰好重试一次：首连 + 1 次重试 = 2 次连接
    EXPECT_EQ(server.accepted(), 2)
        << "瞬时故障后应恰好重试一次，实际连接 " << server.accepted() << " 次";
    EXPECT_EQ(read_file_content(dir + "/recover.bin"), body);

    std::filesystem::remove_all(dir);
}

/// 持续故障：重试耗尽后任务组获得 FAILED 终态，连接次数 = 首连 + 重试数
TEST(DownloadEngineV2Retry, RetriesExhaustedFailsGroupWithExactAttempts) {
    const std::string body = make_body(1024);
    FlakyServer server(/*fail_first_n=*/100);  // 永远失败
    ASSERT_TRUE(server.start(body));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const std::string dir = temp_dir_for("exhaust");
    std::filesystem::create_directories(dir);
    auto options = single_connection_options(dir + "/exhaust.bin");
    options.max_retries = 2;
    options.retry_delay_seconds = 0;

    const TaskId task_id = engine.add_download(server.url("/exhaust.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 20, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    // 首连 + 2 次重试 = 3 次连接；修复前零重试（仅 1 次）
    EXPECT_EQ(server.accepted(), 3)
        << "max_retries=2 应恰好尝试 3 次，实际 " << server.accepted() << " 次";

    std::filesystem::remove_all(dir);
}

/// retry_delay_seconds 消费验证：每次重试间隔该秒数（时间下界断言）
TEST(DownloadEngineV2Retry, RetryDelaySecondsConsumedBetweenAttempts) {
    const std::string body = make_body(1024);
    FlakyServer server(/*fail_first_n=*/100);
    ASSERT_TRUE(server.start(body));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const std::string dir = temp_dir_for("delay");
    std::filesystem::create_directories(dir);
    auto options = single_connection_options(dir + "/delay.bin");
    options.max_retries = 2;
    options.retry_delay_seconds = 1;  // 2 次重试各等 1s

    const TaskId task_id = engine.add_download(server.url("/delay.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 20, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_EQ(server.accepted(), 3);
    // 下界断言：2 次重试各隔 1s；不设上界防 CI 抖动误报
    EXPECT_GE(elapsed_ms, 2000)
        << "retry_delay_seconds=1 未生效：2 次重试应至少间隔 2s，实际 "
        << elapsed_ms << "ms";

    std::filesystem::remove_all(dir);
}

/// 多连接任务不参与连接级重试（分段失败语义不同，保持一次终态）
TEST(DownloadEngineV2Retry, MultiConnectionTaskSkipsConnectionRetry) {
    const std::string body = make_body(1024);
    FlakyServer server(/*fail_first_n=*/100);
    ASSERT_TRUE(server.start(body));

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const std::string dir = temp_dir_for("multiconn");
    std::filesystem::create_directories(dir);
    DownloadOptions options;
    options.output_filename = dir + "/multiconn.bin";
    options.max_connections = 4;  // 多连接意图：不参与连接级重试
    options.max_retries = 3;
    options.retry_delay_seconds = 0;

    const TaskId task_id = engine.add_download(server.url("/multiconn.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 20, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_EQ(server.accepted(), 1)
        << "多连接任务不应参与连接级重试，实际连接 " << server.accepted() << " 次";

    std::filesystem::remove_all(dir);
}

/// 连接被拒绝（无服务器）：修复前组悬空 Downloading、run() 永不退出；
/// 修复后重试耗尽获得 FAILED 终态
TEST(DownloadEngineV2Retry, ConnectionRefusedReachesFailedTerminalState) {
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const std::string dir = temp_dir_for("refused");
    std::filesystem::create_directories(dir);
    auto options = single_connection_options(dir + "/refused.bin");
    options.max_retries = 1;
    options.retry_delay_seconds = 0;

    // 127.0.0.1:1 保留端口：连接拒绝
    const TaskId task_id = engine.add_download("http://127.0.0.1:1/refused.bin",
                                               options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(run_download_to_completion(engine, group, 20, elapsed_ms))
        << "连接拒绝的任务应获得终态（修复前永久悬空）";

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_GE(elapsed_ms, 0);

    std::filesystem::remove_all(dir);
}
