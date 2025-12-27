#pragma once

#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/types.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace falcon {

/// Forward declarations
class DownloadTask;
class IEventListener;

/// Single segment download information
struct Segment {
    std::size_t index;              // Segment index
    Bytes start;                    // Start byte position (inclusive)
    Bytes end;                      // End byte position (inclusive)
    std::atomic<Bytes> downloaded{0};  // Bytes downloaded for this segment
    std::atomic<bool> completed{false};
    std::atomic<bool> active{false};

    Segment(std::size_t idx, Bytes s, Bytes e)
        : index(idx), start(s), end(e) {}

    /// Get segment size in bytes
    [[nodiscard]] Bytes size() const noexcept {
        return end - start + 1;
    }

    /// Get remaining bytes
    [[nodiscard]] Bytes remaining() const noexcept {
        return size() - downloaded.load();
    }

    /// Get progress ratio (0.0 ~ 1.0)
    [[nodiscard]] float progress() const noexcept {
        Bytes seg_size = size();
        return seg_size > 0 ? static_cast<float>(downloaded.load()) / static_cast<float>(seg_size) : 1.0f;
    }
};

/// Statistics for segmented download
struct SegmentStats {
    std::atomic<Bytes> total_downloaded{0};
    std::atomic<Bytes> total_size{0};
    std::atomic<std::size_t> completed_segments{0};
    std::atomic<std::size_t> active_connections{0};
    std::chrono::steady_clock::time_point start_time;
    std::chrono::steady_clock::time_point last_update;
    Bytes last_downloaded{0};

    /// Get overall progress (0.0 ~ 1.0)
    [[nodiscard]] float progress() const noexcept {
        Bytes total = total_size.load();
        return total > 0 ? static_cast<float>(total_downloaded.load()) / static_cast<float>(total) : 0.0f;
    }

    /// Get download speed in bytes/second
    [[nodiscard]] BytesPerSecond speed() const noexcept {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_update).count();

        if (elapsed <= 0) {
            return 0;
        }

        Bytes downloaded = total_downloaded.load();
        Bytes diff = downloaded - last_downloaded;

        // Update last_update time
        // Note: This is called from read context, so we can't update here
        // The caller should update last_update after getting the speed

        return (diff * 1000) / static_cast<Bytes>(elapsed);
    }
};

/// Configuration for segmented downloader
struct SegmentConfig {
    /// Number of concurrent connections (0 = auto-detect based on file size)
    std::size_t num_connections = 4;

    /// Minimum segment size in bytes (files smaller than this won't be split)
    std::size_t min_segment_size = 1024 * 1024;  // 1 MB

    /// Maximum segment size in bytes (each segment won't exceed this)
    std::size_t max_segment_size = 16 * 1024 * 1024;  // 16 MB

    /// Minimum file size to enable segmented download (0 = always enabled)
    std::size_t min_file_size = 5 * 1024 * 1024;  // 5 MB

    /// Connection timeout per segment
    std::size_t timeout_seconds = 30;

    /// Maximum retry attempts per segment
    std::size_t max_retries = 3;

    /// Retry delay in milliseconds
    std::size_t retry_delay_ms = 1000;

    /// Buffer size for each segment download
    std::size_t buffer_size = 64 * 1024;  // 64 KB

    /// Enable adaptive segment sizing
    bool adaptive_sizing = true;

    /// Slow speed threshold in bytes/sec (0 = disabled)
    /// If speed stays below this for slow_timeout, restart the connection
    BytesPerSecond slow_speed_threshold = 1024;  // 1 KB/s

    /// Time in seconds to wait before restarting slow connection
    std::size_t slow_timeout = 30;

    /// Enable piece validation (check downloaded bytes match expected)
    bool validate_pieces = true;
};

/// Segment downloader for multi-threaded chunked downloads
/**
 * This class implements aria2-style segmented downloading:
 * - Splits files into multiple segments
 * - Downloads segments in parallel using multiple connections
 * - Merges segments into final file
 * - Supports pause/resume per segment
 * - Adaptive connection allocation
 */
