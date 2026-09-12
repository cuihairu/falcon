/**
 * @file download_engine_v2_run_test.cpp
 * @brief DownloadEngineV2 事件循环（run）与命令调度覆盖测试
 * @author Falcon Team
 * @date 2026-09-05
 *
 * 通过自定义命令驱动 run() 主循环，覆盖：
 * - 例程命令周期执行与 shutdown 退出
 * - 命令挂起（socket 等待）→ 事件回调 → 恢复执行的完整链路
 * - 未注册事件的等待命令重新入队
 * - 运行中 add_download 的激活路径
 * 所有任务使用 127.0.0.1 保留端口，离线可运行。
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
#include <fcntl.h>
#include <poll.h>
#define CLOSE_SOCKET(fd) close(fd)
#define POLL(fd_ptr, count, timeout_ms) ::poll((fd_ptr), (count), (timeout_ms))
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/command.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#endif

using namespace falcon;

namespace {

#ifdef _WIN32
void ensure_winsock_for_run_test() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
// Winsock（winsock2.h）无 socklen_t/ssize_t：长度参数与 recv 返回值均为 int
using sock_len = int;
using recv_ssize = int;
#else
using sock_len = socklen_t;
using recv_ssize = ssize_t;
#endif

/// 创建一对已连接的非阻塞 Socket
std::array<int, 2> make_socket_pair_nb() {
#ifdef _WIN32
    ensure_winsock_for_run_test();

    SOCKET listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock == INVALID_SOCKET) return {-1, -1};

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    if (bind(listen_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listen_sock, 1) != 0) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    int len = sizeof(addr);
    if (getsockname(listen_sock, reinterpret_cast<struct sockaddr*>(&addr), &len) != 0) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    SOCKET client_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client_sock == INVALID_SOCKET) {
        closesocket(listen_sock);
        return {-1, -1};
    }

    if (connect(client_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listen_sock);
        closesocket(client_sock);
        return {-1, -1};
    }

    SOCKET server_sock = accept(listen_sock, NULL, NULL);
    closesocket(listen_sock);
    if (server_sock == INVALID_SOCKET) {
        closesocket(client_sock);
        return {-1, -1};
    }

    u_long mode = 1;
    ioctlsocket(client_sock, FIONBIO, &mode);
    ioctlsocket(server_sock, FIONBIO, &mode);

    return {static_cast<int>(client_sock), static_cast<int>(server_sock)};
#else
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return {-1, -1};
    }
    for (int i = 0; i < 2; ++i) {
        int flags = fcntl(fds[i], F_GETFL, 0);
        fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
    }
    return {fds[0], fds[1]};
#endif
}

/// 倒计时例程命令：每次被引擎周期执行时计数，归零后请求引擎关闭
class CountdownShutdownRoutine : public AbstractCommand {
public:
    explicit CountdownShutdownRoutine(int countdown)
        : AbstractCommand(0), countdown_(countdown) {}

    bool execute(DownloadEngineV2* engine) override {
        executions_++;
        if (engine && countdown_.fetch_sub(1) <= 1) {
            engine->shutdown();
        }
        return true;
    }

    const char* name() const override { return "CountdownShutdownRoutine"; }
    int executions() const { return executions_.load(); }

private:
    std::atomic<int> countdown_;
    std::atomic<int> executions_{0};
};

/// 立即完成的普通命令
///
/// 注意：命令对象在 run() 执行期间被引擎弹出并销毁，测试断言必须通过
/// 共享计数器读取结果，不能持有指向命令对象的裸指针（use-after-free）
class InstantCommand : public AbstractCommand {
public:
    explicit InstantCommand(std::shared_ptr<std::atomic<int>> counter)
        : AbstractCommand(0), counter_(std::move(counter)) {}
    bool execute(DownloadEngineV2*) override {
        counter_->fetch_add(1);
        return handle_result(ExecutionResult::OK);
    }
    const char* name() const override { return "InstantCommand"; }

private:
    std::shared_ptr<std::atomic<int>> counter_;
};

/// 挂起命令：第一次执行注册 socket 读事件并等待；事件就绪后第二次执行完成并关闭引擎
class SocketWaitCommand : public AbstractCommand {
public:
    SocketWaitCommand(int fd, std::shared_ptr<std::atomic<int>> counter)
        : AbstractCommand(0), fd_(fd), counter_(std::move(counter)) {}

    bool execute(DownloadEngineV2* engine) override {
        counter_->fetch_add(1);
        if (!engine) {
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }
        if (counter_->load() == 1) {
            if (!engine->register_socket_event(
                    fd_, static_cast<int>(net::IOEvent::READ), id())) {
                return handle_result(ExecutionResult::ERROR_OCCURRED);
            }
            mark_active();
            return false;  // 挂起等待事件
        }
        engine->shutdown();
        return handle_result(ExecutionResult::OK);
    }

    const char* name() const override { return "SocketWaitCommand"; }

private:
    int fd_;
    std::shared_ptr<std::atomic<int>> counter_;
};

/// 无事件注册的重试命令：前 N 次返回 false（引擎应重新入队），随后完成
class RequeueCommand : public AbstractCommand {
public:
    RequeueCommand(int false_rounds, std::shared_ptr<std::atomic<int>> counter)
        : AbstractCommand(0), false_rounds_(false_rounds), counter_(std::move(counter)) {}

    bool execute(DownloadEngineV2* engine) override {
        const int executions = counter_->fetch_add(1) + 1;
        if (executions <= false_rounds_) {
            mark_active();
            return false;  // 未注册 socket 事件 → 引擎应重新入队
        }
        if (engine) {
            engine->shutdown();
        }
        return handle_result(ExecutionResult::OK);
    }

    const char* name() const override { return "RequeueCommand"; }

private:
    int false_rounds_;
    std::shared_ptr<std::atomic<int>> counter_;
};

/// 运行期添加任务的例程命令：首次执行 add_download，随后关闭引擎
class AddDownloadRoutine : public AbstractCommand {
public:
    AddDownloadRoutine(const std::string& url, DownloadOptions options)
        : AbstractCommand(0), url_(url), options_(std::move(options)) {}

    bool execute(DownloadEngineV2* engine) override {
        executions_++;
        if (!engine) {
            return true;
        }
        if (executions_.load() == 1) {
            added_id_ = engine->add_download(url_, options_);
        } else {
            engine->shutdown();
        }
        return true;
    }

    const char* name() const override { return "AddDownloadRoutine"; }
    int executions() const { return executions_.load(); }
    TaskId added_id() const { return added_id_; }

private:
    std::string url_;
    DownloadOptions options_;
    std::atomic<int> executions_{0};
    TaskId added_id_ = INVALID_TASK_ID;
};

EngineConfigV2 fast_poll_config() {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 4;
    config.poll_timeout_ms = 10;  // 快速迭代
    return config;
}

EngineConfigV2 single_slot_config() {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 1;
    config.poll_timeout_ms = 10;
    return config;
}

/// 占位任务选项：连接失败后进入长时间挂起的连接级重试链，任务组
/// 保持 ACTIVE（连接失败现已正确终态化——组悬空 Downloading 曾是
/// 这些测试依赖的旧缺陷行为，修复后 all_completed 会提前结束 run()）
DownloadOptions keepalive_options() {
    DownloadOptions o;
    o.max_connections = 1;         // 单连接才有连接级重试
    o.max_retries = 1000;
    o.retry_delay_seconds = 3600;  // 重试等待期组保持非终态
    return o;
}

/// 抛异常的普通命令：抛出前计数，用于断言引擎在异常后仍然存活
class ThrowingCommand : public AbstractCommand {
public:
    ThrowingCommand(TaskId task_id, std::shared_ptr<std::atomic<int>> counter,
                    bool throw_non_std)
        : AbstractCommand(task_id), counter_(std::move(counter)),
          throw_non_std_(throw_non_std) {}

    bool execute(DownloadEngineV2*) override {
        counter_->fetch_add(1);
        if (throw_non_std_) {
            throw 42;  // 非 std::exception：驱动引擎 catch(...) 分支
        }
        throw std::runtime_error("boom");
    }

    const char* name() const override { return "ThrowingCommand"; }

private:
    std::shared_ptr<std::atomic<int>> counter_;
    bool throw_non_std_;
};

/// 每次执行都抛异常的例程命令
class ThrowingRoutine : public AbstractCommand {
public:
    explicit ThrowingRoutine(std::shared_ptr<std::atomic<int>> counter)
        : AbstractCommand(0), counter_(std::move(counter)) {}

    bool execute(DownloadEngineV2*) override {
        counter_->fetch_add(1);
        throw std::runtime_error("routine boom");
    }

    const char* name() const override { return "ThrowingRoutine"; }

private:
    std::shared_ptr<std::atomic<int>> counter_;
};

} // namespace

//==============================================================================
// run() 基础测试
//==============================================================================

TEST(DownloadEngineV2RunTest, RunWithNoTasksExitsImmediately) {
    DownloadEngineV2 engine(fast_poll_config());
    engine.run();  // 无任务：all_completed 为 true，应立即返回
    SUCCEED();
}

TEST(DownloadEngineV2RunTest, RunShutdownByRoutineCommand) {
    DownloadEngineV2 engine(fast_poll_config());

    // 保留端口上的回环地址：离线且连接快速失败，仅用于保持组处于未完成状态
    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);

    auto routine = std::make_unique<CountdownShutdownRoutine>(3);
    CountdownShutdownRoutine* routine_ptr = routine.get();
    engine.add_routine_command(std::move(routine));

    auto instant_counter = std::make_shared<std::atomic<int>>(0);
    engine.add_command(std::make_unique<InstantCommand>(instant_counter));

    engine.run();

    // 例程命令在循环中被周期执行；普通命令被执行一次后出队
    EXPECT_GE(routine_ptr->executions(), 3);
    EXPECT_EQ(instant_counter->load(), 1);
    EXPECT_TRUE(engine.is_shutdown_requested());
}

//==============================================================================
// socket 事件挂起/恢复测试
//==============================================================================

TEST(DownloadEngineV2RunTest, RunResumesParkedCommandOnSocketEvent) {
    DownloadEngineV2 engine(fast_poll_config());
    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);

    auto pair = make_socket_pair_nb();
    ASSERT_GE(pair[0], 0);
    ASSERT_GE(pair[1], 0);

    // 预先写入数据，使读事件立即可触发
    const char payload[] = "wake-up";
#ifdef _WIN32
    ASSERT_GT(send(pair[1], payload, static_cast<int>(sizeof(payload)), 0), 0);
#else
    ASSERT_GT(write(pair[1], payload, sizeof(payload)), 0);
#endif

    auto cmd_counter = std::make_shared<std::atomic<int>>(0);
    engine.add_command(std::make_unique<SocketWaitCommand>(pair[0], cmd_counter));

    engine.run();

    // 第一次执行挂起 → 事件回调恢复 → 第二次执行完成
    EXPECT_EQ(cmd_counter->load(), 2);

    CLOSE_SOCKET(pair[0]);
    CLOSE_SOCKET(pair[1]);
}

TEST(DownloadEngineV2RunTest, RunParksCommandWithoutEventForeverUntilDone) {
    DownloadEngineV2 engine(fast_poll_config());
    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);

    auto routine = std::make_unique<CountdownShutdownRoutine>(20);
    CountdownShutdownRoutine* routine_ptr = routine.get();
    engine.add_routine_command(std::move(routine));

    auto cmd_counter = std::make_shared<std::atomic<int>>(0);
    engine.add_command(std::make_unique<RequeueCommand>(2, cmd_counter));

    engine.run();

    // 未注册事件的等待命令会被重新入队，直到自身完成
    EXPECT_GE(routine_ptr->executions(), 3);
    EXPECT_EQ(cmd_counter->load(), 3);
}

//==============================================================================
// 运行期添加任务测试
//==============================================================================

TEST(DownloadEngineV2RunTest, AddDownloadWhileRunningActivatesTask) {
    DownloadEngineV2 engine(fast_poll_config());

    // 保活任务：避免引擎因"无任务"在例程命令执行前退出
    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);

    DownloadOptions options = keepalive_options();
    options.output_filename = "run_time_added.bin";
    auto routine = std::make_unique<AddDownloadRoutine>(
        "http://127.0.0.1:1/added.bin", options);
    AddDownloadRoutine* routine_ptr = routine.get();
    engine.add_routine_command(std::move(routine));

    engine.run();

    EXPECT_GE(routine_ptr->executions(), 2);
    EXPECT_GT(routine_ptr->added_id(), 0);
    // 任务应已被激活（从等待队列进入活动状态）
    auto* group = engine.request_group_man()->find_group(routine_ptr->added_id());
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->status(), RequestGroupStatus::ACTIVE);
}

//==============================================================================
// 统计信息分支测试
//==============================================================================

TEST(DownloadEngineV2RunTest, StatisticsCountCompletedAndFailedGroups) {
    DownloadEngineV2 engine(fast_poll_config());

    TaskId id1 = engine.add_download("http://127.0.0.1:1/a.bin", DownloadOptions());
    TaskId id2 = engine.add_download("http://127.0.0.1:1/b.bin", DownloadOptions());
    TaskId id3 = engine.add_download("http://127.0.0.1:1/c.bin", DownloadOptions());
    ASSERT_GT(id1, 0);
    ASSERT_GT(id2, 0);
    ASSERT_GT(id3, 0);

    auto* group1 = engine.request_group_man()->find_group(id1);
    auto* group2 = engine.request_group_man()->find_group(id2);
    auto* group3 = engine.request_group_man()->find_group(id3);
    ASSERT_NE(group1, nullptr);
    ASSERT_NE(group2, nullptr);
    ASSERT_NE(group3, nullptr);

    group1->set_status(RequestGroupStatus::COMPLETED);
    group2->set_status(RequestGroupStatus::FAILED);
    group3->set_status(RequestGroupStatus::REMOVED);

    auto stats = engine.get_statistics();
    EXPECT_EQ(stats.completed_tasks, 1U);
    EXPECT_EQ(stats.stopped_tasks, 2U);  // FAILED + REMOVED
}

TEST(DownloadEngineV2RunTest, StatisticsAggregatesGroupProgress) {
    DownloadEngineV2 engine(fast_poll_config());

    TaskId id = engine.add_download("http://127.0.0.1:1/progress.bin",
                                    DownloadOptions());
    auto* group = engine.request_group_man()->find_group(id);
    ASSERT_NE(group, nullptr);

    group->add_downloaded_bytes(1024);

    auto stats = engine.get_statistics();
    EXPECT_EQ(stats.total_downloaded, 1024U);
}

//==============================================================================
// 异常边界测试：命令抛出不得终止引擎（run 在独立线程语境下即 std::terminate）
//==============================================================================

TEST(DownloadEngineV2RunTest, CommandExceptionFailsGroupAndEngineSurvives) {
    // 单槽位：keepalive 占满活动列表，victim 保持 WAITING 不被激活，
    // 因此它没有真实初始命令，FAILED 状态只能来自异常兜底路径
    DownloadEngineV2 engine(single_slot_config());

    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);
    TaskId victim_id = engine.add_download("http://127.0.0.1:1/victim.bin",
                                           DownloadOptions());
    ASSERT_GT(victim_id, 0);

    auto routine = std::make_unique<CountdownShutdownRoutine>(3);
    engine.add_routine_command(std::move(routine));

    auto throw_counter = std::make_shared<std::atomic<int>>(0);
    engine.add_command(
        std::make_unique<ThrowingCommand>(victim_id, throw_counter, false));

    auto instant_counter = std::make_shared<std::atomic<int>>(0);
    engine.add_command(std::make_unique<InstantCommand>(instant_counter));

    engine.run();  // 若无异常边界，此处 std::terminate

    // 异常命令只坑了自己的组：标 FAILED 且错误消息可查
    EXPECT_EQ(throw_counter->load(), 1);
    auto* victim = engine.request_group_man()->find_group(victim_id);
    ASSERT_NE(victim, nullptr);
    EXPECT_EQ(victim->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(victim->error_message().find("command exception"),
              std::string::npos);

    // 同轮其他命令照常执行，引擎正常退出
    EXPECT_EQ(instant_counter->load(), 1);
    EXPECT_TRUE(engine.is_shutdown_requested());
}

TEST(DownloadEngineV2RunTest, NonStdExceptionAlsoFailsGroup) {
    DownloadEngineV2 engine(single_slot_config());

    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);
    TaskId victim_id = engine.add_download("http://127.0.0.1:1/victim.bin",
                                           DownloadOptions());
    ASSERT_GT(victim_id, 0);

    auto routine = std::make_unique<CountdownShutdownRoutine>(3);
    engine.add_routine_command(std::move(routine));

    auto counter = std::make_shared<std::atomic<int>>(0);
    engine.add_command(
        std::make_unique<ThrowingCommand>(victim_id, counter, true));

    engine.run();

    EXPECT_EQ(counter->load(), 1);
    auto* victim = engine.request_group_man()->find_group(victim_id);
    ASSERT_NE(victim, nullptr);
    EXPECT_EQ(victim->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(victim->error_message().find("unknown"), std::string::npos);
}

TEST(DownloadEngineV2RunTest, RoutineExceptionSkipsRoundEngineKeepsRunning) {
    DownloadEngineV2 engine(fast_poll_config());
    ASSERT_GT(engine.add_download("http://127.0.0.1:1/keepalive.bin",
                                  keepalive_options()),
              0);

    auto throw_counter = std::make_shared<std::atomic<int>>(0);
    engine.add_routine_command(std::make_unique<ThrowingRoutine>(throw_counter));

    auto routine = std::make_unique<CountdownShutdownRoutine>(3);
    CountdownShutdownRoutine* routine_ptr = routine.get();
    engine.add_routine_command(std::move(routine));

    engine.run();

    // 抛异常的例程每轮被跳过但引擎不终止，正常例程照常驱动 shutdown
    EXPECT_GE(throw_counter->load(), 3);
    EXPECT_GE(routine_ptr->executions(), 3);
    EXPECT_TRUE(engine.is_shutdown_requested());
}

//==============================================================================
// 超时清理测试：对端黑洞（连接挂死）必须让任务获得 FAILED 终态
//==============================================================================
//
// 此前的缺陷：等待响应的命令挂起超时后，cleanup_completed_commands
// 只销毁命令对象——不关闭 fd（命令析构 = default 不关 fd）、不把任务
// 组标 FAILED。对端黑洞时任务永久悬空在 Downloading、all_completed
// 永不成立、run() 永不退出。

namespace {

/// 黑洞 HTTP 服务器：接受连接后不读不写不响应（对端请求永远挂死）。
/// 每个连接由非阻塞轮询线程持有；对端关闭 fd 时 recv 返回 0（EOF），
/// 计入 peer_closed——若引擎超时清理只销毁命令而泄漏 fd，进程存活
/// 期间服务器侧观察不到 EOF，此计数不增长
class SilentServer {
public:
    ~SilentServer() { stop(); }  // RAII：joinable 线程析构即 terminate

    bool start() {
#ifdef _WIN32
        ensure_winsock_for_run_test();
#endif
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
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();  // 持有线程 200ms 轮询自行退出
        }
        conn_threads_.clear();
    }

    int port() const { return port_; }
    int peer_closed() const { return peer_closed_.load(); }

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
                continue;  // 超时或错误：重新检查 running_
            }
            sockaddr_in peer{};
            sock_len peer_len = sizeof(peer);
            int conn = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &peer_len);
            if (conn < 0) {
                if (!running_) return;
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(conn_mutex_);
                conns_.push_back(conn);
            }
            conn_threads_.emplace_back([this, conn] { hold(conn); });
        }
    }

    void hold(int conn) {
        set_nonblocking(conn);
        char buf[512];
        while (running_.load()) {
            struct pollfd pfd;
            pfd.fd = conn;
            pfd.events = POLLIN;
            pfd.revents = 0;
            if (POLL(&pfd, 1, 200) <= 0) {
                continue;  // 超时轮询 running_
            }
            recv_ssize n = ::recv(conn, buf, sizeof(buf), 0);
            if (n == 0) {
                peer_closed_.fetch_add(1);  // 对端关闭 fd（EOF）
                break;
            }
            if (n < 0) {
#ifdef _WIN32
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
#endif
                break;  // 真实错误（含服务器 stop 后的残连接）
            }
            // 收到客户端请求头：黑洞，不响应，继续持有连接
        }
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conns_.erase(std::remove(conns_.begin(), conns_.end(), conn),
                         conns_.end());
        }
        CLOSE_SOCKET(conn);
    }

    static void set_nonblocking(int fd) {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> peer_closed_{0};
    std::mutex conn_mutex_;
    std::vector<int> conns_;
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

/// 在独立线程跑 run()，等待任务组进入终态；超时强制停机。
/// 返回是否在时限内观察到终态，elapsed_ms 带出耗时
bool wait_group_terminal(DownloadEngineV2& engine, RequestGroup* group,
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

#ifdef _WIN32
int run_test_getpid() { return _getpid(); }
#else
int run_test_getpid() { return static_cast<int>(::getpid()); }
#endif

std::string run_test_temp_dir(const char* tag) {
    return (std::filesystem::temp_directory_path() /
            (std::string("falcon_v2_run_") + tag + "_" +
             std::to_string(run_test_getpid())))
        .string();
}

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

/// 最小可用 HTTP 服务器：对所有请求返回固定 200 响应体。
/// overwrite 门禁的对照组用例需要真实完成一次下载（黑洞与保留端口
/// 服务器都产不出 COMPLETED 终态）
class MinimalHttpServer {
public:
    ~MinimalHttpServer() { stop(); }  // RAII：joinable 线程析构即 terminate

    bool start(std::string body) {
#ifdef _WIN32
        ensure_winsock_for_run_test();
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
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : conn_threads_) {
            if (t.joinable()) t.join();
        }
        conn_threads_.clear();
    }

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
            conn_threads_.emplace_back([this, conn] { serve(conn); });
        }
    }

    void serve(int conn) {
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

    std::string body_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> conn_threads_;
};

} // namespace

TEST(DownloadEngineV2RunTest, BlackHoleServerTimesOutViaTaskTimeout) {
    SilentServer server;
    ASSERT_TRUE(server.start());

    // 任务级超时优先生效：2s 黑洞挂死后任务必须获得 FAILED 终态，
    // run() 随之退出（修复前：任务永久 Downloading、run() 永不退出）
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;  // 引擎兜底超时保持默认 120s，不参与
    DownloadEngineV2 engine(config);

    const auto options = [] {
        DownloadOptions o;
        o.timeout_seconds = 2;
        return o;
    }();

    const TaskId task_id = engine.add_download(server.url("/blackhole.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(wait_group_terminal(engine, group, 15, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    // 显式小于引擎兜底值：证明生效的是任务级 2s 而非全局兜底；
    // 不设下界——超时只会更晚、不会更早的语义由阈值保证
    EXPECT_LT(elapsed_ms, 10000)
        << "任务级 timeout_seconds=2 未生效（等待 " << elapsed_ms << "ms）";
    // fd 必须真实关闭：进程存活期间服务器侧应观察到 EOF
    EXPECT_GE(server.peer_closed(), 1)
        << "超时清理未关闭 fd（泄漏的连接在服务器侧观察不到 EOF）";

    server.stop();
}

TEST(DownloadEngineV2RunTest, BlackHoleServerTimesOutViaEngineFallback) {
    SilentServer server;
    ASSERT_TRUE(server.start());

    // 任务未设置超时（0）时回落引擎兜底值：command_wait_timeout_seconds=2
    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    config.command_wait_timeout_seconds = 2;
    DownloadEngineV2 engine(config);

    const auto options = [] {
        DownloadOptions o;
        o.timeout_seconds = 0;  // 未设置：回落引擎兜底
        return o;
    }();

    const TaskId task_id = engine.add_download(server.url("/fallback.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(wait_group_terminal(engine, group, 15, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    // 显式小于 DownloadOptions 默认值 30s：证明生效的是引擎兜底 2s
    EXPECT_LT(elapsed_ms, 10000)
        << "引擎兜底 command_wait_timeout_seconds=2 未生效（等待 "
        << elapsed_ms << "ms）";
    EXPECT_GE(server.peer_closed(), 1)
        << "超时清理未关闭 fd（泄漏的连接在服务器侧观察不到 EOF）";

    server.stop();
}

//==============================================================================
// overwrite_existing 语义测试：默认配置绝不允许静默销毁已存在文件
//==============================================================================

/// overwrite_existing=false（默认）+ 输出文件已存在：任务组激活即 FAILED、
/// 错误可查、已存在文件内容原样保留（修复前首段命令无条件 trunc，默认
/// 配置也静默销毁用户文件）。门禁在 init() 网络 I/O 之前生效：URL 不可达
/// （127.0.0.1:1）但错误必须是"文件已存在"而非连接失败
TEST(DownloadEngineV2RunTest, OverwriteDisabledKeepsExistingFileAndFailsGroup) {
    const std::string dir = run_test_temp_dir("protected");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/protected.bin";
    const std::string existing = "OLD-CONTENT-MUST-SURVIVE";
    {
        std::ofstream out(out_path, std::ios::binary);
        out << existing;
    }

    DownloadEngineV2 engine(fast_poll_config());
    DownloadOptions options;
    options.output_filename = out_path;  // 显式指向已存在文件
    options.max_connections = 1;

    const TaskId task_id =
        engine.add_download("http://127.0.0.1:1/protected.bin", options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(wait_group_terminal(engine, group, 10, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("已存在"), std::string::npos)
        << "错误应为文件已存在而非连接失败: " << group->error_message();

    auto task = group->download_task();
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Failed);
    EXPECT_NE(task->error_message().find("已存在"), std::string::npos);

    // 数据保护核心断言：已存在文件一个字节都没被碰
    EXPECT_EQ(read_file_content(out_path), existing);

    std::filesystem::remove_all(dir);
}

/// overwrite_existing=true + 输出文件已存在：显式授权覆盖，下载照常进行，
/// 完成后文件内容被完整替换（修复前该路径本就 trunc 重写，行为保持）
TEST(DownloadEngineV2RunTest, OverwriteEnabledReplacesExistingFile) {
    const std::string body = make_body(32 * 1024);
    MinimalHttpServer server;
    ASSERT_TRUE(server.start(body));

    const std::string dir = run_test_temp_dir("replace");
    std::filesystem::create_directories(dir);
    const std::string out_path = dir + "/replace.bin";
    {
        std::ofstream out(out_path, std::ios::binary);
        out << "STALE-CONTENT";
    }

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.overwrite_existing = true;

    const TaskId task_id = engine.add_download(server.url("/replace.bin"), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    long long elapsed_ms = 0;
    ASSERT_TRUE(wait_group_terminal(engine, group, 20, elapsed_ms));

    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    // 旧内容被完整替换：既无残留（截断失效）也无追加（写入位置错乱）
    EXPECT_EQ(read_file_content(out_path), body);

    std::filesystem::remove_all(dir);
}

//==============================================================================
// 停机排水测试：run() 退出必须关闭命令持有的 fd
//==============================================================================

/// 带挂起命令（对端黑洞）的引擎 shutdown 后，命令持有的 fd 必须被
/// 关闭——服务器侧观察到 EOF。任务/引擎兜底超时（30s/120s）在测试
/// 时长内不会触发，超时清理未参与，EOF 只能来自 run() 退出的停机
/// 排水（修复前 fd 随命令析构静默泄漏，进程存活期间观察不到 EOF）
TEST(DownloadEngineV2RunTest, ShutdownDrainsParkedCommandFd) {
    SilentServer server;
    ASSERT_TRUE(server.start());

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;  // 兜底超时保持默认 120s，不参与
    DownloadEngineV2 engine(config);

    const TaskId task_id = engine.add_download(server.url("/drain.bin"),
                                               DownloadOptions());
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    std::thread runner([&engine] { engine.run(); });

    // 等命令挂起（请求已发出、响应黑洞挂死）；即便尚未挂起，命令也
    // 在命令队列中，同样被排水覆盖
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    engine.shutdown();
    runner.join();

    // EOF 观察有服务器线程的 poll 周期延迟，短轮询等待避免误报
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (server.peer_closed() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_GE(server.peer_closed(), 1)
        << "停机后挂起命令的 fd 未关闭（排水缺失，fd 静默泄漏）";
    EXPECT_EQ(group->status(), RequestGroupStatus::ACTIVE)
        << "停机排水不改变任务状态（非失败语义）";

    server.stop();
}
