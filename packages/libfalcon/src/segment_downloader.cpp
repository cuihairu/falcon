// Falcon Segment Downloader - Implementation
// Copyright (c) 2025 Falcon Project

#include <falcon/segment_downloader.hpp>

#include <falcon/exceptions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace falcon {

namespace {
    // Generate random string for temp file suffix
    std::string generate_random_suffix(std::size_t length = 8) {
        const char charset[] = "0123456789abcdef";
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, sizeof(charset) - 2);

        std::string result;
        result.reserve(length);
        for (std::size_t i = 0; i < length; ++i) {
            result += charset[dis(gen)];
        }
        return result;
    }

    // Format bytes to human readable string
    std::string format_bytes(Bytes bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit_index = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024.0 && unit_index < 4) {
            size /= 1024.0;
            ++unit_index;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
        return oss.str();
    }
}  // namespace

SegmentDownloader::SegmentDownloader(DownloadTask::Ptr task,
                                     const std::string& url,
                                     const std::string& output_path,
                                     const SegmentConfig& config)
    : task_(std::move(task)),
      url_(url),
      output_path_(output_path),
      config_(config) {
    stats_.start_time = std::chrono::steady_clock::now();
    stats_.last_update = stats_.start_time;
}

SegmentDownloader::~SegmentDownloader() {
    cancel();
    cleanup_segment_files();
}

std::size_t SegmentDownloader::calculate_optimal_segments(
    Bytes file_size,
    const SegmentConfig& config) {

    // Don't split if file is too small
    if (file_size < config.min_file_size) {
        return 1;
    }

    // Calculate based on file size and segment size constraints
    std::size_t min_segments = file_size / config.max_segment_size;
    std::size_t max_segments = file_size / config.min_segment_size;

    // Ensure at least 1 segment
    min_segments = std::max(min_segments, std::size_t{1});
    max_segments = std::max(max_segments, std::size_t{1});

    // Use configured connection count if within range
    if (config.num_connections > 0) {
        std::size_t segments = config.num_connections;
        if (segments >= min_segments && segments <= max_segments) {
            return segments;
        }
        // Clamp to valid range
        return std::clamp(segments, min_segments, max_segments);
    }

    // Auto-detect: start with min_segments and increase if beneficial
    // For most cases, 4-8 segments is optimal
    std::size_t optimal = std::clamp(
        static_cast<std::size_t>(4),
        min_segments,
        std::min(max_segments, static_cast<std::size_t>(8))
    );

    return optimal;
}

void SegmentDownloader::initialize_segments(Bytes file_size) {
    std::lock_guard<std::mutex> lock(segments_mutex_);

    segments_.clear();
    next_segment_ = 0;

    std::size_t num_segments = calculate_optimal_segments(file_size, config_);

    if (config_.adaptive_sizing) {
        calculate_adaptive_segments(file_size);
    } else {
        // Equal-sized segments
        Bytes segment_size = file_size / num_segments;
        Bytes remainder = file_size % num_segments;

        Bytes current_pos = 0;
        for (std::size_t i = 0; i < num_segments; ++i) {
            Bytes segment_end = current_pos + segment_size - 1;
            if (i == num_segments - 1) {
                // Last segment gets the remainder
                segment_end += remainder;
            }

            auto segment = std::make_shared<Segment>(i, current_pos, segment_end);
            segments_.push_back(segment);

            current_pos = segment_end + 1;
        }
    }

    stats_.total_size = file_size;
}

void SegmentDownloader::calculate_adaptive_segments(Bytes file_size) {
    // Adaptive sizing strategy:
    // - Start segments small at the beginning for faster startup
    // - Gradually increase segment size
    // - Last segment may be larger

    std::size_t num_segments = config_.num_connections;
    if (num_segments == 0) {
        num_segments = calculate_optimal_segments(file_size, config_);
    }

    std::vector<Bytes> segment_sizes;
    segment_sizes.reserve(num_segments);

    // Base segment size (will be adjusted)
    Bytes base_size = file_size / num_segments;

    // Adaptive sizing: smaller segments at the start
    for (std::size_t i = 0; i < num_segments; ++i) {
        // Progressively increase segment size
        double multiplier = 0.5 + (1.5 * i / (num_segments - 1));
        Bytes seg_size = static_cast<Bytes>(base_size * multiplier);

        // Clamp to min/max segment size
        seg_size = std::clamp(seg_size,
                              static_cast<Bytes>(config_.min_segment_size),
                              static_cast<Bytes>(config_.max_segment_size));

        segment_sizes.push_back(seg_size);
    }

    // Create segments with calculated sizes
    Bytes current_pos = 0;
    for (std::size_t i = 0; i < num_segments; ++i) {
        Bytes segment_end = current_pos + segment_sizes[i] - 1;

        // Last segment gets remaining bytes
        if (i == num_segments - 1) {
            segment_end = file_size - 1;
        }

        auto segment = std::make_shared<Segment>(i, current_pos, segment_end);
        segments_.push_back(segment);

        current_pos = segment_end + 1;
    }
}

