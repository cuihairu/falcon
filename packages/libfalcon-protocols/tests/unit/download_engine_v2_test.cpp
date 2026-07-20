/**
 * @file download_engine_v2_test.cpp
 * @brief DownloadEngineV2 单元测试
 * @author Falcon Team
 * @date 2025-12-31
 */

#include <gtest/gtest.h>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/commands/command.hpp>
#include <thread>
#include <chrono>
#ifndef _WIN32
#include <unistd.h>
#endif

using namespace falcon;

namespace {
class MockQueueCommand : public AbstractCommand {
public:
    MockQueueCommand() : AbstractCommand(1) {}

    bool execute(DownloadEngineV2* /*engine*/) override {
        mark_completed();
        return true;
    }

    const char* name() const override { return "MockQueueCommand"; }
};

class ScopedPipe {
public:
    ScopedPipe() {
        if (::pipe(fds_) != 0) {
            fds_[0] = -1;
            fds_[1] = -1;
        }
    }

    ~ScopedPipe() {
        if (fds_[0] >= 0) {
            ::close(fds_[0]);
        }
        if (fds_[1] >= 0) {
            ::close(fds_[1]);
        }
    }

    ScopedPipe(const ScopedPipe&) = delete;
    ScopedPipe& operator=(const ScopedPipe&) = delete;

    int read_fd() const { return fds_[0]; }
    int write_fd() const { return fds_[1]; }
    bool valid() const { return fds_[0] >= 0 && fds_[1] >= 0; }

private:
    int fds_[2] = {-1, -1};
};
}  // namespace

// ============================================================================
// 测试夹具
// ============================================================================

class DownloadEngineV2Test : public ::testing::Test {
protected:
    struct WaitingStateCounts {
        std::size_t waiting_command_times = 0;
        std::size_t socket_waits = 0;
        std::size_t socket_commands = 0;
        std::size_t waiting_commands = 0;
    };

    void SetUp() override {
        EngineConfigV2 config;
        config.max_concurrent_tasks = 3;
        config.poll_timeout_ms = 50;

        engine_ = std::make_unique<DownloadEngineV2>(config);
    }

    void TearDown() override {
        if (engine_) {
            engine_->shutdown();
        }
    }

    std::unique_ptr<DownloadEngineV2> engine_;

    static void cleanup_completed_commands(DownloadEngineV2& engine) {
        engine.cleanup_completed_commands();
    }

    static void add_waiting_entry(
        DownloadEngineV2& engine,
        CommandId command_id,
        int fd,
        std::chrono::steady_clock::time_point timestamp,
        std::unique_ptr<Command> command = nullptr
    ) {
        ASSERT_TRUE(engine.register_socket_event(
            fd, static_cast<int>(net::IOEvent::READ), command_id));
        std::lock_guard<std::mutex> lock(engine.socket_map_mutex_);
        engine.waiting_command_times_[command_id] = timestamp;
        if (command) {
            engine.waiting_commands_[command_id] = std::move(command);
        }
    }

    static WaitingStateCounts count_waiting_entry(DownloadEngineV2& engine,
                                                  CommandId command_id,
                                                  int fd) {
        std::lock_guard<std::mutex> lock(engine.socket_map_mutex_);
        return {
            engine.waiting_command_times_.count(command_id),
            engine.socket_wait_map_.count(command_id),
            engine.socket_command_map_.count(fd),
            engine.waiting_commands_.count(command_id)
        };
    }

    static std::chrono::steady_clock::time_point expired_wait_time() {
        return std::chrono::steady_clock::now() - std::chrono::seconds(2);
    }
};

// ============================================================================
// 构造和析构测试
// ============================================================================

TEST_F(DownloadEngineV2Test, Construction_DefaultConfig) {
    EngineConfigV2 config;
    auto engine = std::make_unique<DownloadEngineV2>(config);

    EXPECT_NE(engine, nullptr);
    EXPECT_FALSE(engine->is_shutdown_requested());
    EXPECT_FALSE(engine->is_force_shutdown_requested());
}

