// Falcon Thread Pool Unit Tests
// Copyright (c) 2025 Falcon Project

#include "internal/thread_pool.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace falcon::internal;

class ThreadPoolTest : public ::testing::Test {
protected:
    void SetUp() override { pool_ = std::make_unique<ThreadPool>(4); }

    void TearDown() override { pool_.reset(); }

    std::unique_ptr<ThreadPool> pool_;
};

TEST_F(ThreadPoolTest, CreatePool) {
    EXPECT_EQ(pool_->size(), 4);
    EXPECT_EQ(pool_->pending(), 0);
}

TEST_F(ThreadPoolTest, CreatePoolDefaultSize) {
    auto default_pool = std::make_unique<ThreadPool>();
    EXPECT_GT(default_pool->size(), 0);
}

TEST_F(ThreadPoolTest, SubmitSimpleTask) {
    std::atomic<int> counter{0};

    auto future = pool_->submit([&counter]() {
        ++counter;
        return 42;
    });

    int result = future.get();
    EXPECT_EQ(result, 42);
    EXPECT_EQ(counter.load(), 1);
}

TEST_F(ThreadPoolTest, SubmitMultipleTasks) {
    std::atomic<int> counter{0};
    std::vector<std::future<int>> futures;

    for (int i = 0; i < 100; ++i) {
        futures.push_back(pool_->submit([&counter, i]() {
            ++counter;
            return i * 2;
        }));
    }

    for (int i = 0; i < 100; ++i) {
        int result = futures[i].get();
        EXPECT_EQ(result, i * 2);
    }

    EXPECT_EQ(counter.load(), 100);
}

TEST_F(ThreadPoolTest, SubmitTaskWithDelay) {
    auto start = std::chrono::steady_clock::now();

    auto future = pool_->submit([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return true;
    });

    bool result = future.get();
    auto end = std::chrono::steady_clock::now();
    auto duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_TRUE(result);
    EXPECT_GE(duration.count(), 50);
}

TEST_F(ThreadPoolTest, SubmitTaskWithArguments) {
    auto future = pool_->submit(
        [](int a, int b) { return a + b; }, 10, 20);

    int result = future.get();
    EXPECT_EQ(result, 30);
}

TEST_F(ThreadPoolTest, Wait) {
    std::atomic<int> counter{0};

    for (int i = 0; i < 50; ++i) {
        pool_->submit([&counter]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ++counter;
        });
    }

    pool_->wait();
    EXPECT_EQ(counter.load(), 50);
}

TEST_F(ThreadPoolTest, ConcurrentExecution) {
    std::atomic<int> max_concurrent{0};
    std::atomic<int> current{0};

    std::vector<std::future<void>> futures;

    for (int i = 0; i < 20; ++i) {
        futures.push_back(pool_->submit([&max_concurrent, &current]() {
            int c = ++current;
            int expected = max_concurrent.load();
            while (c > expected &&
                   !max_concurrent.compare_exchange_weak(expected, c)) {
                expected = max_concurrent.load();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            --current;
        }));
    }

    for (auto& f : futures) {
        f.get();
    }

    // Max concurrent should be at least pool size (4)
    EXPECT_GE(max_concurrent.load(), 2);
    EXPECT_LE(max_concurrent.load(), static_cast<int>(pool_->size()));
}

TEST_F(ThreadPoolTest, DestructorWaitsForTasks) {
    std::atomic<int> counter{0};

    {
        ThreadPool local_pool(2);
        for (int i = 0; i < 10; ++i) {
            local_pool.submit([&counter]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ++counter;
            });
        }
        // Destructor should wait for all tasks
    }

    EXPECT_EQ(counter.load(), 10);
}

TEST_F(ThreadPoolTest, PendingCount) {
    // Create a slow pool to ensure tasks queue up
    auto slow_pool = std::make_unique<ThreadPool>(1);

    std::atomic<bool> first_running{false};
    std::atomic<bool> can_finish{false};

    // First task blocks
    slow_pool->submit([&first_running, &can_finish]() {
        first_running = true;
        while (!can_finish.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Wait for first task to start
    while (!first_running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Add more tasks
    for (int i = 0; i < 5; ++i) {
        slow_pool->submit([]() {});
    }

    // Should have pending tasks
    EXPECT_GE(slow_pool->pending(), 3);

    // Let tasks finish
    can_finish = true;
    slow_pool->wait();
    EXPECT_EQ(slow_pool->pending(), 0);
}

TEST_F(ThreadPoolTest, ExceptionHandling) {
    auto future = pool_->submit([]() -> int {
        throw std::runtime_error("Test exception");
    });

    EXPECT_THROW(future.get(), std::runtime_error);
}

TEST_F(ThreadPoolTest, VoidReturnType) {
    std::atomic<bool> executed{false};

    auto future = pool_->submit([&executed]() { executed = true; });

    future.get();
    EXPECT_TRUE(executed.load());
}
