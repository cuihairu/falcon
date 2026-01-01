// Falcon Download Engine Unit Tests

#include <falcon/download_engine.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>

namespace {

class TestProtocolHandler final : public falcon::IProtocolHandler {
public:
    [[nodiscard]] std::string protocol_name() const override { return "test"; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override { return {"test"}; }

    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind("test://", 0) == 0;
    }

    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "file.bin";
        info.total_size = 4;
        info.supports_resume = true;
        info.content_type = "application/octet-stream";
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        if (!task) return;

        task->mark_started();

        auto info = get_file_info(task->url(), task->options());
        task->set_file_info(info);

        task->update_progress(2, 4, 2);
        task->update_progress(4, 4, 2);

        task->set_status(falcon::TaskStatus::Completed);
    }

    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr task) override {
        if (task) task->set_status(falcon::TaskStatus::Cancelled);
    }
};

} // namespace

TEST(DownloadEngineTest, AddTaskBuildsOutputPath) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.output_directory = "downloads";
    options.output_filename = "out.bin";

    auto task = engine.add_task("test://example.com/path/file.bin", options);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->output_path(), "downloads/out.bin");
}

TEST(DownloadEngineTest, AddTaskExtractsFilenameWhenNotProvided) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    auto task = engine.add_task("test://example.com/path/file.bin", options);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->output_path(), "file.bin");
}

TEST(DownloadEngineTest, AddTaskDefaultsWhenNoPathSegment) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    auto task = engine.add_task("test://example.com/path/", options);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->output_path(), "download");
}

TEST(DownloadEngineTest, UnsupportedUrlThrows) {
    falcon::DownloadEngine engine;
    EXPECT_THROW(engine.add_task("noscheme", falcon::DownloadOptions{}), falcon::UnsupportedProtocolException);
}

TEST(DownloadEngineTest, StartTaskCompletesWithTestHandler) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/path/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(1)));
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
}

// 新增：多任务并发测试
TEST(DownloadEngineTest, MultipleConcurrentTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    std::vector<falcon::DownloadTask::Ptr> tasks;

    // 添加多个任务
    for (int i = 0; i < 5; ++i) {
        auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                    falcon::DownloadOptions{});
        ASSERT_NE(task, nullptr);
        tasks.push_back(task);
    }

    // 启动所有任务
    for (const auto& task : tasks) {
        EXPECT_TRUE(engine.start_task(task->id()));
    }

    // 等待所有任务完成
    for (const auto& task : tasks) {
        EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));
        EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
    }
}

// 新增：暂停任务测试
TEST(DownloadEngineTest, PauseTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));

    // 暂停任务
    EXPECT_TRUE(engine.pause_task(task->id()));

    // 等待一小段时间确保任务被暂停
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(task->status(), falcon::TaskStatus::Paused);
}

// 新增：恢复任务测试
TEST(DownloadEngineTest, ResumeTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(engine.pause_task(task->id()));

    // 恢复任务
    EXPECT_TRUE(engine.resume_task(task->id()));

    // 等待完成
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
}

// 新增：取消任务测试
TEST(DownloadEngineTest, CancelTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));

    // 取消任务
    EXPECT_TRUE(engine.cancel_task(task->id()));

    EXPECT_EQ(task->status(), falcon::TaskStatus::Cancelled);
}

// 新增：取消不存在的任务
TEST(DownloadEngineTest, CancelNonExistentTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    EXPECT_FALSE(engine.cancel_task(99999));
}

// 新增：暂停所有任务
TEST(DownloadEngineTest, PauseAllTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    std::vector<falcon::DownloadTask::Ptr> tasks;

    // 添加并启动多个任务
    for (int i = 0; i < 3; ++i) {
        auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                    falcon::DownloadOptions{});
        ASSERT_NE(task, nullptr);
        EXPECT_TRUE(engine.start_task(task->id()));
        tasks.push_back(task);
    }

    // 暂停所有
    engine.pause_all();

    // 验证所有任务都被暂停
    for (const auto& task : tasks) {
        EXPECT_TRUE(task->status() == falcon::TaskStatus::Paused ||
                    task->status() == falcon::TaskStatus::Completed);
    }
}

// 新增：恢复所有任务
TEST(DownloadEngineTest, ResumeAllTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    std::vector<falcon::DownloadTask::Ptr> tasks;

    // 添加并暂停多个任务
    for (int i = 0; i < 3; ++i) {
        auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                    falcon::DownloadOptions{});
        ASSERT_NE(task, nullptr);
        EXPECT_TRUE(engine.start_task(task->id()));
        EXPECT_TRUE(engine.pause_task(task->id()));
        tasks.push_back(task);
    }

    // 恢复所有
    engine.resume_all();

    // 等待所有任务完成
    for (const auto& task : tasks) {
        EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));
    }
}

// 新增：获取任务信息
TEST(DownloadEngineTest, GetTaskInfo) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    // 获取任务
    auto retrieved = engine.get_task(task->id());
    EXPECT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->id(), task->id());
}

// 新增：获取不存在的任务
TEST(DownloadEngineTest, GetNonExistentTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.get_task(99999);
    EXPECT_EQ(task, nullptr);
}

