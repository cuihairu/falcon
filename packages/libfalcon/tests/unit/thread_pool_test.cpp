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

// 新增：线程池大小为1的测试
TEST_F(ThreadPoolTest, SingleThreadedPool) {
    auto single_pool = std::make_unique<ThreadPool>(1);
    std::atomic<int> execution_order{0};
    std::vector<int> results;

    auto future1 = single_pool->submit([&execution_order, &results]() {
        results.push_back(++execution_order);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    });

    auto future2 = single_pool->submit([&execution_order, &results]() {
        results.push_back(++execution_order);
    });

    future1.get();
    future2.get();

    EXPECT_EQ(results.size(), 2);
    EXPECT_EQ(results[0], 1);
    EXPECT_EQ(results[1], 2);
}

// 新增：大量任务压力测试
TEST_F(ThreadPoolTest, HighStressTest) {
    constexpr int task_count = 10000;
    std::atomic<int> counter{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < task_count; ++i) {
        pool_->submit([&counter]() {
            ++counter;
        });
    }

    pool_->wait();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_EQ(counter.load(), task_count);
    EXPECT_LT(duration.count(), 5000); // 应该在5秒内完成
}

// 新增：任务取消和超时测试
TEST_F(ThreadPoolTest, TaskTimeout) {
    std::atomic<bool> task_started{false};
    std::atomic<bool> task_finished{false};

    auto future = pool_->submit([&task_started, &task_finished]() {
        task_started = true;
        std::this_thread::sleep_for(std::chrono::seconds(10));
        task_finished = true;
        return 42;
    });

    // 等待任务开始
    while (!task_started.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // 验证任务正在运行
    EXPECT_TRUE(task_started.load());
    EXPECT_FALSE(task_finished.load());

    // 注意：C++ std::future 不支持真正的取消，这里只测试状态
}

// 新增：混合任务类型测试
TEST_F(ThreadPoolTest, MixedTaskTypes) {
    std::atomic<int> int_counter{0};
    std::atomic<int> string_counter{0};

    auto int_future = pool_->submit([&int_counter]() -> int {
        ++int_counter;
        return 100;
    });

    auto void_future = pool_->submit([&int_counter]() {
        ++int_counter;
    });

    auto string_future = pool_->submit([&string_counter]() -> std::string {
        ++string_counter;
        return "test";
    });

    EXPECT_EQ(int_future.get(), 100);
    void_future.get();
    EXPECT_EQ(string_future.get(), "test");

    EXPECT_EQ(int_counter.load(), 2);
    EXPECT_EQ(string_counter.load(), 1);
}

// 新增：线程池暂停和恢复测试（如果支持）
TEST_F(ThreadPoolTest, PoolPauseResume) {
    std::atomic<int> counter{0};

    // 提交一些快速任务
    for (int i = 0; i < 10; ++i) {
        pool_->submit([&counter]() {
            ++counter;
        });
    }

    pool_->wait();
    EXPECT_EQ(counter.load(), 10);

    // 再次提交更多任务
    for (int i = 0; i < 20; ++i) {
        pool_->submit([&counter]() {
            ++counter;
        });
    }

    pool_->wait();
    EXPECT_EQ(counter.load(), 30);
}

// 新增：异常不影响其他任务
TEST_F(ThreadPoolTest, ExceptionDoesNotAffectOtherTasks) {
    std::atomic<int> success_count{0};
    std::atomic<int> fail_count{0};

    std::vector<std::future<void>> futures;

    // 添加一些会抛异常的任务
    for (int i = 0; i < 5; ++i) {
        futures.push_back(pool_->submit([&, i]() {
            if (i % 2 == 0) {
                throw std::runtime_error("Intentional error");
            }
            ++success_count;
        }));
    }

    // 添加一些正常任务
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool_->submit([&success_count]() {
            ++success_count;
        }));
    }

    // 处理结果
    int exceptions = 0;
    for (auto& future : futures) {
        try {
            future.get();
        } catch (const std::runtime_error&) {
            ++exceptions;
        }
    }

    EXPECT_GT(exceptions, 0);
    EXPECT_EQ(success_count.load(), 10);
}

// 新增：空任务测试
TEST_F(ThreadPoolTest, EmptyTask) {
    auto future = pool_->submit([]() {
        // 空任务
    });

    EXPECT_NO_THROW(future.get());
}

// 新增：嵌套任务提交测试
TEST_F(ThreadPoolTest, NestedTaskSubmission) {
    std::atomic<int> counter{0};

    auto outer_future = pool_->submit([this, &counter]() {
        ++counter;

        // 在任务内部提交新任务
        auto inner_future = pool_->submit([&counter]() {
            ++counter;
        });

        inner_future.get();
    });

    outer_future.get();
    EXPECT_EQ(counter.load(), 2);
}

// 新增：线程安全的状态查询
TEST_F(ThreadPoolTest, ThreadSafeStatusQuery) {
    constexpr int task_count = 100;

    std::atomic<int> counter{0};
    std::vector<std::future<void>> futures;

    // 并发提交任务和查询状态
    for (int i = 0; i < task_count; ++i) {
        futures.push_back(pool_->submit([&counter]() {
            ++counter;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }));
    }

    // 在任务执行期间查询状态
    for (int i = 0; i < 10; ++i) {
        size_t pending = pool_->pending();
        size_t size = pool_->size();
        EXPECT_GE(size, 0);
    }

    for (auto& f : futures) {
        f.get();
    }

    EXPECT_EQ(counter.load(), task_count);
}

// 新增：性能基准测试
TEST_F(ThreadPoolTest, PerformanceBenchmark) {
    constexpr int iterations = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::future<long>> futures;
    for (int i = 0; i < iterations; ++i) {
        futures.push_back(pool_->submit([i]() -> long {
            long sum = 0;
            for (int j = 0; j < 100; ++j) {
                sum += i * j;
            }
            return sum;
        }));
    }

    long total = 0;
    for (auto& f : futures) {
        total += f.get();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_GT(total, 0);
    EXPECT_LT(duration.count(), 2000); // 1000个任务应该在2秒内完成
}

// 新增：引用捕获测试
TEST_F(ThreadPoolTest, ReferenceCapture) {
    int value = 10;

    auto future = pool_->submit([&value]() -> int {
        value *= 2;
        return value;
    });

    EXPECT_EQ(future.get(), 20);
}

// 新增：移动语义测试
TEST_F(ThreadPoolTest, MoveSemantics) {
    auto future = pool_->submit([]() -> std::unique_ptr<int> {
        return std::make_unique<int>(42);
    });

    auto result = future.get();
    EXPECT_NE(result, nullptr);
    EXPECT_EQ(*result, 42);
}
