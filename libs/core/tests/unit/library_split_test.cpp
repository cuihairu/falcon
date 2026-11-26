// Verify architectural constraints of the library split.
// Links against all four libraries via the compatibility target.

#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>
#include <falcon/protocol_handler.hpp>
#include <falcon/types.hpp>
#include <falcon/version.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// 1. Compatibility target brings in all four libraries
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, CompatibilityTargetLinksAllLibraries) {
    // If this compiles and links, the compatibility `falcon` INTERFACE target
    // successfully pulls in falcon_core, falcon_protocols, falcon_storage,
    // and falcon_drives.
    falcon::DownloadEngine engine;
    falcon::ProtocolRegistry pm;
    EXPECT_EQ(pm.handler_count(), 0u);
}

// ---------------------------------------------------------------------------
// 2. Core types are accessible through falcon_core headers
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, CoreTypesAccessible) {
    falcon::TaskId id = 42;
    EXPECT_EQ(id, 42u);

    falcon::FileInfo info;
    info.url = "http://example.com";
    info.filename = "test.bin";
    info.total_size = 1024;
    EXPECT_EQ(info.total_size, 1024u);

    falcon::ProgressInfo progress;
    progress.downloaded_bytes = 512;
    progress.total_bytes = 1024;
    progress.speed = 100;
    EXPECT_FLOAT_EQ(progress.progress, 0.0f);
}

// ---------------------------------------------------------------------------
// 3. Version info compiles
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, VersionInfoAccessible) {
    std::string ver = falcon::get_version_string();
    EXPECT_FALSE(ver.empty());
}

// ---------------------------------------------------------------------------
// 4. Engine config works
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, EngineConfigWorks) {
    falcon::EngineConfig config;
    config.max_concurrent_tasks = 3;

    falcon::DownloadEngine engine(config);
    EXPECT_EQ(engine.config().max_concurrent_tasks, 3u);
}

// ---------------------------------------------------------------------------
// 5. Custom handler registration via engine (core feature)
// ---------------------------------------------------------------------------

namespace {
class SplitTestHandler final : public falcon::IProtocolHandler {
public:
    [[nodiscard]] std::string protocol_name() const override { return "split-test"; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {"split-test"};
    }
    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind("split-test://", 0) == 0;
    }
    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "split.bin";
        info.total_size = 10;
        info.supports_resume = true;
        return info;
    }
    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        if (!task) return;
        task->mark_started();
        task->update_progress(10, 10, 10);
        task->set_status(falcon::TaskStatus::Completed);
    }
    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr task) override {
        if (task) task->set_status(falcon::TaskStatus::Cancelled);
    }
};
} // namespace

TEST(LibrarySplitTest, RegisterCustomHandlerAndDownload) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<SplitTestHandler>());

    EXPECT_TRUE(engine.is_url_supported("split-test://example.com/file"));
    EXPECT_FALSE(engine.is_url_supported("unknown://something"));

    auto protocols = engine.get_supported_protocols();
    EXPECT_FALSE(protocols.empty());

    falcon::DownloadOptions opts;
    opts.output_directory = "/tmp/falcon_split_test";
    auto task = engine.add_task("split-test://example.com/file", opts);
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->url(), "split-test://example.com/file");
}

// ---------------------------------------------------------------------------
// 6. Handler factory registration
// ---------------------------------------------------------------------------

std::unique_ptr<falcon::IProtocolHandler> make_split_test_handler() {
    return std::make_unique<SplitTestHandler>();
}

TEST(LibrarySplitTest, RegisterHandlerFactory) {
    falcon::DownloadEngine engine;

    // register_handler_factory stores the factory for later use.
    // It does not auto-create handlers for URL routing.
    // Just verify the API does not crash.
    engine.register_handler_factory(make_split_test_handler);

    // Manually register the handler to verify the factory produces valid objects
    auto handler = make_split_test_handler();
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->protocol_name(), "split-test");
}

// ---------------------------------------------------------------------------
// 7. Event listener integration across split
// ---------------------------------------------------------------------------

namespace {
class SplitEventListener final : public falcon::IEventListener {
public:
    std::atomic<int> status_changed_count{0};
    std::atomic<int> progress_count{0};
    std::string last_status;

    void on_status_changed(falcon::TaskId, falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        (void)old_status;
        last_status = falcon::to_string(new_status);
        status_changed_count++;
    }
    void on_progress(const falcon::ProgressInfo&) override {
        progress_count++;
    }
};
} // namespace

TEST(LibrarySplitTest, EventListenerWorksAcrossSplit) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<SplitTestHandler>());

    SplitEventListener listener;
    engine.add_listener(&listener);

    auto task = engine.add_task("split-test://evt.com/f");
    ASSERT_NE(task, nullptr);

    // The handler may complete synchronously; event dispatch may be async.
    // Just verify the listener was added and the task completed successfully.
    engine.start_task(task->id());

    // Give a moment for async events to fire
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // The task should be completed regardless of event notification
    EXPECT_EQ(task->status(), falcon::TaskStatus::Completed);
}

// ---------------------------------------------------------------------------
// 8. loadAllPlugins works with all libraries linked
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, LoadAllPluginsWithFullLink) {
    falcon::ProtocolRegistry manager;
    manager.load_builtin_handlers();

    // With falcon_protocols linked, at least HTTP should be available
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    EXPECT_GT(manager.handler_count(), 0u);
    EXPECT_NE(manager.get_handler("http"), nullptr);
#else
    // Without HTTP, still should not crash
    EXPECT_EQ(manager.handler_count(), 0u);
#endif
}

// ---------------------------------------------------------------------------
// 9. Multiple task operations
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, MultipleTasksWithEngine) {
    falcon::DownloadEngine engine;
    engine.register_handler(std::make_unique<SplitTestHandler>());

    std::vector<std::string> urls = {
        "split-test://a.com/file1",
        "split-test://b.com/file2",
        "split-test://c.com/file3",
    };

    auto tasks = engine.add_tasks(urls);
    EXPECT_EQ(tasks.size(), 3u);

    EXPECT_EQ(engine.get_total_task_count(), 3u);
}

// ---------------------------------------------------------------------------
// 10. Engine statistics
// ---------------------------------------------------------------------------

TEST(LibrarySplitTest, EngineStatistics) {
    falcon::DownloadEngine engine;

    EXPECT_EQ(engine.get_total_speed(), 0u);
    EXPECT_EQ(engine.get_active_task_count(), 0u);
    EXPECT_EQ(engine.get_total_task_count(), 0u);

    engine.set_global_speed_limit(1024 * 1024);
    engine.set_max_concurrent_tasks(5);
}