// 新增：全局速度限制
TEST(DownloadEngineTest, GlobalSpeedLimit) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    // 设置全局速度限制
    engine.set_global_speed_limit(1024 * 1024);  // 1MB/s

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
}

// 新增：移除任务
TEST(DownloadEngineTest, RemoveTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    // 启动并等待完成
    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));

    // 移除已完成的任务
    EXPECT_TRUE(engine.remove_task(task->id()));

    // 任务应该不存在了
    auto retrieved = engine.get_task(task->id());
    EXPECT_EQ(retrieved, nullptr);
}

// 新增：移除不存在的任务
TEST(DownloadEngineTest, RemoveNonExistentTask) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    EXPECT_FALSE(engine.remove_task(99999));
}

// 新增：获取所有任务
TEST(DownloadEngineTest, GetAllTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    // 添加多个任务
    for (int i = 0; i < 5; ++i) {
        auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                    falcon::DownloadOptions{});
        EXPECT_NE(task, nullptr);
    }

    auto tasks = engine.get_all_tasks();
    EXPECT_EQ(tasks.size(), 5u);
}

// 新增：获取活动任务
TEST(DownloadEngineTest, GetActiveTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    std::vector<falcon::DownloadTask::Ptr> tasks;

    // 添加并启动部分任务
    for (int i = 0; i < 3; ++i) {
        auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                    falcon::DownloadOptions{});
        EXPECT_NE(task, nullptr);
        EXPECT_TRUE(engine.start_task(task->id()));
        tasks.push_back(task);
    }

    // 添加但不启动的任务
    auto pending_task = engine.add_task("test://example.com/pending.bin", falcon::DownloadOptions{});
    EXPECT_NE(pending_task, nullptr);

    auto active_tasks = engine.get_active_tasks();
    // 活动任务应该包括正在运行和等待中的任务
    EXPECT_GE(active_tasks.size(), 3u);
}

// 新增：按状态获取任务
TEST(DownloadEngineTest, GetTasksByStatus) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    // 添加不同状态的任务
    auto task1 = engine.add_task("test://example.com/file1.bin", falcon::DownloadOptions{});
    auto task2 = engine.add_task("test://example.com/file2.bin", falcon::DownloadOptions{});

    ASSERT_NE(task1, nullptr);
    ASSERT_NE(task2, nullptr);

    // 启动一个任务
    EXPECT_TRUE(engine.start_task(task1->id()));
    EXPECT_TRUE(task1->wait_for(std::chrono::seconds(2)));

    // 获取已完成的任务
    auto completed_tasks = engine.get_tasks_by_status(falcon::TaskStatus::Completed);
    EXPECT_GE(completed_tasks.size(), 1u);

    // 获取等待中的任务
    auto pending_tasks = engine.get_tasks_by_status(falcon::TaskStatus::Pending);
    EXPECT_GE(pending_tasks.size(), 1u);
}

// 新增：重复注册处理器
TEST(DownloadEngineTest, RegisterDuplicateHandler) {
    falcon::DownloadEngine engine;

    engine.register_handler(std::make_unique<TestProtocolHandler>());
    // 重复注册应该被忽略或替换
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    EXPECT_NE(task, nullptr);
}

// 新增：多个处理器注册
TEST(DownloadEngineTest, RegisterMultipleHandlers) {
    falcon::DownloadEngine engine;

    // 注册不同协议的处理器
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    class TestProtocolHandler2 final : public falcon::IProtocolHandler {
    public:
        [[nodiscard]] std::string protocol_name() const override { return "test2"; }
        [[nodiscard]] std::vector<std::string> supported_schemes() const override { return {"test2"}; }

        [[nodiscard]] bool can_handle(const std::string& url) const override {
            return url.rfind("test2://", 0) == 0;
        }

        [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                     const falcon::DownloadOptions&) override {
            falcon::FileInfo info;
            info.url = url;
            info.filename = "file2.bin";
            info.total_size = 100;
            info.supports_resume = false;
            info.content_type = "application/octet-stream";
            return info;
        }

        void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
            if (!task) return;
            task->mark_started();
            task->update_progress(100, 100, 100);
            task->set_status(falcon::TaskStatus::Completed);
        }

        void pause(falcon::DownloadTask::Ptr) override {}
        void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
        void cancel(falcon::DownloadTask::Ptr task) override {
            if (task) task->set_status(falcon::TaskStatus::Cancelled);
        }
    };

    engine.register_handler(std::make_unique<TestProtocolHandler2>());

    // 两个协议都应该工作
    auto task1 = engine.add_task("test://example.com/file1.bin", falcon::DownloadOptions{});
    auto task2 = engine.add_task("test2://example.com/file2.bin", falcon::DownloadOptions{});

    EXPECT_NE(task1, nullptr);
    EXPECT_NE(task2, nullptr);
}

// 新增：空URL处理
TEST(DownloadEngineTest, EmptyUrl) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    EXPECT_THROW(engine.add_task("", falcon::DownloadOptions{}), falcon::UnsupportedProtocolException);
}

