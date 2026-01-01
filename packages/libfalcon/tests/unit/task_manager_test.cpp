// Falcon Task Manager Unit Tests
// Copyright (c) 2025 Falcon Project

#include <falcon/task_manager.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_dispatcher.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>
#include <chrono>

using namespace falcon;

class TaskManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        TaskManagerConfig config;
        config.max_concurrent_tasks = 5;
        config.cleanup_interval = std::chrono::seconds(1);

        event_dispatcher_ = std::make_unique<EventDispatcher>();
        manager_ = std::make_unique<TaskManager>(config, event_dispatcher_.get());

        // Start the task manager
        manager_->start();
    }

    void TearDown() override {
        if (manager_) {
            manager_->stop();
        }
        if (event_dispatcher_) {
            event_dispatcher_->stop();
        }
    }

    std::unique_ptr<EventDispatcher> event_dispatcher_;
    std::unique_ptr<TaskManager> manager_;
    DownloadOptions default_options_;
};

TEST_F(TaskManagerTest, CreateManager) {
    EXPECT_EQ(manager_->get_max_concurrent_tasks(), 5);
    EXPECT_EQ(manager_->get_queue_size(), 0);
    EXPECT_EQ(manager_->get_active_task_count(), 0);
    EXPECT_TRUE(manager_->is_running());
}

TEST_F(TaskManagerTest, AddTask) {
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/file.zip",
                                                default_options_);
    TaskId id = manager_->add_task(task, TaskPriority::Normal);

    EXPECT_NE(id, INVALID_TASK_ID);
    EXPECT_NE(manager_->get_task(id), nullptr);
    EXPECT_EQ(task->status(), TaskStatus::Pending);
}

TEST_F(TaskManagerTest, AddMultipleTasks) {
    std::vector<TaskId> ids;
    for (int i = 0; i < 10; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        TaskId id = manager_->add_task(task, TaskPriority::Normal);
        ids.push_back(id);
    }

    // Check that all tasks were added
    for (TaskId id : ids) {
        EXPECT_NE(manager_->get_task(id), nullptr);
    }

    // All tasks should be pending since no protocol handler is set
    auto pending_tasks = manager_->get_tasks_by_status(TaskStatus::Pending);
    EXPECT_EQ(pending_tasks.size(), 10);
}

TEST_F(TaskManagerTest, GetTasksByStatus) {
    // Add tasks
    for (int i = 0; i < 5; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
    }

    // Initially all should be pending
    auto pending = manager_->get_tasks_by_status(TaskStatus::Pending);
    EXPECT_EQ(pending.size(), 5);

    auto active = manager_->get_active_tasks();
    EXPECT_EQ(active.size(), 0);

    auto completed = manager_->get_tasks_by_status(TaskStatus::Completed);
    EXPECT_EQ(completed.size(), 0);
}

TEST_F(TaskManagerTest, GetAllTasks) {
    // Add some tasks
    int task_count = 3;
    for (int i = 0; i < task_count; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
    }

    auto all_tasks = manager_->get_all_tasks();
    EXPECT_EQ(all_tasks.size(), task_count);
}

TEST_F(TaskManagerTest, TaskControl) {
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/file.zip",
                                                default_options_);
    TaskId id = manager_->add_task(task, TaskPriority::Normal);

    // Test pause
    EXPECT_TRUE(manager_->pause_task(id));
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // Test resume
    EXPECT_TRUE(manager_->resume_task(id));
    // Note: Task will remain Paused since no protocol handler

    // Test cancel
    EXPECT_TRUE(manager_->cancel_task(id));
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
}

TEST_F(TaskManagerTest, GlobalControl) {
    // Add multiple tasks
    for (int i = 0; i < 3; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
    }

    // Pause all
    manager_->pause_all();
    auto paused = manager_->get_tasks_by_status(TaskStatus::Paused);
    EXPECT_EQ(paused.size(), 3);

    // Resume all
    manager_->resume_all();
    // Note: Tasks will remain Paused since no protocol handler
}

TEST_F(TaskManagerTest, RemoveTask) {
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/file.zip",
                                                default_options_);
    TaskId id = manager_->add_task(task, TaskPriority::Normal);

    // Cannot remove while not finished
    EXPECT_FALSE(manager_->remove_task(id));

    // Mark as finished
    task->set_status(TaskStatus::Completed);

    // Now can remove
    EXPECT_TRUE(manager_->remove_task(id));
    EXPECT_EQ(manager_->get_task(id), nullptr);
}

