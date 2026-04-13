// Cover untested TaskManager API paths.
// Follows the pattern from task_manager_test.cpp.

#include <falcon/download_task.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/task_manager.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace falcon;

namespace {

// Helper to create a task quickly
DownloadTask::Ptr make_task(TaskId id, const std::string& url) {
    return std::make_shared<DownloadTask>(id, url, DownloadOptions{});
}

} // namespace

// ---------------------------------------------------------------------------
// Fixture with EventDispatcher + TaskManager
// ---------------------------------------------------------------------------

class TaskManagerApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        event_dispatcher_ = std::make_unique<EventDispatcher>();
        event_dispatcher_->start();
        manager_ = std::make_unique<TaskManager>(TaskManagerConfig{}, event_dispatcher_.get());
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
};

// ---------------------------------------------------------------------------
// Task creation and retrieval
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, CreateTaskAndGetById) {
    auto task = make_task(1, "https://example.com/file.zip");
    TaskId id = manager_->add_task(task);

    EXPECT_NE(id, INVALID_TASK_ID);

    auto retrieved = manager_->get_task(id);
    ASSERT_NE(retrieved, nullptr);
    EXPECT_EQ(retrieved->id(), id);
}

TEST_F(TaskManagerApiTest, GetTaskByInvalidId) {
    EXPECT_EQ(manager_->get_task(INVALID_TASK_ID), nullptr);
    EXPECT_EQ(manager_->get_task(99999), nullptr);
}

// ---------------------------------------------------------------------------
// get_all_tasks / get_tasks_by_status / get_active_tasks
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, GetAllTasks) {
    manager_->add_task(make_task(1, "https://a.com/1"));
    manager_->add_task(make_task(2, "https://b.com/2"));
    manager_->add_task(make_task(3, "https://c.com/3"));

    auto all = manager_->get_all_tasks();
    EXPECT_EQ(all.size(), 3u);
}

TEST_F(TaskManagerApiTest, GetTasksByStatus) {
    auto t1 = make_task(1, "https://a.com/1");
    auto t2 = make_task(2, "https://b.com/2");
    manager_->add_task(t1);
    manager_->add_task(t2);

    auto pending = manager_->get_tasks_by_status(TaskStatus::Pending);
    EXPECT_EQ(pending.size(), 2u);

    t1->set_status(TaskStatus::Cancelled);
    auto cancelled = manager_->get_tasks_by_status(TaskStatus::Cancelled);
    EXPECT_EQ(cancelled.size(), 1u);
}

TEST_F(TaskManagerApiTest, GetActiveTasksEmpty) {
    EXPECT_TRUE(manager_->get_active_tasks().empty());
}

// ---------------------------------------------------------------------------
// remove_task / cleanup_finished_tasks
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, RemoveFinishedTask) {
    TaskId id = manager_->add_task(make_task(1, "https://a.com/1"));
    auto task = manager_->get_task(id);
    ASSERT_NE(task, nullptr);

    // Task must be in a finished state to be removed
    task->mark_started();
    task->set_status(TaskStatus::Completed);

    bool removed = manager_->remove_task(id);
    EXPECT_TRUE(removed);
    EXPECT_EQ(manager_->get_task(id), nullptr);
}

TEST_F(TaskManagerApiTest, RemoveNonExistentTask) {
    EXPECT_FALSE(manager_->remove_task(99999));
}

TEST_F(TaskManagerApiTest, CleanupFinishedTasks) {
    auto t1 = make_task(1, "https://a.com/1");
    auto t2 = make_task(2, "https://b.com/2");
    manager_->add_task(t1);
    manager_->add_task(t2);

    t1->mark_started();
    t1->set_status(TaskStatus::Completed);

    size_t cleaned = manager_->cleanup_finished_tasks();
    EXPECT_EQ(cleaned, 1u);

    auto remaining = manager_->get_all_tasks();
    EXPECT_EQ(remaining.size(), 1u);
}

// ---------------------------------------------------------------------------
// Task control: pause / resume / cancel
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, CancelTask) {
    TaskId id = manager_->add_task(make_task(1, "https://a.com/1"));
    bool cancelled = manager_->cancel_task(id);
    EXPECT_TRUE(cancelled);
}

TEST_F(TaskManagerApiTest, CancelNonExistentTask) {
    EXPECT_FALSE(manager_->cancel_task(99999));
}

TEST_F(TaskManagerApiTest, PauseNonExistentTask) {
    EXPECT_FALSE(manager_->pause_task(99999));
}

TEST_F(TaskManagerApiTest, ResumeNonExistentTask) {
    EXPECT_FALSE(manager_->resume_task(99999));
}

// ---------------------------------------------------------------------------
// Bulk operations: pause_all / resume_all / cancel_all
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, CancelAllTasks) {
    manager_->add_task(make_task(1, "https://a.com/1"));
    manager_->add_task(make_task(2, "https://b.com/2"));
    manager_->cancel_all();
    // Should not crash
}

// ---------------------------------------------------------------------------
// Priority: adjust_task_priority
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, AdjustTaskPriorityNonExistent) {
    bool adjusted = manager_->adjust_task_priority(99999, TaskPriority::High);
    EXPECT_FALSE(adjusted);
}

TEST_F(TaskManagerApiTest, AdjustTaskPriorityExistingTask) {
    TaskId id = manager_->add_task(make_task(1, "https://a.com/1"));
    // adjust_task_priority may require the task to be in the queue
    // Just verify the API doesn't crash
    manager_->adjust_task_priority(id, TaskPriority::High);
}

// ---------------------------------------------------------------------------
// Stress test: many tasks
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, ManyTasksStressTest) {
    constexpr int count = 200;
    std::vector<TaskId> ids;

    for (int i = 0; i < count; ++i) {
        TaskId id = manager_->add_task(
            make_task(i + 1, "https://host/" + std::to_string(i)));
        ids.push_back(id);
    }

    EXPECT_EQ(manager_->get_all_tasks().size(), static_cast<size_t>(count));

    // Cleanup finished tasks (none finished yet)
    size_t cleaned = manager_->cleanup_finished_tasks();
    EXPECT_EQ(cleaned, 0u);
}

// ---------------------------------------------------------------------------
// wait_all
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, WaitAllWithNoTasks) {
    bool ok = manager_->wait_all(std::chrono::milliseconds(50));
    EXPECT_TRUE(ok);
}

// ---------------------------------------------------------------------------
// Queue size and statistics
// ---------------------------------------------------------------------------

TEST_F(TaskManagerApiTest, QueueSizeStartsAtZero) {
    EXPECT_EQ(manager_->get_queue_size(), 0u);
}

TEST_F(TaskManagerApiTest, ActiveCountStartsAtZero) {
    EXPECT_EQ(manager_->get_active_task_count(), 0u);
}

TEST_F(TaskManagerApiTest, QueueAndActiveCounts) {
    EXPECT_EQ(manager_->get_active_task_count(), 0u);
    EXPECT_EQ(manager_->get_all_tasks().size(), 0u);
    manager_->add_task(make_task(1, "https://a.com/1"));
    EXPECT_EQ(manager_->get_all_tasks().size(), 1u);
}