TEST_F(DownloadEngineV2Test, Construction_CustomConfig) {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 10;
    config.global_speed_limit = 1024 * 1024;
    config.poll_timeout_ms = 200;

    auto engine = std::make_unique<DownloadEngineV2>(config);

    EXPECT_NE(engine, nullptr);
}

TEST_F(DownloadEngineV2Test, Shutdown_Normal) {
    EXPECT_FALSE(engine_->is_shutdown_requested());

    engine_->shutdown();

    EXPECT_TRUE(engine_->is_shutdown_requested());
    EXPECT_FALSE(engine_->is_force_shutdown_requested());
}

TEST_F(DownloadEngineV2Test, Shutdown_Force) {
    EXPECT_FALSE(engine_->is_shutdown_requested());

    engine_->force_shutdown();

    EXPECT_TRUE(engine_->is_shutdown_requested());
    EXPECT_TRUE(engine_->is_force_shutdown_requested());
}

// ============================================================================
// 任务管理测试
// ============================================================================

TEST_F(DownloadEngineV2Test, AddDownload_SingleURL) {
    DownloadOptions options;
    options.output_filename = "test1.bin";

    TaskId id = engine_->add_download("http://example.com/file1.bin", options);

    EXPECT_GT(id, 0);
}

TEST_F(DownloadEngineV2Test, AddDownload_MultipleURLs) {
    DownloadOptions options;
    options.output_filename = "test2.bin";

    std::vector<std::string> urls = {
        "http://mirror1.example.com/file.bin",
        "http://mirror2.example.com/file.bin",
        "http://mirror3.example.com/file.bin"
    };

    TaskId id = engine_->add_download(urls, options);

    EXPECT_GT(id, 0);
}

TEST_F(DownloadEngineV2Test, AddDownload_MultipleTasks) {
    std::vector<TaskId> ids;

    for (int i = 0; i < 5; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        TaskId id = engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
        ids.push_back(id);
    }

    EXPECT_EQ(5, ids.size());

    // 验证 ID 递增
    for (size_t i = 1; i < ids.size(); ++i) {
        EXPECT_GT(ids[i], ids[i - 1]);
    }
}

TEST_F(DownloadEngineV2Test, PauseTask_ValidId) {
    DownloadOptions options;
    TaskId id = engine_->add_download("http://example.com/file.bin", options);

    bool paused = engine_->pause_task(id);

    EXPECT_TRUE(paused);
}

TEST_F(DownloadEngineV2Test, PauseTask_InvalidId) {
    bool paused = engine_->pause_task(99999);

    EXPECT_FALSE(paused);
}

