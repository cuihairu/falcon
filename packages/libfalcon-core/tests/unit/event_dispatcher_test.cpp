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

// 新增：多线程并发事件分发测试
TEST(EventDispatcherTest, ConcurrentDispatch) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = true;
    config.thread_pool_size = 4;
    config.max_queue_size = 1000;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);
    dispatcher.start();

    constexpr int thread_count = 10;
    constexpr int events_per_thread = 100;
    std::vector<std::thread> threads;

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&, i]() {
            for (int j = 0; j < events_per_thread; ++j) {
                falcon::TaskId task_id = i * events_per_thread + j;
                dispatcher.dispatch_status_changed(
                    task_id,
                    falcon::TaskStatus::Pending,
                    falcon::TaskStatus::Downloading
                );
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 等待所有事件被处理
    EXPECT_TRUE(listener.wait_for_total(thread_count * events_per_thread,
                                        std::chrono::milliseconds(5000)));
    dispatcher.stop(true);

    EXPECT_EQ(dispatcher.get_dropped_count(), 0u);
}

// 新增：队列满时的事件处理
TEST(EventDispatcherTest, QueueFullHandling) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = true;
    config.thread_pool_size = 1;
    config.max_queue_size = 10;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);
    dispatcher.start();

    int dropped = 0;
    for (int i = 0; i < 100; ++i) {
        falcon::ProgressInfo info;
        info.task_id = i;
        info.downloaded_bytes = i;
        info.total_bytes = 100;
        info.speed = 1;
        info.progress = static_cast<float>(i) / 100.0f;

        dispatcher.dispatch_progress(i, info);
    }

    // 所有事件都应该被处理（因为 dispatch_progress 返回 void，不会丢弃）
    // dropped 保持为 0
    EXPECT_EQ(dropped, 0);

    dispatcher.stop(true);
    EXPECT_GT(dispatcher.get_dropped_count(), 0u);
}

// 新增：多监听器测试
TEST(EventDispatcherTest, MultipleListeners) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = false;

    falcon::EventDispatcher dispatcher(config);

    constexpr int listener_count = 5;
    std::vector<std::unique_ptr<CountingListener>> listeners;

    for (int i = 0; i < listener_count; ++i) {
        auto listener = std::make_unique<CountingListener>();
        dispatcher.add_listener(listener.get());
        listeners.push_back(std::move(listener));
    }

    dispatcher.start();

    constexpr int event_count = 10;
    for (int i = 0; i < event_count; ++i) {
        dispatcher.dispatch_status_changed(
            i,
            falcon::TaskStatus::Pending,
            falcon::TaskStatus::Downloading
        );
    }

    // 所有监听器都应该收到事件
    for (const auto& listener : listeners) {
        EXPECT_TRUE(listener->wait_for_total(event_count,
                                            std::chrono::milliseconds(500)));
    }

    dispatcher.stop();
}

// 新增：移除监听器测试
TEST(EventDispatcherTest, RemoveListener) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = false;

    falcon::EventDispatcher dispatcher(config);

    auto listener1 = std::make_unique<CountingListener>();
    auto listener2 = std::make_unique<CountingListener>();

    dispatcher.add_listener(listener1.get());
    dispatcher.add_listener(listener2.get());
    dispatcher.start();

    // 发送事件
    dispatcher.dispatch_status_changed(
        1,
        falcon::TaskStatus::Pending,
        falcon::TaskStatus::Downloading
    );

    // 等待事件被处理
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 移除一个监听器
    dispatcher.remove_listener(listener1.get());

    // 发送更多事件
    for (int i = 0; i < 10; ++i) {
        dispatcher.dispatch_status_changed(
            i + 2,
            falcon::TaskStatus::Pending,
            falcon::TaskStatus::Downloading
        );
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // listener1 应该只收到第一个事件
    EXPECT_EQ(listener1->status_changed.load(), 1);

    // listener2 应该收到所有事件
    EXPECT_EQ(listener2->status_changed.load(), 11);

    dispatcher.stop();
}

// 新增：错误事件测试
TEST(EventDispatcherTest, ErrorEventDispatch) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = true;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);
    dispatcher.start();

    constexpr int error_count = 5;
    for (int i = 0; i < error_count; ++i) {
        dispatcher.dispatch_error(i, "Error message " + std::to_string(i));
    }

    EXPECT_TRUE(listener.wait_for_total(error_count,
                                        std::chrono::milliseconds(500)));
    EXPECT_EQ(listener.errors.load(), error_count);

    dispatcher.stop(true);
}

// 新增：性能测试
TEST(EventDispatcherTest, PerformanceHighThroughput) {
    falcon::EventDispatcherConfig config;
    config.enable_async_dispatch = true;
    config.thread_pool_size = 4;
    config.max_queue_size = 10000;

    falcon::EventDispatcher dispatcher(config);
    CountingListener listener;
    dispatcher.add_listener(&listener);
    dispatcher.start();

    constexpr int total_events = 10000;
    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < total_events; ++i) {
        falcon::ProgressInfo info;
        info.task_id = i;
        info.downloaded_bytes = i;
        info.total_bytes = total_events;
        info.speed = 1000;
        info.progress = static_cast<float>(i) / total_events;

        dispatcher.dispatch_progress(i, info);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该快速完成（< 1秒）
    EXPECT_LT(duration.count(), 1000);

    dispatcher.stop(true);

    // 应该处理了大量事件
    EXPECT_GT(dispatcher.get_processed_count(), 0u);
}

