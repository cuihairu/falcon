/**
 * @file download_engine_v2_pause_test.cpp
 * @brief V2 引擎真暂停端到端测试（入口守卫 + 暂停清扫 + 状态守卫）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 暂停正确性的三位一体不变量：
 * - 暂停停数据流：PAUSED 后字节不再增长，挂起中的连接被清扫收走
 *   （服务器侧观察到 EOF——进程存活期间 fd 未关闭即观察不到），
 *   清扫检查点固化断点，恢复后带 Range 从断点继续、成品一致
 * - 暂停组不被超时清理误杀：任务超时周期照常运转，PAUSED 组保持
 *   PAUSED（sweep 漏收挂起命令或 fail_group 状态守卫缺失任一环节
 *   失守，组都会被改写成 FAILED）
 * - pause_group 语义：幂等；终态组拒绝暂停
 *
 * 测试服务器分角色编排：0 号连接慢速发送（给主线程留暂停观察窗口，
 * 客户端断开即 sweep 生效证据），后续连接按 Range 特征正常应答
 * （206 续传 / 200 全量）。
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
#include <falcon/protocols/resume_control.hpp>

#include <atomic>
#include <chrono>
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
void ensure_winsock_for_pause_test() {
    static bool initialized = false;
    if (!initialized) {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
}

inline int pause_test_getpid() { return _getpid(); }
using sock_len = int;
using recv_ssize = int;
#else
inline int pause_test_getpid() { return static_cast<int>(::getpid()); }
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
            (std::string("falcon_v2_pause_") + tag + "_" +
             std::to_string(pause_test_getpid())))
        .string();
}

/**
 * @brief 暂停测试服务器
 *
 * 0 号连接：200 头 + 分块慢速发送（chunk_bytes/interval 给主线程留
 * 暂停窗口）；客户端断开（send 失败）记录为 swept 观测——真暂停下
 * 这是清扫收走连接的直接证据。
 * 后续连接：带 Range → 206 从起点续发；无 Range → 200 全量快发
 * （恢复后的续传/重下路径）。
 */
class PauseTestServer {
public:
    ~PauseTestServer() { stop(); }  // RAII：joinable 线程析构即 terminate

    bool start(std::string body, std::size_t chunk_bytes, int interval_ms) {
#ifdef _WIN32
        ensure_winsock_for_pause_test();
#endif
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body_ = std::move(body);
        }
        chunk_bytes_ = chunk_bytes;
        interval_ms_ = interval_ms;

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

    // ---- 观测值 -----------------------------------------------------

    /// 慢速连接上观察到客户端断开（sweep 关闭 fd 的证据）
    bool client_swept() const { return client_swept_.load(); }

    /// 慢速连接已完整发完（未在传输中途暂停的对照观测）
    bool first_served_fully() const { return first_served_fully_.load(); }

    int connections_accepted() const { return connections_.load(); }

