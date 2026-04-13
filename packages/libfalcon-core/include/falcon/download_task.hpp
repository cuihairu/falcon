#pragma once

#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/types.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace falcon {

// Forward declarations
class IProtocolHandler;

/// Download task class representing a single download operation
class DownloadTask : public std::enable_shared_from_this<DownloadTask> {
public:
    using Ptr = std::shared_ptr<DownloadTask>;
    using WeakPtr = std::weak_ptr<DownloadTask>;

    /// Create a new download task
    /// @param id Unique task identifier
    /// @param url Download URL
    /// @param options Download options
    DownloadTask(TaskId id, std::string url, DownloadOptions options);

    /// Destructor
    ~DownloadTask();

    // Non-copyable
    DownloadTask(const DownloadTask&) = delete;
    DownloadTask& operator=(const DownloadTask&) = delete;

    // Non-movable (due to atomic members)
    DownloadTask(DownloadTask&&) = delete;
    DownloadTask& operator=(DownloadTask&&) = delete;

    // === Status Query ===

    /// Get task ID
    [[nodiscard]] TaskId id() const noexcept { return id_; }

    /// Get download URL
    [[nodiscard]] const std::string& url() const noexcept { return url_; }

    /// Get current status
    [[nodiscard]] TaskStatus status() const noexcept { return status_.load(); }

    /// Get download progress (0.0 ~ 1.0)
    [[nodiscard]] float progress() const noexcept;

    /// Get total file size in bytes (0 if unknown)
    [[nodiscard]] Bytes total_bytes() const noexcept {
        return total_bytes_.load();
    }

    /// Get downloaded bytes
    [[nodiscard]] Bytes downloaded_bytes() const noexcept {
        return downloaded_bytes_.load();
    }

    /// Get current download speed in bytes/second
    [[nodiscard]] BytesPerSecond speed() const noexcept {
        return current_speed_.load();
    }

    /// Get output file path
    [[nodiscard]] const std::string& output_path() const noexcept {
        return output_path_;
    }

    /// Get download options
    [[nodiscard]] const DownloadOptions& options() const noexcept {
        return options_;
    }

    /// Get file info (available after preparation)
    [[nodiscard]] const FileInfo& file_info() const noexcept {
        return file_info_;
    }

    /// Get error message (if failed)
    [[nodiscard]] const std::string& error_message() const noexcept {
        return error_message_;
    }

    /// Get start time
    [[nodiscard]] TimePoint start_time() const noexcept { return start_time_; }

    /// Get elapsed time
    [[nodiscard]] Duration elapsed() const noexcept;

    /// Get estimated remaining time
    [[nodiscard]] Duration estimated_remaining() const noexcept;

    /// Check if task is active (downloading or preparing)
    [[nodiscard]] bool is_active() const noexcept;

    /// Check if task is finished (completed, failed, or cancelled)
    [[nodiscard]] bool is_finished() const noexcept;

    // === Control Operations ===

    /// Pause the download
    /// @return true if paused successfully
    bool pause();

    /// Resume a paused download
    /// @return true if resumed successfully
    bool resume();

    /// Cancel the download
    /// @return true if cancelled successfully
    bool cancel();

    /// Wait for task to complete
    void wait();

    /// Wait for task with timeout
    /// @param timeout Maximum time to wait
    /// @return true if task completed, false if timeout
    bool wait_for(Duration timeout);

    // === Internal Methods (used by engine) ===

    /// Set the protocol handler
    void set_handler(std::shared_ptr<IProtocolHandler> handler);

    /// Get the protocol handler
    std::shared_ptr<IProtocolHandler> get_handler() const;

    /// Set event listener
    void set_listener(IEventListener* listener);

    /// Update task status (internal)
    void set_status(TaskStatus new_status);

    /// Update progress (internal)
    void update_progress(Bytes downloaded, Bytes total, BytesPerSecond speed);

    /// Set file info (internal)
    void set_file_info(const FileInfo& info);

    /// Set output path (internal)
    void set_output_path(const std::string& path);

    /// Set error message (internal)
    void set_error(const std::string& message);

    /// Mark as started (internal)
    void mark_started();

    /// Get progress info structure
    [[nodiscard]] ProgressInfo get_progress_info() const;

private:
    TaskId id_;
    std::string url_;
    DownloadOptions options_;

    std::atomic<TaskStatus> status_{TaskStatus::Pending};
    std::atomic<Bytes> total_bytes_{0};
    std::atomic<Bytes> downloaded_bytes_{0};
    std::atomic<BytesPerSecond> current_speed_{0};

    std::string output_path_;
    std::string error_message_;
    FileInfo file_info_;

    TimePoint start_time_;
    TimePoint last_progress_time_;

    std::shared_ptr<IProtocolHandler> handler_;
    IEventListener* listener_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> cancel_requested_{false};
    std::atomic<bool> pause_requested_{false};
};

}  // namespace falcon
