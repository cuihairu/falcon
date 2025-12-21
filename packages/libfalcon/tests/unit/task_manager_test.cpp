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