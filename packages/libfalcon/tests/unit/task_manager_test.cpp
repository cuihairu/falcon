// Falcon Task Manager Unit Tests
// Copyright (c) 2025 Falcon Project

#include "internal/task_manager.hpp"

#include <falcon/download_task.hpp>

#include <gtest/gtest.h>

#include <thread>
#include <vector>

using namespace falcon;
using namespace falcon::internal;

class TaskManagerTest : public ::testing::Test {
protected:
    void SetUp() override { manager_ = std::make_unique<TaskManager>(5); }

    std::unique_ptr<TaskManager> manager_;
    DownloadOptions default_options_;
};

TEST_F(TaskManagerTest, CreateManager) {
    EXPECT_EQ(manager_->max_concurrent(), 5);
    EXPECT_EQ(manager_->total_count(), 0);
    EXPECT_EQ(manager_->active_count(), 0);
}

TEST_F(TaskManagerTest, AddTask) {
    TaskId id = manager_->next_id();
    auto task = std::make_shared<DownloadTask>(id, "https://example.com/file.zip",
                                                default_options_);
    manager_->add_task(task);

    EXPECT_EQ(manager_->total_count(), 1);
    EXPECT_NE(manager_->get_task(id), nullptr);
}

TEST_F(TaskManagerTest, AddMultipleTasks) {
    for (int i = 0; i < 10; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task);
    }

    EXPECT_EQ(manager_->total_count(), 10);
}

TEST_F(TaskManagerTest, GetNonExistentTask) {
    auto task = manager_->get_task(999);
    EXPECT_EQ(task, nullptr);
}

TEST_F(TaskManagerTest, RemoveFinishedTask) {
    TaskId id = manager_->next_id();
    auto task = std::make_shared<DownloadTask>(id, "https://example.com/file.zip",
                                                default_options_);
    manager_->add_task(task);

    // Cannot remove non-finished task
    EXPECT_FALSE(manager_->remove_task(id));
    EXPECT_EQ(manager_->total_count(), 1);

    // Mark as completed
    task->set_status(TaskStatus::Completed);

    // Now can remove
    EXPECT_TRUE(manager_->remove_task(id));
    EXPECT_EQ(manager_->total_count(), 0);
}

TEST_F(TaskManagerTest, GetAllTasks) {
    for (int i = 0; i < 5; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task);
    }

    auto tasks = manager_->get_all_tasks();
    EXPECT_EQ(tasks.size(), 5);
}

TEST_F(TaskManagerTest, GetTasksByStatus) {
    // Add tasks with different statuses
    for (int i = 0; i < 3; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/pending" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task);
    }

    for (int i = 0; i < 2; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/downloading" + std::to_string(i) + ".zip",
            default_options_);
        task->set_status(TaskStatus::Downloading);
        manager_->add_task(task);
    }

    auto pending = manager_->get_tasks_by_status(TaskStatus::Pending);
    EXPECT_EQ(pending.size(), 3);

    auto downloading = manager_->get_tasks_by_status(TaskStatus::Downloading);
    EXPECT_EQ(downloading.size(), 2);

    auto completed = manager_->get_tasks_by_status(TaskStatus::Completed);
    EXPECT_EQ(completed.size(), 0);
}

TEST_F(TaskManagerTest, GetActiveTasks) {
    // Add pending task
    {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(id, "https://example.com/pending.zip",
                                                    default_options_);
        manager_->add_task(task);
    }

    // Add downloading tasks
    for (int i = 0; i < 2; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/downloading" + std::to_string(i) + ".zip",
            default_options_);
        task->set_status(TaskStatus::Downloading);
        manager_->add_task(task);
    }

    // Add preparing task
    {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(id, "https://example.com/preparing.zip",
                                                    default_options_);
        task->set_status(TaskStatus::Preparing);
        manager_->add_task(task);
    }

    auto active = manager_->get_active_tasks();
    EXPECT_EQ(active.size(), 3);  // 2 downloading + 1 preparing
}

TEST_F(TaskManagerTest, GetNextPending) {
    // Add tasks in order
    TaskId first_id = 0;
    for (int i = 0; i < 3; ++i) {
        TaskId id = manager_->next_id();
        if (i == 0) first_id = id;
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/file" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task);
    }

    auto next = manager_->get_next_pending();
    ASSERT_NE(next, nullptr);
    EXPECT_EQ(next->id(), first_id);
}

TEST_F(TaskManagerTest, SetMaxConcurrent) {
    EXPECT_EQ(manager_->max_concurrent(), 5);

    manager_->set_max_concurrent(10);
    EXPECT_EQ(manager_->max_concurrent(), 10);

    manager_->set_max_concurrent(1);
    EXPECT_EQ(manager_->max_concurrent(), 1);
}

TEST_F(TaskManagerTest, CanStartMore) {
    EXPECT_TRUE(manager_->can_start_more());

    manager_->set_max_concurrent(0);
    EXPECT_FALSE(manager_->can_start_more());
}

TEST_F(TaskManagerTest, RemoveFinished) {
    // Add mix of tasks
    for (int i = 0; i < 3; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/pending" + std::to_string(i) + ".zip",
            default_options_);
        manager_->add_task(task);
    }

    for (int i = 0; i < 2; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/completed" + std::to_string(i) + ".zip",
            default_options_);
        task->set_status(TaskStatus::Completed);
        manager_->add_task(task);
    }

    for (int i = 0; i < 2; ++i) {
        TaskId id = manager_->next_id();
        auto task = std::make_shared<DownloadTask>(
            id, "https://example.com/failed" + std::to_string(i) + ".zip",
            default_options_);
        task->set_status(TaskStatus::Failed);
        manager_->add_task(task);
    }

    EXPECT_EQ(manager_->total_count(), 7);

    std::size_t removed = manager_->remove_finished();
    EXPECT_EQ(removed, 4);  // 2 completed + 2 failed
    EXPECT_EQ(manager_->total_count(), 3);  // 3 pending remain
}

TEST_F(TaskManagerTest, NextIdIsUnique) {
    std::vector<TaskId> ids;
    for (int i = 0; i < 100; ++i) {
        ids.push_back(manager_->next_id());
    }

    // All IDs should be unique
    std::sort(ids.begin(), ids.end());
    auto last = std::unique(ids.begin(), ids.end());
    EXPECT_EQ(last, ids.end());
}

TEST_F(TaskManagerTest, ThreadSafeAddTask) {
    const int num_threads = 10;
    const int tasks_per_thread = 100;

    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, t, tasks_per_thread]() {
            for (int i = 0; i < tasks_per_thread; ++i) {
                TaskId id = manager_->next_id();
                auto task = std::make_shared<DownloadTask>(
                    id, "https://example.com/thread" + std::to_string(t) +
                            "_file" + std::to_string(i) + ".zip",
                    default_options_);
                manager_->add_task(task);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(manager_->total_count(),
              static_cast<std::size_t>(num_threads * tasks_per_thread));
}