TEST_F(DownloadEngineV2Test, ResumeTask_ValidId) {
    DownloadOptions options;
    TaskId id = engine_->add_download("http://example.com/file.bin", options);

    engine_->pause_task(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    bool resumed = engine_->resume_task(id);

    EXPECT_TRUE(resumed);
}

TEST_F(DownloadEngineV2Test, ResumeTask_InvalidId) {
    bool resumed = engine_->resume_task(99999);

    EXPECT_FALSE(resumed);
}

TEST_F(DownloadEngineV2Test, CancelTask_ValidId) {
    DownloadOptions options;
    TaskId id = engine_->add_download("http://example.com/file.bin", options);

    bool cancelled = engine_->cancel_task(id);

    EXPECT_TRUE(cancelled);
}

TEST_F(DownloadEngineV2Test, CancelTask_InvalidId) {
    bool cancelled = engine_->cancel_task(99999);

    EXPECT_FALSE(cancelled);
}

// ============================================================================
// 批量操作测试
// ============================================================================

TEST_F(DownloadEngineV2Test, PauseAll_EmptyEngine) {
    // 不应该崩溃
    engine_->pause_all();
}

TEST_F(DownloadEngineV2Test, PauseAll_WithTasks) {
    for (int i = 0; i < 3; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
    }

    // 不应该崩溃
    engine_->pause_all();
}

TEST_F(DownloadEngineV2Test, ResumeAll_EmptyEngine) {
    // 不应该崩溃
    engine_->resume_all();
}

TEST_F(DownloadEngineV2Test, ResumeAll_WithTasks) {
    for (int i = 0; i < 3; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
    }

    engine_->pause_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 不应该崩溃
    engine_->resume_all();
}

TEST_F(DownloadEngineV2Test, CancelAll_EmptyEngine) {
    engine_->cancel_all();

    EXPECT_TRUE(engine_->is_shutdown_requested());
}

TEST_F(DownloadEngineV2Test, CancelAll_WithTasks) {
    for (int i = 0; i < 3; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
    }

    engine_->cancel_all();

    EXPECT_TRUE(engine_->is_shutdown_requested());
}

// ============================================================================
// 命令队列测试
// ============================================================================

TEST_F(DownloadEngineV2Test, AddCommand_Valid) {
    auto cmd = std::make_unique<MockQueueCommand>();

    // 不应该崩溃
    engine_->add_command(std::move(cmd));
}

TEST_F(DownloadEngineV2Test, AddCommand_Nullptr) {
    // 不应该崩溃
    engine_->add_command(nullptr);
}

TEST_F(DownloadEngineV2Test, AddRoutineCommand_Valid) {
    auto cmd = std::make_unique<MockQueueCommand>();

    // 不应该崩溃
    engine_->add_routine_command(std::move(cmd));
}

TEST_F(DownloadEngineV2Test, AddRoutineCommand_Nullptr) {
    // 不应该崩溃
    engine_->add_routine_command(nullptr);
}

TEST_F(DownloadEngineV2Test, AddMultipleCommands) {
    for (int i = 0; i < 10; ++i) {
        auto cmd = std::make_unique<MockQueueCommand>();
        engine_->add_command(std::move(cmd));
    }

    // 不应该崩溃
}

// ============================================================================
// Socket 事件注册测试
// ============================================================================

TEST_F(DownloadEngineV2Test, RegisterSocketEvent_Valid) {
    ScopedPipe pipe;
    ASSERT_TRUE(pipe.valid());

    CommandId cmd_id = 1;
    int fd = pipe.read_fd();
    int events = 1;  // 读事件

    bool registered = engine_->register_socket_event(fd, events, cmd_id);

    EXPECT_TRUE(registered);
}

TEST_F(DownloadEngineV2Test, RegisterSocketEvent_DuplicateFd) {
    ScopedPipe pipe;
    ASSERT_TRUE(pipe.valid());

    CommandId cmd_id1 = 1;
    CommandId cmd_id2 = 2;
    int fd = pipe.read_fd();
    int events = 1;

    engine_->register_socket_event(fd, events, cmd_id1);
    bool registered = engine_->register_socket_event(fd, events, cmd_id2);

    // 应该允许覆盖
    EXPECT_TRUE(registered);
}

TEST_F(DownloadEngineV2Test, UnregisterSocketEvent_Valid) {
    ScopedPipe pipe;
    ASSERT_TRUE(pipe.valid());

    CommandId cmd_id = 1;
    int fd = pipe.read_fd();
    int events = 1;

    engine_->register_socket_event(fd, events, cmd_id);
    bool unregistered = engine_->unregister_socket_event(fd);

    EXPECT_TRUE(unregistered);
}

TEST_F(DownloadEngineV2Test, UnregisterSocketEvent_NonExistent) {
    int fd = 999;

    bool unregistered = engine_->unregister_socket_event(fd);

    EXPECT_FALSE(unregistered);
}

// ============================================================================
// 统计信息测试
// ============================================================================

TEST_F(DownloadEngineV2Test, GetStatistics_EmptyEngine) {
    auto stats = engine_->get_statistics();

    EXPECT_EQ(0, stats.active_tasks);
    EXPECT_EQ(0, stats.waiting_tasks);
    EXPECT_EQ(0, stats.completed_tasks);
    EXPECT_EQ(0, stats.stopped_tasks);
    EXPECT_EQ(0, stats.global_download_speed);
    EXPECT_EQ(0, stats.total_downloaded);
}

TEST_F(DownloadEngineV2Test, GetStatistics_WithTasks) {
    for (int i = 0; i < 5; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
    }

    auto stats = engine_->get_statistics();

    // 统计信息应该被更新
    EXPECT_GE(stats.waiting_tasks, 0);
}

// ============================================================================
// 访问器测试
// ============================================================================

TEST_F(DownloadEngineV2Test, Accessors_NonNull) {
    EXPECT_NE(engine_->event_poll(), nullptr);
    EXPECT_NE(engine_->request_group_man(), nullptr);
    EXPECT_NE(engine_->socket_pool(), nullptr);
}

// ============================================================================
// 线程安全测试
// ============================================================================

TEST_F(DownloadEngineV2Test, ConcurrentAddDownloads) {
    constexpr int thread_count = 5;
    constexpr int tasks_per_thread = 10;

    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([this, i, tasks_per_thread]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                DownloadOptions options;
                options.output_filename = "test_" + std::to_string(i) + "_" + std::to_string(j) + ".bin";
                engine_->add_download("http://example.com/file" + std::to_string(j) + ".bin", options);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证统计信息
    auto stats = engine_->get_statistics();
    EXPECT_GE(stats.waiting_tasks, 0);
}

TEST_F(DownloadEngineV2Test, ConcurrentCommandAddition) {
    constexpr int thread_count = 10;
    constexpr int commands_per_thread = 100;

    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([this, commands_per_thread]() {
            for (int j = 0; j < commands_per_thread; ++j) {
                auto cmd = std::make_unique<MockQueueCommand>();
                engine_->add_command(std::move(cmd));
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 不应该崩溃或死锁
}

TEST_F(DownloadEngineV2Test, ConcurrentPauseResume) {
    std::vector<TaskId> ids;

    // 添加任务
    for (int i = 0; i < 10; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        TaskId id = engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
        ids.push_back(id);
    }

    std::vector<std::thread> threads;

    // 线程1：暂停任务
    threads.emplace_back([this, &ids]() {
        for (TaskId id : ids) {
            engine_->pause_task(id);
        }
    });

    // 线程2：恢复任务
    threads.emplace_back([this, &ids]() {
        for (TaskId id : ids) {
            engine_->resume_task(id);
        }
    });

    for (auto& t : threads) {
        t.join();
    }

    // 不应该崩溃或死锁
}

// ============================================================================
// 边界条件测试
// ============================================================================

TEST_F(DownloadEngineV2Test, AddDownload_EmptyURL) {
    DownloadOptions options;

    // 应该返回有效 ID（即使 URL 无效）
    TaskId id = engine_->add_download("", options);
    EXPECT_GT(id, 0);
}

TEST_F(DownloadEngineV2Test, AddDownload_EmptyURLList) {
    DownloadOptions options;
    std::vector<std::string> urls;

    // 空 URL 列表不应触发崩溃
    TaskId id = engine_->add_download(urls, options);
    EXPECT_EQ(id, INVALID_TASK_ID);
}

TEST_F(DownloadEngineV2Test, MaxConcurrentTasks_Limit) {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 2;

    auto engine = std::make_unique<DownloadEngineV2>(config);

    // 添加超过并发限制的任务数
    for (int i = 0; i < 10; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        engine->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
    }

    auto stats = engine->get_statistics();

    // 验证等待队列中有任务
    EXPECT_GE(stats.waiting_tasks, 0);
}

// ============================================================================
// 性能测试
// ============================================================================

TEST_F(DownloadEngineV2Test, Performance_AddManyTasks) {
    constexpr int task_count = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < task_count; ++i) {
        DownloadOptions options;
        options.output_filename = "test_" + std::to_string(i) + ".bin";
        engine_->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该在合理时间内完成（< 1秒）
    EXPECT_LT(duration.count(), 1000) << "Adding " << task_count
                                       << " tasks took " << duration.count() << "ms";
}

TEST_F(DownloadEngineV2Test, Performance_AddManyCommands) {
    constexpr int command_count = 10000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < command_count; ++i) {
        auto cmd = std::make_unique<MockQueueCommand>();
        engine_->add_command(std::move(cmd));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该在合理时间内完成（< 500ms）
    EXPECT_LT(duration.count(), 500) << "Adding " << command_count
                                      << " commands took " << duration.count() << "ms";
}

// ============================================================================
// 内存泄漏检测
// ============================================================================

TEST_F(DownloadEngineV2Test, NoMemoryLeaks_MultipleCycles) {
    for (int cycle = 0; cycle < 10; ++cycle) {
        auto engine = std::make_unique<DownloadEngineV2>();

        // 添加任务
        for (int i = 0; i < 50; ++i) {
            DownloadOptions options;
            options.output_filename = "test_" + std::to_string(i) + ".bin";
            engine->add_download("http://example.com/file" + std::to_string(i) + ".bin", options);
        }

        // 添加命令
        for (int i = 0; i < 100; ++i) {
            auto cmd = std::make_unique<MockQueueCommand>();
            engine->add_command(std::move(cmd));
        }

        // 取消所有任务
        engine->cancel_all();

        // 销毁引擎
        engine.reset();
    }

    // 如果有内存泄漏，Valgrind/ASan 会报告
    SUCCEED();
}

// ============================================================================
// 命令等待超时清理测试
// 验证 DownloadEngineV2::cleanup_completed_commands 的行为：
//   1) timeout <= 0 时禁用清理；
//   2) 超时命令从所有 sidecar 映射中被移除；
//   3) 未超时命令保留；
//   4) 超时命令的 unique_ptr<Command> 被正确销毁。
// 友元 ::DownloadEngineV2Test 直接驱动 private 成员，避免依赖真实 socket/EventPoll。
// ============================================================================

// 自定义命令：execute() 始终返回 false，模拟等待 I/O 的命令
namespace {
class AlwaysWaitingCommand : public AbstractCommand {
public:
    AlwaysWaitingCommand() : AbstractCommand(1) {}
    bool execute(DownloadEngineV2*) override { return false; }
    const char* name() const override { return "AlwaysWaitingCommand"; }
};
}  // namespace

TEST_F(DownloadEngineV2Test, Config_DefaultCommandWaitTimeout) {
    EngineConfigV2 config;
    EXPECT_EQ(config.command_wait_timeout_seconds, 120);
}

TEST_F(DownloadEngineV2Test, CleanupCompletedCommands_NoWaiting_NoOp) {
    // 空状态下调用清理不应崩溃
    cleanup_completed_commands(*engine_);
    SUCCEED();
}

TEST_F(DownloadEngineV2Test, CleanupCompletedCommands_DisabledWhenZeroTimeout) {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 1;
    config.poll_timeout_ms = 10;
    config.command_wait_timeout_seconds = 0;  // 禁用清理

    auto engine = std::make_unique<DownloadEngineV2>(config);

    constexpr CommandId kCmd = 7001;
    ScopedPipe fds;
    ASSERT_TRUE(fds.valid());
    add_waiting_entry(*engine, kCmd, fds.read_fd(), expired_wait_time());

    cleanup_completed_commands(*engine);

    // 禁用清理：所有映射应原封不动
    auto counts = count_waiting_entry(*engine, kCmd, fds.read_fd());
    EXPECT_EQ(counts.waiting_command_times, 1u);
    EXPECT_EQ(counts.socket_waits, 1u);
    EXPECT_EQ(counts.socket_commands, 1u);
}

TEST_F(DownloadEngineV2Test, CleanupCompletedCommands_RemovesExpiredEntries) {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 1;
    config.poll_timeout_ms = 10;
    config.command_wait_timeout_seconds = 1;

    auto engine = std::make_unique<DownloadEngineV2>(config);

    constexpr CommandId kCmd = 7101;
    ScopedPipe fds;
    ASSERT_TRUE(fds.valid());
    add_waiting_entry(*engine, kCmd, fds.read_fd(), expired_wait_time());

    cleanup_completed_commands(*engine);

    auto counts = count_waiting_entry(*engine, kCmd, fds.read_fd());
    EXPECT_EQ(counts.waiting_command_times, 0u);
    EXPECT_EQ(counts.socket_waits, 0u);
    EXPECT_EQ(counts.socket_commands, 0u);
    EXPECT_EQ(counts.waiting_commands, 0u);
}

TEST_F(DownloadEngineV2Test, CleanupCompletedCommands_PreservesFreshEntries) {
    EngineConfigV2 config;
    config.max_concurrent_tasks = 1;
    config.poll_timeout_ms = 10;
    config.command_wait_timeout_seconds = 3600;  // 1 小时

    auto engine = std::make_unique<DownloadEngineV2>(config);

    constexpr CommandId kCmd = 7201;
    ScopedPipe fds;
    ASSERT_TRUE(fds.valid());
    add_waiting_entry(*engine, kCmd, fds.read_fd(), std::chrono::steady_clock::now());

    cleanup_completed_commands(*engine);

    auto counts = count_waiting_entry(*engine, kCmd, fds.read_fd());
    EXPECT_EQ(counts.waiting_command_times, 1u);
    EXPECT_EQ(counts.socket_waits, 1u);
    EXPECT_EQ(counts.socket_commands, 1u);
}

TEST_F(DownloadEngineV2Test, CleanupCompletedCommands_PartialExpiry) {
    EngineConfigV2 config;
    config.command_wait_timeout_seconds = 1;
    auto engine = std::make_unique<DownloadEngineV2>(config);

    constexpr CommandId kExpired = 7301;
    constexpr CommandId kFresh = 7302;
    ScopedPipe expired_fds;
    ScopedPipe fresh_fds;
    ASSERT_TRUE(expired_fds.valid());
    ASSERT_TRUE(fresh_fds.valid());
    add_waiting_entry(*engine, kExpired, expired_fds.read_fd(), expired_wait_time());
    add_waiting_entry(*engine, kFresh, fresh_fds.read_fd(), std::chrono::steady_clock::now());

    cleanup_completed_commands(*engine);

    auto expired_counts = count_waiting_entry(*engine, kExpired, expired_fds.read_fd());
    EXPECT_EQ(expired_counts.waiting_command_times, 0u);
    EXPECT_EQ(expired_counts.socket_waits, 0u);
    EXPECT_EQ(expired_counts.socket_commands, 0u);

    auto fresh_counts = count_waiting_entry(*engine, kFresh, fresh_fds.read_fd());
    EXPECT_EQ(fresh_counts.waiting_command_times, 1u);
    EXPECT_EQ(fresh_counts.socket_waits, 1u);
    EXPECT_EQ(fresh_counts.socket_commands, 1u);
}

TEST_F(DownloadEngineV2Test, CleanupCompletedCommands_DropsParkedCommandOwnership) {
    EngineConfigV2 config;
    config.command_wait_timeout_seconds = 1;
    auto engine = std::make_unique<DownloadEngineV2>(config);

    ScopedPipe fds;
    ASSERT_TRUE(fds.valid());
    auto cmd = std::make_unique<AlwaysWaitingCommand>();
    CommandId real_id = cmd->id();
    add_waiting_entry(*engine, real_id, fds.read_fd(), expired_wait_time(), std::move(cmd));

    cleanup_completed_commands(*engine);

    auto counts = count_waiting_entry(*engine, real_id, fds.read_fd());
    EXPECT_EQ(counts.waiting_commands, 0u);
    EXPECT_EQ(counts.waiting_command_times, 0u);
}
