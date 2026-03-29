/**
 * @file task_storage.hpp
 * @brief SQLite-based task persistence for Falcon daemon
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>

#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <optional>
#include <functional>

// Forward declaration to avoid SQLite header dependency in public API
struct sqlite3;

namespace falcon::daemon {

/**
 * @brief Persisted task record
 */
struct TaskRecord {
    TaskId id;                                  ///< Task ID
    std::string url;                            ///< Download URL
    std::string output_path;                    ///< Output file path
    TaskStatus status;                          ///< Task status
    double progress;                            ///< Progress (0.0 - 1.0)
    Bytes total_bytes;                          ///< Total file size
    Bytes downloaded_bytes;                     ///< Downloaded bytes
    BytesPerSecond speed;                       ///< Current download speed
    std::string error_message;                  ///< Error message if failed
    DownloadOptions options;                    ///< Download options
    std::chrono::system_clock::time_point created_at;   ///< Creation time
    std::chrono::system_clock::time_point updated_at;   ///< Last update time
    std::optional<std::chrono::system_clock::time_point> completed_at; ///< Completion time
};

/**
 * @brief Task storage configuration
 */
struct TaskStorageConfig {
    std::string db_path = ":memory:";           ///< Database file path (default: in-memory)
    bool auto_create_tables = true;             ///< Automatically create tables on init
    bool enable_wal_mode = true;                ///< Enable Write-Ahead Logging for better performance
    int busy_timeout_ms = 5000;                 ///< SQLite busy timeout
};

/**
 * @brief Query options for listing tasks
 */
struct TaskQueryOptions {
    std::optional<TaskStatus> status_filter;    ///< Filter by status
    int limit = 0;                              ///< Max results (0 = unlimited)
    int offset = 0;                             ///< Offset for pagination
    bool order_by_created_desc = true;          ///< Order by creation time descending
};

/**
 * @brief SQLite-based task persistence layer
 *
 * Provides CRUD operations for download tasks with automatic
 * serialization of download options to JSON.
 *
 * Thread-safe: All public methods are safe to call from multiple threads.
 */
class TaskStorage {
public:
    /**
     * @brief Construct a new Task Storage object
     * @param config Storage configuration
     */
    explicit TaskStorage(const TaskStorageConfig& config = {});

    /**
     * @brief Destructor - closes database connection
     */
    ~TaskStorage();

    // Non-copyable
    TaskStorage(const TaskStorage&) = delete;
    TaskStorage& operator=(const TaskStorage&) = delete;

    // Movable
    TaskStorage(TaskStorage&&) noexcept;
    TaskStorage& operator=(TaskStorage&&) noexcept;

    /**
     * @brief Initialize the storage (create tables if needed)
     * @return true on success, false on failure
     */
    bool initialize();

    /**
     * @brief Check if storage is initialized and ready
     * @return true if ready
     */
    bool is_ready() const;

    // === CRUD Operations ===

    /**
     * @brief Create a new task record
     * @param record Task record (id will be assigned)
     * @return Assigned task ID, or INVALID_TASK_ID on failure
     */
    TaskId create_task(const TaskRecord& record);

    /**
     * @brief Get a task by ID
     * @param id Task ID
     * @return Task record, or nullopt if not found
     */
    std::optional<TaskRecord> get_task(TaskId id) const;

    /**
     * @brief Update an existing task
     * @param id Task ID
     * @param record Updated record
     * @return true on success
     */
    bool update_task(TaskId id, const TaskRecord& record);

    /**
     * @brief Delete a task
     * @param id Task ID
     * @return true on success
     */
    bool delete_task(TaskId id);

    /**
     * @brief Check if a task exists
     * @param id Task ID
     * @return true if exists
     */
    bool task_exists(TaskId id) const;

    // === Query Operations ===

    /**
     * @brief List tasks with optional filtering
     * @param options Query options
     * @return List of task records
     */
    std::vector<TaskRecord> list_tasks(const TaskQueryOptions& options = {}) const;

    /**
     * @brief Get tasks by status
     * @param status Task status
     * @return List of matching tasks
     */
    std::vector<TaskRecord> get_tasks_by_status(TaskStatus status) const;

    /**
     * @brief Get all active (downloading/preparing) tasks
     * @return List of active tasks
     */
    std::vector<TaskRecord> get_active_tasks() const;

    /**
     * @brief Get count of tasks by status
     * @param status Task status
     * @return Count
     */
    int count_tasks_by_status(TaskStatus status) const;

    /**
     * @brief Get total task count
     * @return Total count
     */
    int count_all_tasks() const;

    // === Batch Operations ===

    /**
     * @brief Update task progress (partial update)
     * @param id Task ID
     * @param downloaded_bytes Downloaded bytes
     * @param progress Progress percentage
     * @param speed Current speed
     * @return true on success
     */
    bool update_progress(TaskId id, Bytes downloaded_bytes, double progress, BytesPerSecond speed);

    /**
     * @brief Update task status
     * @param id Task ID
     * @param status New status
     * @param error_message Optional error message
     * @return true on success
     */
    bool update_status(TaskId id, TaskStatus status, const std::string& error_message = "");

    /**
     * @brief Mark task as completed
     * @param id Task ID
     * @return true on success
     */
    bool mark_completed(TaskId id);

    /**
     * @brief Mark task as failed
     * @param id Task ID
     * @param error_message Error description
     * @return true on success
     */
    bool mark_failed(TaskId id, const std::string& error_message);

    // === Maintenance Operations ===

    /**
     * @brief Delete all completed tasks older than specified days
     * @param retention_days Days to retain
     * @return Number of deleted tasks
     */
    int cleanup_completed_tasks(int retention_days = 7);

    /**
     * @brief Delete all tasks (use with caution)
     * @return true on success
     */
    bool delete_all_tasks();

    /**
     * @brief Vacuum the database to reclaim space
     * @return true on success
     */
    bool vacuum();

    /**
     * @brief Get the database file path
     * @return Database path
     */
    std::string get_db_path() const;

    /**
     * @brief Get last error message
     * @return Error message
     */
    std::string get_last_error() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace falcon::daemon