class SegmentDownloader {
public:
    /// Callback function type for downloading a single segment
    using SegmentDownloadFunc = std::function<bool(
        const std::string& url,
        Bytes start,
        Bytes end,
        const std::string& output_path,
        std::atomic<bool>& cancelled
    )>;

    /// Create a segment downloader
    SegmentDownloader(DownloadTask::Ptr task,
                      const std::string& url,
                      const std::string& output_path,
                      const SegmentConfig& config = {});

    /// Destructor
    ~SegmentDownloader();

    // Non-copyable, non-movable
    SegmentDownloader(const SegmentDownloader&) = delete;
    SegmentDownloader& operator=(const SegmentDownloader&) = delete;
    SegmentDownloader(SegmentDownloader&&) = delete;
    SegmentDownloader& operator=(SegmentDownloader&&) = delete;

    /// Start segmented download
    /// @param download_func Function to download a single segment
    /// @return true if download completed successfully
    bool start(SegmentDownloadFunc download_func);

    /// Pause the download (stops all active segments)
    void pause();

    /// Resume the download (restarts from current progress)
    void resume();

    /// Cancel the download
    void cancel();

    /// Check if download is active
    [[nodiscard]] bool is_active() const noexcept;

    /// Get current progress (0.0 ~ 1.0)
    [[nodiscard]] float progress() const noexcept;

    /// Get download speed in bytes/second
    [[nodiscard]] BytesPerSecond speed() const noexcept;

    /// Get total downloaded bytes
    [[nodiscard]] Bytes downloaded_bytes() const noexcept;

    /// Get total file size
    [[nodiscard]] Bytes total_bytes() const noexcept;

    /// Get number of completed segments
    [[nodiscard]] std::size_t completed_segments() const noexcept;

    /// Get total number of segments
    [[nodiscard]] std::size_t total_segments() const noexcept;

    /// Get current number of active connections
    [[nodiscard]] std::size_t active_connections() const noexcept;

    /// Set event listener
    void set_event_listener(IEventListener* listener);

    /// Get segment configuration
    [[nodiscard]] const SegmentConfig& config() const noexcept {
        return config_;
    }

    /// Calculate optimal number of segments for a given file size
    [[nodiscard]] static std::size_t calculate_optimal_segments(
        Bytes file_size,
        const SegmentConfig& config = {});

private:
    /// Initialize segments based on file size
    void initialize_segments(Bytes file_size);

    /// Worker thread function for downloading a single segment
    void download_segment(std::shared_ptr<Segment> segment,
                          SegmentDownloadFunc download_func);

    /// Merge all completed segments into final file
    bool merge_segments();

    /// Allocate a segment to a worker thread
    std::shared_ptr<Segment> allocate_segment();

    /// Mark segment as completed and allocate next one
    void complete_segment(std::shared_ptr<Segment> segment);

    /// Update progress statistics
    void update_progress();

    /// Check if all segments are completed
    [[nodiscard]] bool all_segments_completed() const noexcept;

    /// Get temporary file path for a segment
    [[nodiscard]] std::string get_segment_path(std::size_t segment_index) const;

    /// Clean up temporary segment files
    void cleanup_segment_files();

    /// Monitor slow connections and restart them
    void monitor_connections();

    /// Calculate segment sizes adaptively
    void calculate_adaptive_segments(Bytes file_size);

private:
    struct FailureState {
        std::mutex mutex;
        std::optional<std::string> message;
    };

    DownloadTask::Ptr task_;
    std::string url_;
    std::string output_path_;
    SegmentConfig config_;

    std::vector<std::shared_ptr<Segment>> segments_;
    std::vector<std::thread> workers_;
    std::thread monitor_thread_;

    SegmentStats stats_;

    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> cancelled_{false};

    std::mutex segments_mutex_;
    std::mutex workers_mutex_;
    std::condition_variable cv_;

    std::size_t next_segment_{0};
    std::atomic<std::size_t> active_workers_{0};
    std::atomic<bool> failed_{false};
    FailureState failure_;

    IEventListener* event_listener_{nullptr};

    std::atomic<BytesPerSecond> current_speed_{0};
};

}  // namespace falcon
