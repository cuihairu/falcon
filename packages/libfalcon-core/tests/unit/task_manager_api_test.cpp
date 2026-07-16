// Falcon Task Manager API Tests
// Cover untested TaskManager API paths.
// Follows the pattern from task_manager_test.cpp.

#include <falcon/download_task.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_handler.hpp>
#include <falcon/task_manager.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace falcon;

namespace {

// Helper to create a task quickly
DownloadTask::Ptr make_task(TaskId id, const std::string& url) {
    return std::make_shared<DownloadTask>(id, url, DownloadOptions{});
}

// Helper to create a task with a specific handler
DownloadTask::Ptr make_task_with_handler(TaskId id, const std::string& url,
                                          std::shared_ptr<IProtocolHandler> handler) {
    auto task = std::make_shared<DownloadTask>(id, url, DownloadOptions{});
    task->set_handler(std::move(handler));
    return task;
}

// Handler that blocks until released, recording execution order
class OrderedBlockingHandler final : public IProtocolHandler {
public:
    std::string protocol_name() const override { return "ordered_blocking"; }

    std::vector<std::string> supported_schemes() const override {
        return {"https"};
    }

    bool can_handle(const std::string& url) const override {
        return url.rfind("https://", 0) == 0;
    }

    FileInfo get_file_info(const std::string& url, const DownloadOptions& /*options*/) override {
        FileInfo info;
        info.url = url;
        info.filename = "test.bin";
        info.total_size = 1;
        info.supports_resume = false;
        return info;
    }

    void download(DownloadTask::Ptr task, IEventListener* /*listener*/) override {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            execution_order_.push_back(task->id());
        }
        cv_.notify_all();

        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this, task_id = task->id()] {
            return released_ids_.count(task_id) > 0;
        });
        released_ids_.erase(task->id());
        lock.unlock();
        cv_.notify_all();

        task->set_status(TaskStatus::Completed);
    }

    void pause(DownloadTask::Ptr task) override {
        task->set_status(TaskStatus::Paused);
    }

    void resume(DownloadTask::Ptr task, IEventListener* listener) override {
        download(std::move(task), listener);
    }

    void cancel(DownloadTask::Ptr task) override {
        task->set_status(TaskStatus::Cancelled);
    }

    void release(TaskId id) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ids_.insert(id);
        }
        cv_.notify_all();
    }

    std::vector<TaskId> get_execution_order() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return execution_order_;
    }

    bool wait_for_activated_count(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] {
            return execution_order_.size() >= count;
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<TaskId> execution_order_;
    std::unordered_set<TaskId> released_ids_;
};

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
    auto task = manager_->get_task(id);
    ASSERT_NE(task, nullptr);

    // Initial priority should be Normal (default)
    EXPECT_EQ(task->get_priority(), TaskPriority::Normal);

    // Adjust to High
    bool adjusted = manager_->adjust_task_priority(id, TaskPriority::High);
    EXPECT_TRUE(adjusted);
    EXPECT_EQ(task->get_priority(), TaskPriority::High);

    // Adjust to Low
    adjusted = manager_->adjust_task_priority(id, TaskPriority::Low);
    EXPECT_TRUE(adjusted);
    EXPECT_EQ(task->get_priority(), TaskPriority::Low);
}

