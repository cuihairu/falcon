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
#else
#include <unistd.h>
#include <sys/socket.h>
#include <fcntl.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/command.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

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
                                  DownloadOptions()),
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
                                  DownloadOptions()),
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
                                  DownloadOptions()),
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
                                  DownloadOptions()),
              0);

    DownloadOptions options;
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