    /// 后续连接收到的 Range 起点（断点续传证据）
    std::vector<Bytes> range_starts_snapshot() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return range_starts_;
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
            connections_.fetch_add(1);
            conn_threads_.emplace_back([this, conn] { serve(conn); });
        }
    }

    void serve(int conn) {
        const int conn_idx = conn_seq_.fetch_add(1);  // serve 线程私有序号
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

        std::string body;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            body = body_;
        }

        // Range 头简易解析（"bytes=N-" 或 "bytes=N-M"）
        Bytes range_start = 0;
        bool has_range = false;
        {
            const auto pos = request.find("Range: bytes=");
            const auto pos2 = request.find("range: bytes=");
            const auto at = pos != std::string::npos ? pos : pos2;
            if (at != std::string::npos) {
                std::size_t i = at + std::string("Range: bytes=").size();
                while (i < request.size() &&
                       (request[i] < '0' || request[i] > '9')) {
                    ++i;
                }
                std::size_t value = 0;
                while (i < request.size() && request[i] >= '0' && request[i] <= '9') {
                    value = value * 10 + static_cast<std::size_t>(request[i] - '0');
                    ++i;
                }
                range_start = value;
                has_range = true;
            }
        }

        const std::string common =
            "Content-Type: application/octet-stream\r\n"
            "Accept-Ranges: bytes\r\n"
            "ETag: \"pause-test\"\r\n"
            "Connection: close\r\n";

        if (conn_idx == 0 && !has_range && chunk_bytes_ > 0) {
            // 0 号连接：慢速发送——客户端中途断开即 swept 观测
            std::string header =
                "HTTP/1.1 200 OK\r\n" + common +
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            if (!send_all(conn, header.data(), header.size())) {
                client_swept_ = true;
                CLOSE_SOCKET(conn);
                return;
            }
            for (std::size_t off = 0; off < body.size(); off += chunk_bytes_) {
                const std::size_t n = std::min(chunk_bytes_, body.size() - off);
                if (!send_all(conn, body.data() + off, n)) {
                    client_swept_ = true;  // send 失败 = 客户端已断开
                    CLOSE_SOCKET(conn);
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
            }
            first_served_fully_ = true;
            CLOSE_SOCKET(conn);
            return;
        }

        if (has_range) {
            std::lock_guard<std::mutex> lock(mutex_);
            range_starts_.push_back(range_start);
        }

        if (has_range && range_start < body.size()) {
            const std::string slice = body.substr(static_cast<std::size_t>(range_start));
            std::string header =
                "HTTP/1.1 206 Partial Content\r\n" + common +
                "Content-Range: bytes " + std::to_string(range_start) + "-" +
                std::to_string(body.size() - 1) + "/" +
                std::to_string(body.size()) + "\r\n"
                "Content-Length: " + std::to_string(slice.size()) + "\r\n\r\n";
            send_all(conn, header.data(), header.size());
            send_all(conn, slice.data(), slice.size());
            CLOSE_SOCKET(conn);
            return;
        }

        std::string header =
            "HTTP/1.1 200 OK\r\n" + common +
            "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        send_all(conn, header.data(), header.size());
        send_all(conn, body.data(), body.size());
        CLOSE_SOCKET(conn);
    }

    bool send_all(int conn, const char* data, std::size_t size) {
        std::size_t sent = 0;
        while (sent < size) {
            // MSG_NOSIGNAL：客户端断开后继续 send 会触发 SIGPIPE
            // （暂停清扫场景必然发生），必须按错误返回而非杀进程
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
            if (n <= 0) return false;
            sent += static_cast<std::size_t>(n);
        }
        return true;
    }

    mutable std::mutex mutex_;
    std::string body_;
    std::vector<Bytes> range_starts_;
    std::size_t chunk_bytes_ = 0;
    int interval_ms_ = 0;

    std::atomic<bool> client_swept_{false};
    std::atomic<bool> first_served_fully_{false};
    std::atomic<int> connections_{0};
    std::atomic<int> conn_seq_{0};

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

/// 轮询等待条件成立（10ms 步进；超时返回 false）
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

/// 等待任务组到达终态；超时返回 false
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

/// 引擎线程 RAII 守卫：ASSERT 失败提前退出测试时仍正确停机并 join
///（joinable thread 析构即 terminate，会掩盖真实断言失败）
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

    /// 正常停机（ drain 路径）；析构兜底
    void shutdown_and_join() {
        engine_.shutdown();
        if (thread_.joinable()) thread_.join();
    }

private:
    DownloadEngineV2& engine_;
    std::thread thread_;
};

} // namespace