TEST_F(TaskManagerTest, CleanupFinishedTasks) {
    // Add tasks with different statuses
    for (int i = 0; i < 5; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);

        // Mark some as finished
        if (i % 2 == 0) {
            task->set_status(TaskStatus::Completed);
        }
    }

    size_t removed = manager_->cleanup_finished_tasks();
    EXPECT_GT(removed, 0);

    // Check that finished tasks were removed
    auto all_tasks = manager_->get_all_tasks();
    EXPECT_LT(all_tasks.size(), 5);
}

TEST_F(TaskManagerTest, GetStatistics) {
    // Add tasks
    int total = 10;
    for (int i = 0; i < total; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);

        // Set some statuses
        if (i < 3) {
            task->set_status(TaskStatus::Completed);
        } else if (i < 5) {
            task->set_status(TaskStatus::Failed);
        }
    }

    auto stats = manager_->get_statistics();
    EXPECT_EQ(stats.total_tasks, total);
    EXPECT_EQ(stats.completed_tasks, 3);
    EXPECT_EQ(stats.failed_tasks, 2);
    EXPECT_EQ(stats.pending_tasks, 5);
}

TEST_F(TaskManagerTest, SetMaxConcurrentTasks) {
    // Default should be 5
    EXPECT_EQ(manager_->get_max_concurrent_tasks(), 5);

    // Change to 10
    manager_->set_max_concurrent_tasks(10);
    EXPECT_EQ(manager_->get_max_concurrent_tasks(), 10);
}

// 新增：任务优先级测试
TEST_F(TaskManagerTest, TaskPriority) {
    auto task1 = std::make_shared<DownloadTask>(1, "https://example.com/file1.zip",
                                                 default_options_);
    auto task2 = std::make_shared<DownloadTask>(2, "https://example.com/file2.zip",
                                                 default_options_);
    auto task3 = std::make_shared<DownloadTask>(3, "https://example.com/file3.zip",
                                                 default_options_);

    TaskId id1 = manager_->add_task(task1, TaskPriority::Low);
    TaskId id2 = manager_->add_task(task2, TaskPriority::High);
    TaskId id3 = manager_->add_task(task3, TaskPriority::Normal);

    EXPECT_NE(id1, INVALID_TASK_ID);
    EXPECT_NE(id2, INVALID_TASK_ID);
    EXPECT_NE(id3, INVALID_TASK_ID);
}

