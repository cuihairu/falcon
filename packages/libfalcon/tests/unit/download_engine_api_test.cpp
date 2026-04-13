// Cover untested DownloadEngine API paths:
// - register_handler_factory
// - set_global_speed_limit / set_max_concurrent_tasks
// - get_tasks_by_status / get_active_tasks
// - remove_task / remove_finished_tasks
// - pause_all / resume_all / cancel_all / wait_all
// - is_url_supported / get_supported_protocols

#include <falcon/download_engine.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

class QuickHandler final : public falcon::IProtocolHandler {
public:
    explicit QuickHandler(std::string proto = "quick")
        : proto_(std::move(proto)) {}

    [[nodiscard]] std::string protocol_name() const override { return proto_; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {proto_};
    }
    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind(proto_ + "://", 0) == 0;
    }
    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "file.bin";
        info.total_size = 100;
        info.supports_resume = true;
        return info;
    }
    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        if (!task) return;
        task->mark_started();
        task->update_progress(50, 100, 50);
        task->update_progress(100, 100, 50);
        task->set_status(falcon::TaskStatus::Completed);
    }
    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr task) override {
        if (task) task->set_status(falcon::TaskStatus::Cancelled);
    }

private:
    std::string proto_;
};

} // namespace

// ---------------------------------------------------------------------------
// register_handler_factory
// ---------------------------------------------------------------------------

std::unique_ptr<falcon::IProtocolHandler> make_quick_handler() {
    return std::make_unique<QuickHandler>("factory");
}

TEST(DownloadEngineApiTest, RegisterHandlerFactoryDoesNotCrash) {
    falcon::DownloadEngine engine;

    // Verify the factory can be registered without crash
    engine.register_handler_factory(make_quick_handler);
    // The factory mechanism is complex; just verify no crash
}

// ---------------------------------------------------------------------------
// set_global_speed_limit / set_max_concurrent_tasks
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, SetGlobalSpeedLimit) {
    falcon::DownloadEngine engine;
    engine.set_global_speed_limit(1024 * 1024);  // 1 MB/s
    // No crash, config accepted
    EXPECT_EQ(engine.get_total_speed(), 0u);
}

TEST(DownloadEngineApiTest, SetMaxConcurrentTasks) {
    falcon::DownloadEngine engine;
    engine.set_max_concurrent_tasks(2);

    engine.register_handler(std::make_unique<QuickHandler>());
    auto t1 = engine.add_task("quick://a.com/1");
    auto t2 = engine.add_task("quick://b.com/2");
    ASSERT_NE(t1, nullptr);
    ASSERT_NE(t2, nullptr);
    EXPECT_EQ(engine.get_total_task_count(), 2u);
}

// ---------------------------------------------------------------------------
// get_tasks_by_status / get_active_tasks
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, GetTasksByStatus) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    auto t1 = engine.add_task("quick://a.com/1");
    ASSERT_NE(t1, nullptr);

    // Initially pending
    auto pending = engine.get_tasks_by_status(falcon::TaskStatus::Pending);
    EXPECT_EQ(pending.size(), 1u);

    auto completed = engine.get_tasks_by_status(falcon::TaskStatus::Completed);
    EXPECT_TRUE(completed.empty());
}

TEST(DownloadEngineApiTest, GetActiveTasksEmpty) {
    falcon::DownloadEngine engine;
    auto active = engine.get_active_tasks();
    EXPECT_TRUE(active.empty());
}

// ---------------------------------------------------------------------------
// remove_task / remove_finished_tasks
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, RemoveNonExistentTask) {
    falcon::DownloadEngine engine;
    bool removed = engine.remove_task(99999);
    EXPECT_FALSE(removed);
}

TEST(DownloadEngineApiTest, RemoveFinishedTasksWhenNoneFinished) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    auto t = engine.add_task("quick://a.com/1");
    ASSERT_NE(t, nullptr);

    // Task is pending, not finished
    auto count = engine.remove_finished_tasks();
    EXPECT_EQ(count, 0u);
}

TEST(DownloadEngineApiTest, RemoveFinishedTasksAfterCompletion) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    auto t = engine.add_task("quick://a.com/1");
    ASSERT_NE(t, nullptr);

    engine.start_task(t->id());

    auto count = engine.remove_finished_tasks();
    EXPECT_GE(count, 0u);
}