bool SegmentDownloader::start(SegmentDownloadFunc download_func) {
    if (running_.load()) {
        return false;  // Already running
    }

    // Get file size from task info
    Bytes file_size = task_->file_info().total_size;
    if (file_size == 0) {
        throw FileIOException("Unknown file size, cannot perform segmented download");
    }

    // Initialize segments
    initialize_segments(file_size);

    if (segments_.empty()) {
        throw FileIOException("Failed to create download segments");
    }

    running_.store(true);
    paused_.store(false);
    cancelled_.store(false);

    // Reset stats
    stats_.total_downloaded.store(0);
    stats_.total_size.store(file_size);
    stats_.completed_segments.store(0);
    stats_.active_connections.store(0);
    stats_.start_time = std::chrono::steady_clock::now();
    stats_.last_update = stats_.start_time;
    stats_.last_downloaded = 0;

    // Start worker threads
    std::size_t num_workers = std::min(
        segments_.size(),
        static_cast<std::size_t>(config_.num_connections > 0 ?
                                 config_.num_connections : segments_.size())
    );

    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.reserve(num_workers);
    }

    // Allocate initial segments
    for (std::size_t i = 0; i < num_workers; ++i) {
        auto segment = allocate_segment();
        if (!segment) {
            break;  // No more segments to allocate
        }

        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back([this, segment, download_func]() {
            download_segment(segment, download_func);
        });
        active_workers_++;
    }

    // Start connection monitor thread
    monitor_thread_ = std::thread([this]() {
        monitor_connections();
    });

    // Wait for all workers to complete
    {
        std::unique_lock<std::mutex> lock(workers_mutex_);
        cv_.wait(lock, [this]() {
            return active_workers_ == 0 || cancelled_.load();
        });
    }

    // Join all worker threads
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    // Stop monitor thread
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }

    running_.store(false);

    // Check if all segments completed successfully
    if (cancelled_.load()) {
        return false;
    }

    if (!all_segments_completed()) {
        throw FileIOException("Download incomplete: not all segments finished");
    }

    // Merge segments into final file
    return merge_segments();
}

std::shared_ptr<Segment> SegmentDownloader::allocate_segment() {
    std::lock_guard<std::mutex> lock(segments_mutex_);

    while (next_segment_ < segments_.size()) {
        auto& segment = segments_[next_segment_];
        next_segment_++;

        if (!segment->completed.load() && !segment->active.load()) {
            segment->active.store(true);
            stats_.active_connections++;
            return segment;
        }
    }

    return nullptr;  // No more segments to allocate
}

void SegmentDownloader::download_segment(
    std::shared_ptr<Segment> segment,
    SegmentDownloadFunc download_func) {

    std::size_t retry_count = 0;

    while (!cancelled_.load() && !segment->completed.load() &&
           retry_count <= config_.max_retries) {

        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        try {
            std::string segment_path = get_segment_path(segment->index);

            // Call the download function
            bool success = download_func(
                url_,
                segment->start + segment->downloaded,  // Resume position
                segment->end,
                segment_path,
                cancelled_
            );

            if (success && !cancelled_.load()) {
                // Mark segment as completed
                complete_segment(segment);
                break;
            } else if (cancelled_.load()) {
                break;
            }

        } catch (const std::exception& e) {
            std::cerr << "Segment " << segment->index
                     << " error: " << e.what() << std::endl;
        }

        // Retry logic
        if (retry_count < config_.max_retries && !cancelled_.load()) {
            retry_count++;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(config_.retry_delay_ms * retry_count)
            );
        }
    }

    // Decrease active connections count
    stats_.active_connections--;

    // Try to allocate next segment
    if (!cancelled_.load() && segment->completed.load()) {
        auto next_seg = allocate_segment();
        if (next_seg) {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            workers_.emplace_back([this, next_seg, download_func]() {
                download_segment(next_seg, download_func);
            });
            active_workers_++;
        }
    }

    // Check if all workers are done
    if (stats_.active_connections == 0 || all_segments_completed()) {
        cv_.notify_all();
    }

    active_workers_--;
}

void SegmentDownloader::complete_segment(std::shared_ptr<Segment> segment) {
    segment->completed.store(true);
    segment->active.store(false);
    stats_.completed_segments++;

    // Get actual downloaded size
    std::string segment_path = get_segment_path(segment->index);
    std::ifstream file(segment_path, std::ios::binary | std::ios::ate);
    if (file.is_open()) {
        segment->downloaded = static_cast<Bytes>(file.tellg());
        stats_.total_downloaded += segment->downloaded;
    }

    update_progress();
}