TEST_F(TaskManagerApiTest, AdjustTaskPriorityMultipleTasks) {
    TaskId id1 = manager_->add_task(make_task(1, "https://a.com/1"));
    TaskId id2 = manager_->add_task(make_task(2, "https://b.com/2"));
    TaskId id3 = manager_->add_task(make_task(3, "https://c.com/3"));

    auto task1 = manager_->get_task(id1);
    auto task2 = manager_->get_task(id2);
    auto task3 = manager_->get_task(id3);

    ASSERT_NE(task1, nullptr);
    ASSERT_NE(task2, nullptr);
    ASSERT_NE(task3, nullptr);

    // Set different priorities
    manager_->adjust_task_priority(id1, TaskPriority::Low);
    manager_->adjust_task_priority(id2, TaskPriority::High);
    manager_->adjust_task_priority(id3, TaskPriority::Critical);

    EXPECT_EQ(task1->get_priority(), TaskPriority::Low);
    EXPECT_EQ(task2->get_priority(), TaskPriority::High);
    EXPECT_EQ(task3->get_priority(), TaskPriority::Critical);
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

// ============================================================================
// Priority: ordering and reorder tests
// ============================================================================

class TaskManagerPriorityTest : public ::testing::Test {
protected:
    void SetUp() override {
        handler_ = std::make_shared<OrderedBlockingHandler>();

        event_dispatcher_ = std::make_unique<EventDispatcher>();
        event_dispatcher_->start();

        TaskManagerConfig config;
        config.max_concurrent_tasks = 1; // Execute one at a time to observe order
        manager_ = std::make_unique<TaskManager>(config, event_dispatcher_.get());
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

    std::shared_ptr<OrderedBlockingHandler> handler_;
    std::unique_ptr<EventDispatcher> event_dispatcher_;
    std::unique_ptr<TaskManager> manager_;
    DownloadOptions default_options_;
};

TEST_F(TaskManagerPriorityTest, PriorityDequeueOrder) {
    // Add tasks with different priorities
    auto low_task    = make_task_with_handler(1, "https://a.com/low",    handler_);
    auto normal_task = make_task_with_handler(2, "https://b.com/normal", handler_);
    auto high_task   = make_task_with_handler(3, "https://c.com/high",   handler_);
    auto critical_task = make_task_with_handler(4, "https://d.com/critical", handler_);

    manager_->add_task(low_task,    TaskPriority::Low);
    manager_->add_task(normal_task, TaskPriority::Normal);
    manager_->add_task(high_task,   TaskPriority::High);
    manager_->add_task(critical_task, TaskPriority::Critical);

    // Start all tasks — they should queue
    manager_->start_task(1);
    manager_->start_task(2);
    manager_->start_task(3);
    manager_->start_task(4);

    ASSERT_TRUE(handler_->wait_for_activated_count(1, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), std::vector<TaskId>({4u}));

    handler_->release(4);
    ASSERT_TRUE(handler_->wait_for_activated_count(2, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), (std::vector<TaskId>{4u, 3u}));

    handler_->release(3);
    ASSERT_TRUE(handler_->wait_for_activated_count(3, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), (std::vector<TaskId>{4u, 3u, 2u}));

    handler_->release(2);
    ASSERT_TRUE(handler_->wait_for_activated_count(4, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), (std::vector<TaskId>{4u, 3u, 2u, 1u}));

    handler_->release(1);
    EXPECT_TRUE(manager_->wait_all(std::chrono::milliseconds(1000)));
}

TEST_F(TaskManagerPriorityTest, AdjustPriorityReorder) {
    auto first_task = make_task_with_handler(1, "https://a.com/first", handler_);
    auto second_task = make_task_with_handler(2, "https://b.com/second", handler_);
    auto promoted_task = make_task_with_handler(3, "https://c.com/promoted", handler_);

    manager_->add_task(first_task, TaskPriority::Critical);
    manager_->add_task(second_task, TaskPriority::Normal);
    manager_->add_task(promoted_task, TaskPriority::Low);

    ASSERT_TRUE(manager_->start_task(1));
    ASSERT_TRUE(manager_->start_task(2));
    ASSERT_TRUE(manager_->start_task(3));
    ASSERT_TRUE(manager_->adjust_task_priority(3, TaskPriority::High));

    ASSERT_TRUE(handler_->wait_for_activated_count(1, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), std::vector<TaskId>({1u}));

    handler_->release(1);
    ASSERT_TRUE(handler_->wait_for_activated_count(2, std::chrono::milliseconds(1000)));

    auto order = handler_->get_execution_order();
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[1], 3u) << "Adjusted high-priority task should run before older normal task";

    handler_->release(3);
    ASSERT_TRUE(handler_->wait_for_activated_count(3, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), (std::vector<TaskId>{1u, 3u, 2u}));

    handler_->release(2);
    EXPECT_TRUE(manager_->wait_all(std::chrono::milliseconds(1000)));
}

TEST_F(TaskManagerPriorityTest, ResumePreservesPriority) {
    auto task = make_task(1, "https://a.com/high");
    manager_->add_task(task, TaskPriority::High);

    ASSERT_TRUE(manager_->pause_task(1));
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    ASSERT_TRUE(manager_->resume_task(1));
    EXPECT_EQ(task->get_priority(), TaskPriority::High);
    EXPECT_EQ(manager_->get_queue_size(), 1u);
}

TEST_F(TaskManagerPriorityTest, AddTaskSetsPriority) {
    auto task = make_task(1, "https://a.com/test");
    manager_->add_task(task, TaskPriority::Critical);
    EXPECT_EQ(task->get_priority(), TaskPriority::Critical);

    auto task2 = make_task(2, "https://b.com/test");
    manager_->add_task(task2);  // default should be Normal
    EXPECT_EQ(task2->get_priority(), TaskPriority::Normal);
}

TEST_F(TaskManagerPriorityTest, DuplicateStartDoesNotRequeueTask) {
    auto task = make_task(1, "https://a.com/test");
    manager_->add_task(task, TaskPriority::High);

    ASSERT_TRUE(manager_->start_task(1));
    EXPECT_EQ(manager_->get_queue_size(), 1u);

    EXPECT_FALSE(manager_->start_task(1));
    EXPECT_EQ(manager_->get_queue_size(), 1u);
}

TEST_F(TaskManagerPriorityTest, AdjustNonExistentTask) {
    bool adjusted = manager_->adjust_task_priority(99999, TaskPriority::High);
    EXPECT_FALSE(adjusted);
}

TEST_F(TaskManagerPriorityTest, StartTaskUsesTaskPriority) {
    auto task = make_task_with_handler(1, "https://a.com/high", handler_);
    task->set_priority(TaskPriority::High);
    manager_->add_task(task, TaskPriority::High);

    // start_task(id) without priority param should use task->get_priority()
    bool started = manager_->start_task(1);
    EXPECT_TRUE(started);

    ASSERT_TRUE(handler_->wait_for_activated_count(1, std::chrono::milliseconds(1000)));
    EXPECT_EQ(handler_->get_execution_order(), std::vector<TaskId>({1u}));
    handler_->release(1);
    EXPECT_TRUE(manager_->wait_all(std::chrono::milliseconds(1000)));
}