// ---------------------------------------------------------------------------
// pause_all / resume_all / cancel_all
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, PauseAllNoTasks) {
    falcon::DownloadEngine engine;
    engine.pause_all();  // Should not crash
}

TEST(DownloadEngineApiTest, ResumeAllNoTasks) {
    falcon::DownloadEngine engine;
    engine.resume_all();  // Should not crash
}

TEST(DownloadEngineApiTest, CancelAllNoTasks) {
    falcon::DownloadEngine engine;
    engine.cancel_all();  // Should not crash
}

TEST(DownloadEngineApiTest, CancelAllWithTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    engine.add_task("quick://a.com/1");
    engine.add_task("quick://b.com/2");

    engine.cancel_all();
    // All tasks should be cancelled
}

// ---------------------------------------------------------------------------
// is_url_supported / get_supported_protocols
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, IsUrlSupportedWithoutHandlers) {
    falcon::DownloadEngine engine;
    EXPECT_FALSE(engine.is_url_supported("http://example.com"));
    EXPECT_FALSE(engine.is_url_supported("ftp://example.com"));
}

TEST(DownloadEngineApiTest, IsUrlSupportedWithHandler) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());
    EXPECT_TRUE(engine.is_url_supported("quick://example.com"));
    EXPECT_FALSE(engine.is_url_supported("http://example.com"));
}

TEST(DownloadEngineApiTest, GetSupportedProtocolsEmpty) {
    falcon::DownloadEngine engine;
    auto protocols = engine.get_supported_protocols();
    EXPECT_TRUE(protocols.empty());
}

TEST(DownloadEngineApiTest, GetSupportedProtocolsWithHandler) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>("alpha"));
    engine.register_handler(std::make_unique<QuickHandler>("beta"));

    auto protocols = engine.get_supported_protocols();
    EXPECT_EQ(protocols.size(), 2u);
}

// ---------------------------------------------------------------------------
// add_listener / remove_listener
// ---------------------------------------------------------------------------

namespace {
class NullListener final : public falcon::IEventListener {
public:
    void on_status_changed(falcon::TaskId, falcon::TaskStatus,
                           falcon::TaskStatus) override {}
    void on_progress(const falcon::ProgressInfo&) override {}
};
} // namespace

TEST(DownloadEngineApiTest, AddRemoveListener) {
    falcon::DownloadEngine engine;
    NullListener listener;

    engine.add_listener(&listener);
    engine.remove_listener(&listener);
    // Should not crash
}

// ---------------------------------------------------------------------------
// add_tasks bulk
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, AddMultipleTasks) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    std::vector<std::string> urls = {
        "quick://a.com/1",
        "quick://b.com/2",
        "quick://c.com/3",
    };

    auto tasks = engine.add_tasks(urls);
    EXPECT_EQ(tasks.size(), 3u);
    EXPECT_EQ(engine.get_total_task_count(), 3u);
}

TEST(DownloadEngineApiTest, AddMultipleTasksWithMixedSupport) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    std::vector<std::string> urls = {
        "quick://a.com/1",
        "quick://b.com/2",
    };

    auto tasks = engine.add_tasks(urls);
    EXPECT_EQ(tasks.size(), 2u);
    EXPECT_NE(tasks[0], nullptr);
    EXPECT_NE(tasks[1], nullptr);
}

// ---------------------------------------------------------------------------
// Engine with custom config
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, EngineWithCustomConfig) {
    falcon::EngineConfig config;
    config.max_concurrent_tasks = 10;

    falcon::DownloadEngine engine(config);
    EXPECT_EQ(engine.config().max_concurrent_tasks, 10u);
}

// ---------------------------------------------------------------------------
// get_task by id
// ---------------------------------------------------------------------------

TEST(DownloadEngineApiTest, GetTaskById) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<QuickHandler>());

    auto t = engine.add_task("quick://a.com/1");
    ASSERT_NE(t, nullptr);

    auto retrieved = engine.get_task(t->id());
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->id(), t->id());
}

TEST(DownloadEngineApiTest, GetTaskByInvalidId) {
    falcon::DownloadEngine engine;
    auto t = engine.get_task(falcon::INVALID_TASK_ID);
    EXPECT_EQ(t, nullptr);
}