bool SegmentDownloader::merge_segments() {
    std::ofstream output_file(output_path_, std::ios::binary);
    if (!output_file.is_open()) {
        throw FileIOException("Failed to create output file: " + output_path_);
    }

    // Merge segments in order
    for (const auto& segment : segments_) {
        std::string segment_path = get_segment_path(segment->index);
        std::ifstream input_file(segment_path, std::ios::binary);
        if (!input_file.is_open()) {
            throw FileIOException("Failed to open segment file: " + segment_path);
        }

        output_file << input_file.rdbuf();
        input_file.close();
    }

    output_file.close();

    // Clean up segment files
    cleanup_segment_files();

    return true;
}

void SegmentDownloader::pause() {
    paused_.store(true);
}

void SegmentDownloader::resume() {
    paused_.store(false);
}

void SegmentDownloader::cancel() {
    cancelled_.store(true);
    running_.store(false);
    cv_.notify_all();

    // Wait for workers to finish
    std::lock_guard<std::mutex> lock(workers_mutex_);
    for (auto& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();

    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void SegmentDownloader::monitor_connections() {
    while (running_.load() && !cancelled_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));

        // Update speed calculation
        auto now = std::chrono::steady_clock::now();
        Bytes downloaded = stats_.total_downloaded.load();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - stats_.last_update).count();

        if (elapsed > 0) {
            Bytes diff = downloaded - stats_.last_downloaded;
            BytesPerSecond speed = (diff * 1000) / static_cast<Bytes>(elapsed);
            current_speed_.store(speed);
            stats_.last_downloaded = downloaded;
            stats_.last_update = now;
        }

        // Update task progress
        update_progress();

        // Check for slow connections
        if (config_.slow_speed_threshold > 0 && active_workers_ > 0) {
            BytesPerSecond speed = current_speed_.load();
            if (speed < config_.slow_speed_threshold) {
                // Could restart slow connections here
                // For now, just log
                // std::cerr << "Slow connection detected: "
                //          << format_bytes(speed) << "/s" << std::endl;
            }
        }
    }
}

void SegmentDownloader::update_progress() {
    if (event_listener_ && task_) {
        ProgressInfo progress;
        progress.task_id = task_->id();
        progress.downloaded_bytes = stats_.total_downloaded.load();
        progress.total_bytes = stats_.total_size.load();
        progress.speed = current_speed_.load();

        task_->update_progress(
            progress.downloaded_bytes,
            progress.total_bytes,
            progress.speed
        );

        event_listener_->on_progress(progress);
    }
}

bool SegmentDownloader::is_active() const noexcept {
    return running_.load() && !paused_.load();
}

float SegmentDownloader::progress() const noexcept {
    return stats_.progress();
}

BytesPerSecond SegmentDownloader::speed() const noexcept {
    return current_speed_.load();
}

Bytes SegmentDownloader::downloaded_bytes() const noexcept {
    return stats_.total_downloaded.load();
}

Bytes SegmentDownloader::total_bytes() const noexcept {
    return stats_.total_size.load();
}

std::size_t SegmentDownloader::completed_segments() const noexcept {
    return stats_.completed_segments.load();
}

std::size_t SegmentDownloader::total_segments() const noexcept {
    // segments_mutex_ is mutable, so we can lock it in a const method
    std::lock_guard<std::mutex> lock(const_cast<SegmentDownloader*>(this)->segments_mutex_);
    return segments_.size();
}

std::size_t SegmentDownloader::active_connections() const noexcept {
    return stats_.active_connections.load();
}

void SegmentDownloader::set_event_listener(IEventListener* listener) {
    event_listener_ = listener;
}

bool SegmentDownloader::all_segments_completed() const noexcept {
    std::lock_guard<std::mutex> lock(const_cast<SegmentDownloader*>(this)->segments_mutex_);
    for (const auto& segment : segments_) {
        if (!segment->completed.load()) {
            return false;
        }
    }
    return true;
}

std::string SegmentDownloader::get_segment_path(std::size_t segment_index) const {
    // Create temp file path for segment
    // Format: output_path.falcon.tmp.segN
    return output_path_ + ".falcon.tmp.seg" + std::to_string(segment_index);
}

void SegmentDownloader::cleanup_segment_files() {
    std::lock_guard<std::mutex> lock(segments_mutex_);
    for (const auto& segment : segments_) {
        std::string segment_path = get_segment_path(segment->index);
        std::remove(segment_path.c_str());
    }
}

}  // namespace falcon