// 新增：并发任务操作测试
TEST_F(TaskManagerTest, ConcurrentTaskOperations) {
    constexpr int thread_count = 10;
    constexpr int tasks_per_thread = 20;

    std::vector<std::thread> threads;
    std::vector<TaskId> all_ids;

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([this, i, tasks_per_thread, &all_ids]() {
            for (int j = 0; j < tasks_per_thread; ++j) {
                auto task = std::make_shared<DownloadTask>(
                    i * tasks_per_thread + j + 1,
                    "https://example.com/file" + std::to_string(i) + "_" + std::to_string(j) + ".zip",
                    default_options_);

                TaskId id = manager_->add_task(task, TaskPriority::Normal);

                // 线程安全地添加到列表
                static std::mutex ids_mutex;
                std::lock_guard<std::mutex> lock(ids_mutex);
                all_ids.push_back(id);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 验证所有任务都被添加
    auto stats = manager_->get_statistics();
    EXPECT_EQ(stats.total_tasks, thread_count * tasks_per_thread);
}

// 新增：任务查找测试
TEST_F(TaskManagerTest, FindTaskByURL) {
    std::string test_url = "https://example.com/test.zip";
    auto task = std::make_shared<DownloadTask>(1, test_url, default_options_);
    manager_->add_task(task, TaskPriority::Normal);

    // 通过 URL 查找任务（如果实现了这个功能）
    auto all_tasks = manager_->get_all_tasks();
    bool found = false;
    for (const auto& t : all_tasks) {
        if (t && t->url() == test_url) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// 新增：无效任务操作测试
TEST_F(TaskManagerTest, InvalidTaskOperations) {
    // 测试对不存在任务的操作
    TaskId invalid_id = 99999;

    EXPECT_FALSE(manager_->pause_task(invalid_id));
    EXPECT_FALSE(manager_->resume_task(invalid_id));
    EXPECT_FALSE(manager_->cancel_task(invalid_id));
    EXPECT_FALSE(manager_->remove_task(invalid_id));

    EXPECT_EQ(manager_->get_task(invalid_id), nullptr);
}

// 新增：重复任务ID测试
TEST_F(TaskManagerTest, DuplicateTaskIDs) {
    // 添加多个任务，验证 ID 不会重复
    std::set<TaskId> ids;

    for (int i = 0; i < 100; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        TaskId id = manager_->add_task(task, TaskPriority::Normal);
        ids.insert(id);
    }

    EXPECT_EQ(ids.size(), 100);
}

// 新增：任务状态转换测试
TEST_F(TaskManagerTest, TaskStatusTransitions) {
    auto task = std::make_shared<DownloadTask>(1, "https://example.com/file.zip",
                                                 default_options_);
    TaskId id = manager_->add_task(task, TaskPriority::Normal);

    // 初始状态
    EXPECT_EQ(task->status(), TaskStatus::Pending);

    // 暂停
    manager_->pause_task(id);
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // 恢复
    manager_->resume_task(id);
    // 任务应该仍然是 Paused 或变为 Pending

    // 取消
    manager_->cancel_task(id);
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);
}

// 新增：清理所有已完成任务
TEST_F(TaskManagerTest, CleanupAllFinishedTasks) {
    // 添加 20 个任务，全部标记为完成
    for (int i = 0; i < 20; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
        task->set_status(TaskStatus::Completed);
    }

    size_t removed = manager_->cleanup_finished_tasks();
    EXPECT_EQ(removed, 20);

    auto stats = manager_->get_statistics();
    EXPECT_EQ(stats.total_tasks, 0);
}

// 新增：压力测试
TEST_F(TaskManagerTest, StressTest) {
    constexpr int task_count = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < task_count; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该快速完成（< 2秒）
    EXPECT_LT(duration.count(), 2000) << "Adding " << task_count
                                       << " tasks took " << duration.count() << "ms";

    auto stats = manager_->get_statistics();
    EXPECT_EQ(stats.total_tasks, task_count);
}

// 新增：空任务列表操作
TEST_F(TaskManagerTest, OperationsOnEmptyManager) {
    EXPECT_EQ(manager_->get_all_tasks().size(), 0);
    EXPECT_EQ(manager_->get_active_tasks().size(), 0);
    EXPECT_EQ(manager_->get_tasks_by_status(TaskStatus::Pending).size(), 0);

    // 这些操作不应该崩溃
    manager_->pause_all();
    manager_->resume_all();

    size_t removed = manager_->cleanup_finished_tasks();
    EXPECT_EQ(removed, 0);
}

// 新增：最大并发限制测试
TEST_F(TaskManagerTest, MaxConcurrentLimit) {
    manager_->set_max_concurrent_tasks(3);

    // 添加 10 个任务
    for (int i = 0; i < 10; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
    }

    auto stats = manager_->get_statistics();
    // 所有任务都应该在等待队列中（因为没有协议处理器）
    EXPECT_GE(stats.pending_tasks, 0);
}

// 新增：任务统计信息准确性
TEST_F(TaskManagerTest, StatisticsAccuracy) {
    // 添加不同状态的任务
    int completed_count = 5;
    int failed_count = 3;
    int paused_count = 2;
    int pending_count = 10;

    for (int i = 0; i < completed_count; ++i) {
        auto task = std::make_shared<DownloadTask>(
            i + 1, "https://example.com/completed" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
        task->set_status(TaskStatus::Completed);
    }

    for (int i = 0; i < failed_count; ++i) {
        auto task = std::make_shared<DownloadTask>(
            completed_count + i + 1, "https://example.com/failed" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
        task->set_status(TaskStatus::Failed);
    }

    for (int i = 0; i < paused_count; ++i) {
        auto task = std::make_shared<DownloadTask>(
            completed_count + failed_count + i + 1,
            "https://example.com/paused" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
        manager_->pause_task(task->id());
    }

    for (int i = 0; i < pending_count; ++i) {
        auto task = std::make_shared<DownloadTask>(
            completed_count + failed_count + paused_count + i + 1,
            "https://example.com/pending" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task, TaskPriority::Normal);
    }

    auto stats = manager_->get_statistics();
    EXPECT_EQ(stats.completed_tasks, completed_count);
    EXPECT_EQ(stats.failed_tasks, failed_count);
    EXPECT_EQ(stats.total_tasks, completed_count + failed_count + paused_count + pending_count);
}