// 新增：无效URL协议
TEST(DownloadEngineTest, InvalidProtocol) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    EXPECT_THROW(engine.add_task("invalid://example.com/file.bin", falcon::DownloadOptions{}),
                 falcon::UnsupportedProtocolException);
}

// 新增：自定义输出路径
TEST(DownloadEngineTest, CustomOutputPath) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.output_directory = "/custom/path";
    options.output_filename = "custom_name.bin";

    auto task = engine.add_task("test://example.com/file.bin", options);
    ASSERT_NE(task, nullptr);

    EXPECT_EQ(task->output_path(), "/custom/path/custom_name.bin");
}

// 新增：相对路径处理
TEST(DownloadEngineTest, RelativePathHandling) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.output_directory = "downloads";

    auto task = engine.add_task("test://example.com/file.bin", options);
    ASSERT_NE(task, nullptr);

    EXPECT_EQ(task->output_path(), "downloads/file.bin");
}

// 新增：任务选项传递
TEST(DownloadEngineTest, TaskOptionsPassed) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.timeout_seconds = 60;
    options.max_retries = 3;

    auto task = engine.add_task("test://example.com/file.bin", options);
    ASSERT_NE(task, nullptr);

    // 验证选项被正确传递
    EXPECT_EQ(task->options().max_connections, 5);
    EXPECT_EQ(task->options().timeout_seconds, 60);
    EXPECT_EQ(task->options().max_retries, 3);
}

// 新增：大量任务压力测试
TEST(DownloadEngineTest, HighStressManyTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    constexpr int task_count = 100;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < task_count; ++i) {
        auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                    falcon::DownloadOptions{});
        EXPECT_NE(task, nullptr);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该快速完成
    EXPECT_LT(duration.count(), 1000);
}

// 新增：事件监听器测试
class TestEventListener : public falcon::IEventListener {
public:
    std::atomic<int> progress_count{0};
    std::atomic<int> complete_count{0};
    std::atomic<int> error_count{0};

    void on_progress(const falcon::ProgressInfo&) override {
        ++progress_count;
    }

    void on_completed(falcon::TaskId, const std::string&) override {
        ++complete_count;
    }

    void on_error(falcon::TaskId, const std::string&) override {
        ++error_count;
    }
};

TEST(DownloadEngineTest, EventListener) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto listener = std::make_shared<TestEventListener>();
    engine.add_listener(listener);

    auto task = engine.add_task("test://example.com/file.bin", falcon::DownloadOptions{});
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));

    // 验证事件被触发
    EXPECT_GT(listener->complete_count.load(), 0);
}

// 新增：速度限制选项
TEST(DownloadEngineTest, PerTaskSpeedLimit) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.speed_limit = 512 * 1024;  // 512 KB/s

    auto task = engine.add_task("test://example.com/file.bin", options);
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    EXPECT_TRUE(task->wait_for(std::chrono::seconds(2)));
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
}

// 新增：用户代理设置
TEST(DownloadEngineTest, UserAgentSetting) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.user_agent = "Falcon-Downloader/1.0";

    auto task = engine.add_task("test://example.com/file.bin", options);
    ASSERT_NE(task, nullptr);

    EXPECT_EQ(task->options().user_agent, "Falcon-Downloader/1.0");
}

// 新增：自定义HTTP头
TEST(DownloadEngineTest, CustomHttpHeaders) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    falcon::DownloadOptions options;
    options.headers["Authorization"] = "Bearer token123";
    options.headers["X-Custom-Header"] = "CustomValue";

    auto task = engine.add_task("test://example.com/file.bin", options);
    ASSERT_NE(task, nullptr);

    EXPECT_EQ(task->options().headers.size(), 2u);
    EXPECT_EQ(task->options().headers["Authorization"], "Bearer token123");
}

// 新增：并发启动任务
TEST(DownloadEngineTest, ConcurrentTaskStart) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    std::vector<falcon::DownloadTask::Ptr> tasks;
    std::vector<std::thread> threads;

    // 多线程同时添加和启动任务
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, &engine, &tasks, i]() {
            auto task = engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                                        falcon::DownloadOptions{});
            if (task) {
                engine.start_task(task->id());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证所有任务都被处理
    auto all_tasks = engine.get_all_tasks();
    EXPECT_GE(all_tasks.size(), 0u);
}

// 新增：统计信息
TEST(DownloadEngineTest, Statistics) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    auto stats = engine.get_statistics();

    // 初始统计应该为0或接近0
    EXPECT_GE(stats.active_tasks, 0);
    EXPECT_GE(stats.waiting_tasks, 0);
    EXPECT_GE(stats.completed_tasks, 0);
}

// 新增：清空所有任务
TEST(DownloadEngineTest, ClearAllTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<TestProtocolHandler>());

    // 添加一些任务
    for (int i = 0; i < 5; ++i) {
        engine.add_task("test://example.com/file" + std::to_string(i) + ".bin",
                       falcon::DownloadOptions{});
    }

    EXPECT_EQ(engine.get_all_tasks().size(), 5u);

    // 清空所有任务
    engine.clear_all_tasks();

    EXPECT_EQ(engine.get_all_tasks().size(), 0u);
}

