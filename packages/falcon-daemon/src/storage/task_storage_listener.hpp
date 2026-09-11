/**
 * @file task_storage_listener.hpp
 * @brief Download engine event listener that persists task state to TaskStorage
 * @author Falcon Team
 * @date 2026-09-11
 */

#pragma once

#include <falcon/event_listener.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace falcon::daemon {

class TaskStorage;

/**
 * @brief Persists engine task events (status changes and progress) into TaskStorage
 *
 * Register an instance on the DownloadEngine via add_listener(). While attached:
 * - Status changes are written immediately (Completed/Failed go through
 *   mark_completed()/mark_failed() so terminal timestamps are recorded).
 * - Progress updates are throttled per task to avoid hammering SQLite with
 *   the high-frequency progress events.
 * - Error messages reported via on_error() are cached per task and consumed
 *   as the error description when the task enters Failed.
 *
 * Thread-safe: engine callbacks may arrive from the event dispatcher thread;
 * all methods serialize internally. On daemon shutdown call shutdown() BEFORE
 * destroying the TaskStorage - after shutdown() returns no callback will
 * touch the storage anymore, so the destruction order is safe even if the
 * dispatcher still holds queued events.
 */
class TaskStorageListener final : public falcon::IEventListener {
public:
    /**
     * @brief Construct a listener writing to the given storage
     * @param storage Initialized task storage; not owned
     * @param progress_flush_interval Minimum interval between two progress
     *        writes for the same task
     */
    explicit TaskStorageListener(
        TaskStorage* storage,
        std::chrono::milliseconds progress_flush_interval = std::chrono::milliseconds(1000));

    TaskStorageListener(const TaskStorageListener&) = delete;
    TaskStorageListener& operator=(const TaskStorageListener&) = delete;

    // === IEventListener ===

    void on_status_changed(TaskId task_id, TaskStatus old_status,
                           TaskStatus new_status) override;
    void on_progress(const ProgressInfo& info) override;
    void on_error(TaskId task_id, const std::string& error_message) override;

    // === Shutdown ===

    /**
     * @brief Stop persisting and drop the storage reference
     *
     * After this call every engine callback is a no-op. Call it before the
     * TaskStorage is destroyed to guarantee no in-flight dispatcher event
     * touches a dangling pointer.
     */
    void shutdown();

private:
    TaskStorage* storage_;  ///< Target storage; guarded by mutex_
    std::chrono::milliseconds progress_flush_interval_;

    mutable std::mutex mutex_;
    bool stopped_ = false;  ///< Set by shutdown(); guarded by mutex_
    /// Last progress flush time per task; guarded by mutex_
    std::map<TaskId, std::chrono::steady_clock::time_point> last_flush_;
    /// Cached error message per task; guarded by mutex_
    std::map<TaskId, std::string> last_error_;
};

}  // namespace falcon::daemon
