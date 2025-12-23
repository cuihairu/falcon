// Falcon Event Dispatcher Unit Tests

#include <falcon/event_dispatcher.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace {

class CountingListener final : public falcon::IEventListener {
public:
    void on_status_changed(falcon::TaskId, falcon::TaskStatus, falcon::TaskStatus) override {
        status_changed.fetch_add(1);
        notify();
    }

    void on_progress(const falcon::ProgressInfo&) override {
        progress.fetch_add(1);
        notify();
    }

    void on_error(falcon::TaskId, const std::string&) override {
        errors.fetch_add(1);
        notify();
    }

    void on_completed(falcon::TaskId, const std::string&) override {
        completed.fetch_add(1);
        notify();
    }

    bool wait_for_total(int expected, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mu);
        return cv.wait_for(lock, timeout, [&] { return total() >= expected; });
    }

    int total() const {
        return status_changed.load() + progress.load() + errors.load() + completed.load();
    }

    std::atomic<int> status_changed{0};
    std::atomic<int> progress{0};
    std::atomic<int> errors{0};
    std::atomic<int> completed{0};

private:
    void notify() {
        std::lock_guard<std::mutex> lock(mu);
        cv.notify_all();
    }

    mutable std::mutex mu;
    std::condition_variable cv;
};

} // namespace

TEST(EventDispatcherTest, DispatchSyncDoesNotQueue) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = false;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);
    dispatcher.start();

    dispatcher.dispatch_status_changed(1, falcon::TaskStatus::Pending, falcon::TaskStatus::Downloading);
    EXPECT_TRUE(listener.wait_for_total(1, std::chrono::milliseconds(250)));

    EXPECT_EQ(dispatcher.get_queue_size(), 0u);
    EXPECT_EQ(dispatcher.get_dropped_count(), 0u);
    EXPECT_GE(dispatcher.get_processed_count(), 1u);
}

TEST(EventDispatcherTest, DropWhenNotRunning) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = true;
    config.max_queue_size = 1;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);

    EXPECT_TRUE(dispatcher.dispatch(std::make_shared<falcon::StatusChangedEvent>(
        1, falcon::TaskStatus::Pending, falcon::TaskStatus::Preparing)));
    EXPECT_FALSE(dispatcher.dispatch(std::make_shared<falcon::StatusChangedEvent>(
        2, falcon::TaskStatus::Pending, falcon::TaskStatus::Preparing)));

    EXPECT_EQ(dispatcher.get_queue_size(), 1u);
    EXPECT_EQ(dispatcher.get_dropped_count(), 1u);
    EXPECT_EQ(listener.total(), 0);
}

TEST(EventDispatcherTest, AsyncDispatchDeliversEvents) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = true;
    config.thread_pool_size = 1;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);
    dispatcher.start();

    falcon::ProgressInfo info;
    info.task_id = 7;
    info.downloaded_bytes = 1;
    info.total_bytes = 2;
    info.speed = 1;
    info.progress = 0.5f;

    dispatcher.dispatch_progress(7, info);
    dispatcher.dispatch_completed(7, "out.bin", 2, falcon::Duration{0});

    EXPECT_TRUE(listener.wait_for_total(2, std::chrono::milliseconds(500)));
    dispatcher.stop(true);

    EXPECT_EQ(dispatcher.get_dropped_count(), 0u);
    EXPECT_GE(dispatcher.get_processed_count(), 2u);
}

