#pragma once

#include <falcon/types.hpp>

#include <string>

namespace falcon {

/// Task status enumeration
enum class TaskStatus {
    Pending,      ///< Waiting in queue
    Preparing,    ///< Getting file info
    Downloading,  ///< Actively downloading
    Paused,       ///< Paused by user
    Completed,    ///< Successfully completed
    Failed,       ///< Failed with error
    Cancelled     ///< Cancelled by user
};

/// Convert TaskStatus to string
inline const char* to_string(TaskStatus status) {
    switch (status) {
        case TaskStatus::Pending:
            return "Pending";
        case TaskStatus::Preparing:
            return "Preparing";
        case TaskStatus::Downloading:
            return "Downloading";
        case TaskStatus::Paused:
            return "Paused";
        case TaskStatus::Completed:
            return "Completed";
        case TaskStatus::Failed:
            return "Failed";
        case TaskStatus::Cancelled:
            return "Cancelled";
        default:
            return "Unknown";
    }
}

/// Event listener interface for download callbacks
class IEventListener {
public:
    virtual ~IEventListener() = default;

    /// Called when task status changes
    /// @param task_id The task identifier
    /// @param old_status Previous status
    /// @param new_status New status
    virtual void on_status_changed(TaskId task_id, TaskStatus old_status,
                                   TaskStatus new_status) {
        (void)task_id;
        (void)old_status;
        (void)new_status;
    }

    /// Called periodically with progress update
    /// @param info Progress information
    virtual void on_progress(const ProgressInfo& info) { (void)info; }

    /// Called when an error occurs
    /// @param task_id The task identifier
    /// @param error_message Error description
    virtual void on_error(TaskId task_id, const std::string& error_message) {
        (void)task_id;
        (void)error_message;
    }

    /// Called when task completes successfully
    /// @param task_id The task identifier
    /// @param output_path Path to downloaded file
    virtual void on_completed(TaskId task_id, const std::string& output_path) {
        (void)task_id;
        (void)output_path;
    }

    /// Called when file info is retrieved
    /// @param task_id The task identifier
    /// @param info File information
    virtual void on_file_info(TaskId task_id, const FileInfo& info) {
        (void)task_id;
        (void)info;
    }
};

}  // namespace falcon
