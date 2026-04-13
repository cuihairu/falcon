#pragma once

#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace falcon {
namespace internal {

/// Task manager for handling download task lifecycle
class TaskManager {
public:
    explicit TaskManager(std::size_t max_concurrent = 5);
    ~TaskManager();

    // Non-copyable
    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;

    /// Add a new task to the queue
    void add_task(DownloadTask::Ptr task);

    /// Remove a task (only if finished)
    bool remove_task(TaskId id);

    /// Get task by ID
    [[nodiscard]] DownloadTask::Ptr get_task(TaskId id) const;

    /// Get all tasks
    [[nodiscard]] std::vector<DownloadTask::Ptr> get_all_tasks() const;

    /// Get tasks by status
    [[nodiscard]] std::vector<DownloadTask::Ptr> get_tasks_by_status(
        TaskStatus status) const;

    /// Get active tasks
    [[nodiscard]] std::vector<DownloadTask::Ptr> get_active_tasks() const;

    /// Get number of active tasks
    [[nodiscard]] std::size_t active_count() const;

    /// Get total number of tasks
    [[nodiscard]] std::size_t total_count() const;

    /// Mark task as ready to start
    void schedule_task(TaskId id);

    /// Get next pending task to start
    [[nodiscard]] DownloadTask::Ptr get_next_pending() const;

    /// Set maximum concurrent tasks
    void set_max_concurrent(std::size_t max);

    /// Get maximum concurrent tasks
    [[nodiscard]] std::size_t max_concurrent() const;

    /// Check if can start more tasks
    [[nodiscard]] bool can_start_more() const;

    /// Remove all finished tasks
    std::size_t remove_finished();

    /// Generate next task ID
    [[nodiscard]] TaskId next_id();

    /// Wait for all tasks to complete
    void wait_all();

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;

    std::unordered_map<TaskId, DownloadTask::Ptr> tasks_;
    std::deque<TaskId> pending_queue_;

    std::atomic<TaskId> next_id_{1};
    std::atomic<std::size_t> max_concurrent_;
    std::atomic<std::size_t> active_count_{0};
};

}  // namespace internal
}  // namespace falcon