/// 真暂停主路径：传输中途暂停 → 字节冻结 + 服务器观察到断开（sweep
/// 收走挂起连接 + 检查点固化断点）→ 恢复后 206 从断点续传，成品一致
TEST(DownloadEngineV2Pause, PauseStopsFlowSweepsConnectionAndResumes) {
    const std::size_t kBodySize = 256 * 1024;
    const std::string body = make_body(kBodySize);
    PauseTestServer server;
    // 8KB/15ms → 32 块 ≈ 480ms 传输窗口；观察点 32KB（4 块 ≈ 60ms）
    ASSERT_TRUE(server.start(body, 8 * 1024, 15));

    const std::string dir = temp_dir_for("flow");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "flow.bin").string();
    const std::string temp_path = out_path + ".falcon.tmp";

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.enable_disk_cache = false;  // 直写：主线程可从临时文件观察进度
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(server.url("/flow.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);

    // 等下载推进到中段（临时文件出现并过观察点）。file_size 失败时
    // 返回 uint64max 且只设 ec——必须显式检查，否则文件不存在也判真
    ASSERT_TRUE(wait_for(
        [&] {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(temp_path, ec);
            return !ec && sz > 32 * 1024;
        },
        15000))
        << "下载未在时限内推进到观察点";

    // ---- 暂停 --------------------------------------------------------
    ASSERT_TRUE(engine.pause_task(task_id));
    ASSERT_TRUE(wait_for(
        [&] { return group->status() == RequestGroupStatus::PAUSED; }, 5000));
    ASSERT_TRUE(wait_for([&] { return server.client_swept(); }, 5000))
        << "暂停后服务器未观察到断开——挂起连接未被清扫收走";

    const Bytes paused_downloaded = group->downloaded_bytes();
    const auto paused_file_size = std::filesystem::file_size(temp_path);
    ASSERT_GT(paused_downloaded, 0u);
    ASSERT_EQ(paused_downloaded, paused_file_size)
        << "直写模式下组进度应与落盘尺寸一致";

    // 冻结断言：PAUSED 后字节不再增长
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(group->downloaded_bytes(), paused_downloaded)
        << "暂停后组进度仍在增长";
    EXPECT_EQ(std::filesystem::file_size(temp_path), paused_file_size)
        << "暂停后临时文件仍在增长";

    // 断点已固化到控制文件（pause 固化 + sweep 检查点补上报）
    std::error_code ctrl_ec;
    EXPECT_TRUE(std::filesystem::exists(out_path + kResumeControlExtension, ctrl_ec))
        << "暂停应固化断点控制文件";

    // ---- 恢复 --------------------------------------------------------
    ASSERT_TRUE(engine.resume_task(task_id));
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    runner.shutdown_and_join();
    server.stop();

    EXPECT_FALSE(server.first_served_fully())
        << "慢速连接应被中途截断而非完整发完";
    ASSERT_FALSE(server.range_starts_snapshot().empty())
        << "恢复连接应携带断点 Range";
    EXPECT_EQ(server.range_starts_snapshot().front(), paused_downloaded)
        << "续传起点应等于暂停时的落盘进度";

    EXPECT_EQ(read_file_content(out_path), body) << "成品逐字节一致";
    EXPECT_FALSE(std::filesystem::exists(temp_path));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 暂停组不被超时清理误杀：任务超时（2s）后继续等待 3.5s，PAUSED 组
/// 必须仍是 PAUSED——sweep 漏收挂起命令或 fail_group 状态守卫缺失，
/// 超时清理都会把组改写成 FAILED（复合不变量锁定）
TEST(DownloadEngineV2Pause, TimeoutCleanupDoesNotKillPausedGroup) {
    PauseTestServer server;
    // 慢发同样适用：黑洞语义由"暂停窗口足够长"替代——连接建立后命令
    // 挂起等数据（每块间隔远超任务超时即等效黑洞）
    ASSERT_TRUE(server.start(make_body(64 * 1024), 2 * 1024, 2000));

    const std::string dir = temp_dir_for("timeout");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "slow.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    options.timeout_seconds = 2;  // 任务级超时 2s

    const TaskId task_id = engine.add_download(server.url("/slow.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);

    // 连接已建立（命令进入挂起等响应/数据）
    ASSERT_TRUE(wait_for(
        [&] { return server.connections_accepted() >= 1; }, 10000));

    // 在首个 2s 超时到点前暂停：连接挂起中 → sweep 必须收走
    ASSERT_TRUE(engine.pause_task(task_id));
    ASSERT_TRUE(wait_for(
        [&] { return group->status() == RequestGroupStatus::PAUSED; }, 5000));

    // 跨过任务超时（2s）继续等待：总 3.5s——若 sweep 漏收或守卫缺失，
    // 超时清理会在 2s 点触发 fail_group_of_command 把 PAUSED 组改写
    // 成 FAILED
    std::this_thread::sleep_for(std::chrono::milliseconds(3500));
    EXPECT_EQ(group->status(), RequestGroupStatus::PAUSED)
        << "暂停组被超时清理误杀";

    runner.shutdown_and_join();
    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// pause_group 语义：幂等（已暂停返回 true）；终态组拒绝暂停
TEST(DownloadEngineV2Pause, PauseGroupIdempotentAndTerminalRejected) {
    const std::string body = make_body(16 * 1024);
    PauseTestServer server;
    ASSERT_TRUE(server.start(body, 0, 0));  // 不慢发：快发直完成

    const std::string dir = temp_dir_for("semantics");
    std::filesystem::create_directories(dir);
    const std::string out_path = (std::filesystem::path(dir) / "done.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;

    const TaskId task_id = engine.add_download(server.url("/done.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    // 终态组拒绝暂停；不存在的任务同样拒绝
    EXPECT_FALSE(engine.pause_task(task_id));
    EXPECT_FALSE(engine.pause_task(task_id + 999));

    runner.shutdown_and_join();
    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 批次 V：重试等待窗口内暂停——HttpRetryCommand 以 NEED_RETRY 回队
/// 轮询到 retry_delay 到点，期间任务组被暂停：retry 命令下一轮 execute
/// 的 PAUSED 入口守卫必须静默收口（不再续建重试链），组保持 PAUSED
/// 而非被改写为 FAILED
TEST(DownloadEngineV2Pause, PauseDuringRetryDelayWindowKeepsGroupPaused) {
    // 端口 1 无监听：连接立即拒绝 → 连接失败 → HttpRetryCommand 入队
    // 等 retry_delay_seconds（3600s 保证窗口不耗尽）
    const std::string dir = temp_dir_for("retrywait");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "retry.bin").string();

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 2;
    options.retry_delay_seconds = 3600;

    const TaskId task_id =
        engine.add_download("http://127.0.0.1:1/wait.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);

    // 连接拒绝即时发生；留 500ms 让 HttpRetryCommand 完成入队
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    ASSERT_TRUE(engine.pause_task(task_id));
    ASSERT_TRUE(wait_for(
        [&] { return group->status() == RequestGroupStatus::PAUSED; }, 5000));

    // retry 命令在下一轮轮询时命中 PAUSED 守卫静默退出；多留窗口确保
    // 至少一轮 execute 发生，随后停机（PAUSED 视为已安顿）
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    runner.shutdown_and_join();

    EXPECT_EQ(group->status(), RequestGroupStatus::PAUSED)
        << "重试窗口内暂停不得被改写为终态";
    EXPECT_TRUE(group->error_message().empty())
        << "暂停语义非失败，不得遗留错误消息";

    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 批次 V：多段任务同引擎 pause→resume——恢复的初始连接带断点 Range
/// 收到 206 后，determine_download_strategy 经 has_resume_state 进入
/// schedule_resume_download；组保持 multi_segment 标志触发防御分支：
/// abandon_resume（丢弃断点）+ 调度全新无 Range 下载。此前该分支的
/// 唯一入口（同引擎多段任务暂停恢复）从未被测试执行。
/// 路径铁证：resume 后的连接序列中出现无 Range 的全新初始连接
/// （单连接续传的恢复连接必带 Range，见 PauseStopsFlow…用例）。
TEST(DownloadEngineV2Pause, MultiSegmentPauseResumeAbandonsForFreshDownload) {
    const std::size_t kBodySize = 256 * 1024;
    const std::string body = make_body(kBodySize);
    PauseTestServer server;
    // 0 号连接（多段初始连接，无 Range）慢发留暂停窗口；段连接快发
    ASSERT_TRUE(server.start(body, 8 * 1024, 15));

    const std::string dir = temp_dir_for("multiresume");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "multi.bin").string();
    const std::string temp_path = out_path + ".falcon.tmp";

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.enable_disk_cache = false;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 2;
    options.max_retries = 0;
    options.min_segment_size = 64 * 1024;

    const TaskId task_id =
        engine.add_download(server.url("/multi.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);

    // 推进到多段传输中（临时文件过观察点 = 段 0 慢发推进中）
    ASSERT_TRUE(wait_for(
        [&] {
            std::error_code ec;
            const auto sz = std::filesystem::file_size(temp_path, ec);
            return !ec && sz > 32 * 1024;
        },
        15000))
        << "多段下载未在时限内推进到观察点";

    // 暂停：sweep 收走全部连接，断点固化
    ASSERT_TRUE(engine.pause_task(task_id));
    ASSERT_TRUE(wait_for(
        [&] { return group->status() == RequestGroupStatus::PAUSED; }, 5000));
    ASSERT_TRUE(wait_for([&] { return server.client_swept(); }, 5000));

    // 多段组形成铁证：暂停前已有段连接携带 Range 收到 206
    const auto ranges_before = server.range_starts_snapshot().size();
    ASSERT_GT(ranges_before, 0u)
        << "未观察到段连接 Range——组未进入多段形态，用例前提不成立";
    const auto accepted_before =
        static_cast<std::size_t>(server.connections_accepted());

    // 恢复：初始连接带断点 Range → 206 → schedule_resume_download →
    // is_multi_segment 防御 → abandon → 全新无 Range 下载直至完成
    ASSERT_TRUE(engine.resume_task(task_id));
    ASSERT_TRUE(wait_group_terminal(engine, group, 60000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    const auto accepted_after =
        static_cast<std::size_t>(server.connections_accepted());
    const auto ranges_after = server.range_starts_snapshot().size();
    EXPECT_GT(accepted_after, accepted_before)
        << "恢复后必须有新连接（abandon 全新下载）";
    EXPECT_GT(accepted_after - accepted_before,
              ranges_after - ranges_before)
        << "恢复后的新连接中必须存在无 Range 的全新初始连接"
           "（带 Range 的仅是 abandon 前的断点探测连接）";

    EXPECT_EQ(read_file_content(out_path), body) << "成品逐字节一致";
    EXPECT_FALSE(std::filesystem::exists(temp_path));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 批次 W：sweep_task_connections 的跨任务 continue 分支——清扫 B 时
/// 等待表里的 A 挂起命令必须跳过（只收走目标任务自己的命令）。A 全程
/// 不受反复清扫影响：慢发照常推进、成品逐字节一致
TEST(DownloadEngineV2Pause, SweepSkipsOtherTasksPendingCommands) {
    const std::size_t kBodySize = 128 * 1024;
    const std::string body = make_body(kBodySize);
    PauseTestServer server;
    // 4KB/10ms → 32 块 ≈ 320ms 慢发窗口，期间 A 命令绝大部分时间挂起
    ASSERT_TRUE(server.start(body, 4 * 1024, 10));

    const std::string dir = temp_dir_for("sweepskip");
    std::filesystem::create_directories(dir);

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options_a;
    options_a.output_filename = (std::filesystem::path(dir) / "a.bin").string();
    options_a.max_connections = 1;
    options_a.max_retries = 0;
    const TaskId task_a = engine.add_download(server.url("/a.bin"), options_a);
    ASSERT_GT(task_a, 0u);
    auto* group_a = engine.request_group_man()->find_group(task_a);
    ASSERT_NE(group_a, nullptr);

    // B 连端口 1 立即失败（重试 0）→ 快速终态，等待表里只剩 A 的命令
    DownloadOptions options_b;
    options_b.output_filename = (std::filesystem::path(dir) / "b.bin").string();
    options_b.max_retries = 0;
    const TaskId task_b = engine.add_download("http://127.0.0.1:1/b.bin", options_b);
    ASSERT_GT(task_b, 0u);

    EngineRunner runner(engine);

    // A 慢发期间反复清扫 B：每轮扫描等待表都遇到 A 的挂起命令并跳过
    for (int i = 0; i < 30; ++i) {
        engine.sweep_task_connections(task_b);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // A 不被误收：照常完成、成品一致
    ASSERT_TRUE(wait_group_terminal(engine, group_a, 10'000));
    EXPECT_EQ(group_a->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(read_file_content((std::filesystem::path(dir) / "a.bin").string()),
              body);

    runner.shutdown_and_join();
    server.stop();
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}

/// 批次 W+：迟到清扫不得误杀 resume 激活的新命令（cutoff 过滤钉子）。
/// CI 红面根因（metalink 桥接 V2PauseThenResume 120s Timeout）：pause
/// 投递的清扫命令若在 resume 之后才执行（引擎队列积压/线程调度），
/// 按旧语义会按 task 无差别收走等待表里全部命令——包括 resume 激活
/// 的新链命令。组 ACTIVE 但再无命令推进，且命令已摘出等待表、超时
/// 清理扫不到，上层永远等不到终态。修复后清扫只收进入等待表早于
/// 投递时刻（cutoff）的命令；本用例以 pause 之前的时刻为 cutoff
/// 模拟迟到清扫，断言恢复链照常完成
TEST(DownloadEngineV2Pause, LateSweepAfterResumeSparesNewCommands) {
    const std::size_t kBodySize = 64 * 1024;
    const std::string body = make_body(kBodySize);
    PauseTestServer server;
    // 4KB/10ms → 16 块 ≈ 160ms 慢发窗口，命令绝大部分时间挂起
    ASSERT_TRUE(server.start(body, 4 * 1024, 10));

    const std::string dir = temp_dir_for("latesweep");
    std::filesystem::create_directories(dir);
    const std::string out_path =
        (std::filesystem::path(dir) / "late.bin").string();
    const std::string temp_path = out_path + ".falcon.tmp";

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    const TaskId task_id = engine.add_download(server.url("/late.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    EngineRunner runner(engine);

    // 等首笔进度（旧链命令进入挂起等待）
    ASSERT_TRUE(wait_for([&] { return group->downloaded_bytes() > 0; }, 15000))
        << "下载未在时限内推进";

    const auto accepted_before = server.connections_accepted();

    // cutoff 取 pause 之前——模拟「投递时刻在 pause 前后、执行却在
    // resume 之后」的迟到清扫
    const auto late_cutoff = std::chrono::steady_clock::now();
    ASSERT_TRUE(engine.pause_task(task_id));
    // 立即恢复：与清扫竞速（CI 红面时序——resume 先于清扫命令执行）
    ASSERT_TRUE(engine.resume_task(task_id));

    // 等恢复链的新连接被服务器接受：此刻新链命令已注册
    ASSERT_TRUE(wait_for(
                   [&] {
                       return server.connections_accepted() > accepted_before;
                   },
                   15000))
        << "resume 后未观察到新连接";
    // 再给一拍：让新命令从执行队列落进等待表（连接建立/收头间隙）
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 迟到清扫：cutoff 早于新命令的挂起时刻——按修复后语义只收旧链
    // 命令，resume 后注册的新命令必须全部存活（旧语义在此处误杀：
    // 组再无命令推进，wait_group_terminal 超时红）
    engine.sweep_task_connections(task_id, late_cutoff);

    ASSERT_TRUE(wait_group_terminal(engine, group, 30'000));
    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();

    runner.shutdown_and_join();
    server.stop();

    EXPECT_EQ(read_file_content(out_path), body) << "成品逐字节一致";
    EXPECT_FALSE(std::filesystem::exists(temp_path));
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec);
}
