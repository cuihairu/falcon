// Falcon Segment Downloader - Implementation
// Copyright (c) 2025 Falcon Project

#include <falcon/protocols/segment_downloader.hpp>

#include <falcon/exceptions.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <system_error>

namespace falcon {

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

    // Don't split very small files unless max segment size would be exceeded.
    if (file_size < config.min_file_size && file_size <= config.max_segment_size) {
        return 1;
    }

    // Calculate based on file size and segment size constraints
    std::size_t min_segments =
        std::max<std::size_t>(1, (file_size + config.max_segment_size - 1) / config.max_segment_size);
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

    std::size_t num_segments = calculate_optimal_segments(file_size, config_);

    if (num_segments <= 1) {
        segments_.push_back(std::make_shared<Segment>(0, 0, file_size - 1));
        return;
    }

    std::vector<Bytes> segment_sizes;
    segment_sizes.reserve(num_segments);

    // Base segment size (will be adjusted)
    Bytes base_size = file_size / num_segments;

    // Adaptive sizing: smaller segments at the start
    for (std::size_t i = 0; i < num_segments; ++i) {
        // Progressively increase segment size
        double multiplier = 0.5 + (1.5 * static_cast<double>(i) / static_cast<double>(num_segments - 1));
        Bytes seg_size = static_cast<Bytes>(static_cast<double>(base_size) * multiplier);

        // Clamp to min/max segment size
        seg_size = std::clamp(seg_size,
                              static_cast<Bytes>(config_.min_segment_size),
                              static_cast<Bytes>(config_.max_segment_size));

        segment_sizes.push_back(seg_size);
    }

    // Create segments with calculated sizes
    Bytes current_pos = 0;
    for (std::size_t i = 0; i < num_segments; ++i) {
        Bytes remaining_bytes = file_size - current_pos;
        Bytes remaining_segments = static_cast<Bytes>(num_segments - i);
        Bytes segment_size = segment_sizes[i];

        // Last segment gets remaining bytes
        if (i == num_segments - 1) {
            segment_size = remaining_bytes;
        } else {
            // Keep room for the remaining segments and avoid invalid ranges.
            Bytes max_size_for_segment = remaining_bytes - (remaining_segments - 1);
            segment_size = std::min(segment_size, max_size_for_segment);
            segment_size = std::max<Bytes>(1, segment_size);
        }

        Bytes segment_end = current_pos + segment_size - 1;

        auto segment = std::make_shared<Segment>(i, current_pos, segment_end);
        segments_.push_back(segment);

        current_pos = segment_end + 1;
    }
}

bool SegmentDownloader::start(SegmentDownloadFunc download_func) {
    if (running_.load()) {
        return false;  // Already running
    }
    if (cancelled_.load()) {
        return false;  // Cancelled before start
    }

    try {
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
    failed_.store(false);

    // Reset stats
    stats_.total_size.store(file_size);
    stats_.active_connections.store(0);
    stats_.start_time = std::chrono::steady_clock::now();
    stats_.last_update = stats_.start_time;
    stats_.last_downloaded = 0;
    current_speed_.store(0);

    // Ensure output directory exists (for segment temp files and final output)
    if (task_ && task_->options().create_directory) {
        try {
            std::filesystem::path out_path(output_path_);
            auto parent = out_path.parent_path();
            if (!parent.empty()) {
                std::filesystem::create_directories(parent);
            }
        } catch (const std::exception& e) {
            running_.store(false);
            throw FileIOException(std::string("Failed to create output directory: ") + e.what());
        }
    }

    // Resume support: detect existing segment files and continue from where we left off
    const bool resume_enabled = task_ ? task_->options().resume_enabled : false;
    if (!resume_enabled) {
        cleanup_segment_files();
    } else {
        for (auto& segment : segments_) {
            const std::string segment_path = get_segment_path(segment->index);
            // 只有普通文件才可能是断点段文件：路径被目录占用时 ifstream
            // 打开目录同样成功、tellg() 返回目录 st_size（文件系统相关，
            // CI 的 /tmp 上可远大于段大小）——继续往下走会把"目录占用"
            // 误判为"超尺寸损坏段"并把空目录删掉，下载照常进行。非普通
            // 文件一律不视为断点也不删除，交给段下载以打开失败收口
            std::error_code reg_ec;
            if (!std::filesystem::is_regular_file(segment_path, reg_ec)) continue;
            std::ifstream file(segment_path, std::ios::binary | std::ios::ate);
            if (!file.is_open()) continue;
            auto sz = file.tellg();
            if (sz <= 0) continue;
            Bytes downloaded = static_cast<Bytes>(sz);
            if (downloaded > segment->size()) {
                // 超尺寸段不可信（Range 被服务器忽略、旧版缺陷遗留的损坏
                // 段文件）：删除整段重下，绝不带着可疑数据进 merge
                file.close();
                std::error_code rm_ec;
                std::filesystem::remove(segment_path, rm_ec);
                continue;
            }
            segment->downloaded.store(downloaded);
            if (downloaded >= segment->size()) {
                segment->completed.store(true);
            }
        }
    }

    update_progress();

    // If everything is already downloaded, just merge.
    if (all_segments_completed()) {
        running_.store(false);
        return merge_segments();
    }

    // Start connection monitor thread (store under lock to avoid races with cancel())
    {
        std::thread monitor([this]() { monitor_connections(); });
        std::lock_guard<std::mutex> lock(workers_mutex_);
        monitor_thread_ = std::move(monitor);
    }

    // Start worker threads
    std::size_t desired_workers = config_.num_connections > 0 ? config_.num_connections : segments_.size();
    std::size_t num_workers = std::max<std::size_t>(1, std::min(desired_workers, segments_.size()));
    active_workers_.store(num_workers);

    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.clear();
        workers_.reserve(num_workers);

        for (std::size_t i = 0; i < num_workers; ++i) {
            workers_.emplace_back([this, download_func]() {
                while (!cancelled_.load() && !failed_.load()) {
                    if (paused_.load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }

                    auto segment = allocate_segment();
                    if (!segment) {
                        break;
                    }

                    download_segment(std::move(segment), download_func);
                }

                if (active_workers_.fetch_sub(1) == 1) {
                    cv_.notify_all();
                } else {
                    cv_.notify_all();
                }
            });
        }
    }

    // Wait for all workers to complete or cancellation/failure
    {
        std::unique_lock<std::mutex> lock(workers_mutex_);

        // Add timeout to prevent real deadlock
        bool wait_result = cv_.wait_for(lock, std::chrono::seconds(30), [this]() {
            return active_workers_.load() == 0 || cancelled_.load() || failed_.load();
        });

        if (!wait_result) {
            // Timeout - this indicates a real bug
            std::cerr << "ERROR: Timeout waiting for workers! active_workers="
                     << active_workers_.load() << ", cancelled=" << cancelled_.load()
                     << ", failed=" << failed_.load() << std::endl;
            cancelled_.store(true);
            failed_.store(true);
        }
    }

    running_.store(false);

    // Join workers (don't hold workers_mutex_ while joining)
    std::vector<std::thread> workers_to_join;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_to_join.swap(workers_);
    }
    for (auto& worker : workers_to_join) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    // Stop monitor thread
    {
        std::thread monitor_to_join;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            monitor_to_join = std::move(monitor_thread_);
        }
        if (monitor_to_join.joinable()) {
            monitor_to_join.join();
        }
    }

    if (failed_.load()) {
        return false;
    }

    // Check if cancelled
    if (cancelled_.load()) {
        return false;
    }

    if (!all_segments_completed()) {
        return false;
    }

    // Merge segments into final file
    return merge_segments();
    } catch (const std::exception& e) {
        {
            std::lock_guard<std::mutex> lock(failure_.mutex);
            if (!failure_.message.has_value()) {
                failure_.message = e.what();
            }
        }
        failed_.store(true);
        cancelled_.store(true);
        cancel();
        return false;
    }
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

    // Ensure segment is marked inactive and active connection count is decremented on all exits.
    struct ConnectionGuard {
        std::shared_ptr<Segment> segment;
        SegmentStats* stats;
        explicit ConnectionGuard(std::shared_ptr<Segment> s, SegmentStats* st)
            : segment(std::move(s)), stats(st) {}
        ~ConnectionGuard() {
            if (segment) {
                segment->active.store(false);
            }
            if (stats) {
                stats->active_connections--;
            }
        }
    } guard(segment, &stats_);

    std::size_t attempt = 0;
    std::string last_error;

    while (!cancelled_.load() && !segment->completed.load() &&
           attempt <= config_.max_retries) {

        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        const std::string segment_path = get_segment_path(segment->index);
        const Bytes segment_size = segment->size();
        const Bytes existing_downloaded = std::min(segment->downloaded.load(), segment_size);
        if (existing_downloaded >= segment_size) {
            complete_segment(segment);
            break;
        }
        const std::string& download_path = segment_path;

        try {
            // Call the download function
            bool success = download_func(
                url_,
                segment->start + existing_downloaded,  // Resume position
                segment->end,
                download_path,
                cancelled_
            );

            if (success && !cancelled_.load()) {
                // 精确尺寸校验（取代 validate_pieces 开关的宽松校验）：
                // 段文件完成时必须恰好等于段长——短传（无 Content-Length
                // 时提前断连 curl 也可报 OK）与超发（Range 被忽略回传
                // 整个文件）都在这里拦下，损坏段不进 merge；尺寸不符按
                // 失败走下方 best-effort 更新与重试
                std::error_code size_ec;
                const auto actual_size =
                    std::filesystem::file_size(download_path, size_ec);
                if (!size_ec && actual_size == segment_size) {
                    segment->downloaded.store(segment_size);
                    complete_segment(segment);
                    break;
                }
                last_error = "Segment size mismatch: seg" +
                             std::to_string(segment->index) +
                             " (expected " + std::to_string(segment_size) +
                             ", got " +
                             std::to_string(size_ec ? 0 : actual_size) + ")";
            } else if (cancelled_.load()) {
                break;
            }

        } catch (const std::exception& e) {
            std::cerr << "Segment " << segment->index
                     << " error: " << e.what() << std::endl;
            last_error = e.what();
        }

        // Update downloaded bytes from partial segment file (best-effort)
        {
            // 同 start() 恢复检测：目录占用的段路径 ifstream 打开成功、
            // tellg() 是目录 st_size——超尺寸删除分支不得作用于目录，
            // 占位目录必须保留到段下载打开失败收口
            std::error_code reg_ec;
            const bool is_regular =
                std::filesystem::is_regular_file(segment_path, reg_ec);
            std::ifstream file(segment_path, std::ios::binary | std::ios::ate);
            if (is_regular && file.is_open()) {
                Bytes downloaded = static_cast<Bytes>(file.tellg());
                if (downloaded > segment_size) {
                    // 超尺寸段不可信（本次尝试已确认数据损坏）：删除，
                    // 下次尝试从段头重来
                    file.close();
                    std::error_code rm_ec;
                    std::filesystem::remove(segment_path, rm_ec);
                    segment->downloaded.store(0);
                } else {
                    segment->downloaded.store(downloaded);
                    if (downloaded >= segment_size) {
                        complete_segment(segment);
                        break;
                    }
                }
            }
        }

        if (cancelled_.load()) {
            break;
        }

        if (attempt >= config_.max_retries) {
            {
                std::lock_guard<std::mutex> lock(failure_.mutex);
                if (!failure_.message.has_value()) {
                    if (!last_error.empty()) {
                        failure_.message = last_error;
                    } else {
                        failure_.message = "Segment failed: seg" + std::to_string(segment->index);
                    }
                }
            }
            failed_.store(true);
            cancelled_.store(true);
            cv_.notify_all();
            break;
        }

        ++attempt;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(config_.retry_delay_ms * attempt)
        );
    }
}

void SegmentDownloader::complete_segment(std::shared_ptr<Segment> segment) {
    segment->completed.store(true);
    segment->active.store(false);

    update_progress();
}

bool SegmentDownloader::merge_segments() {
    const std::string temp_output = output_path_ + ".falcon.tmp.merge";
    std::ofstream output_file(temp_output, std::ios::binary | std::ios::trunc);
    if (!output_file.is_open()) {
        throw FileIOException("Failed to create output file: " + temp_output);
    }

    // Merge segments in order
    for (const auto& segment : segments_) {
        std::string segment_path = get_segment_path(segment->index);

        // 最终闸门：merge 前逐段校验尺寸，任何漂移（短传/超发/外部
        // 篡改）都拒绝出成品——错误的 COMPLETED 比失败的下载更糟
        std::error_code size_ec;
        const auto actual_size =
            std::filesystem::file_size(segment_path, size_ec);
        if (size_ec || actual_size != segment->size()) {
            std::remove(temp_output.c_str());
            throw FileIOException("Segment size mismatch before merge: seg" +
                                  std::to_string(segment->index) +
                                  " (expected " +
                                  std::to_string(segment->size()) + ", got " +
                                  std::to_string(size_ec ? 0 : actual_size) +
                                  ")");
        }

        std::ifstream input_file(segment_path, std::ios::binary);
        if (!input_file.is_open()) {
            throw FileIOException("Failed to open segment file: " + segment_path);
        }

        output_file << input_file.rdbuf();
        input_file.close();
    }

    output_file.close();

    // Atomically move into place
    std::error_code ec;
    std::filesystem::rename(temp_output, output_path_, ec);
    if (ec) {
        std::remove(temp_output.c_str());
        throw FileIOException("Failed to move merged file to destination: " + ec.message());
    }

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
    std::vector<std::thread> workers_to_join;
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_to_join.swap(workers_);
    }
    for (auto& worker : workers_to_join) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    {
        std::thread monitor_to_join;
        {
            std::lock_guard<std::mutex> lock(workers_mutex_);
            monitor_to_join = std::move(monitor_thread_);
        }
        if (monitor_to_join.joinable()) {
            monitor_to_join.join();
        }
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
        if (config_.slow_speed_threshold > 0 && active_workers_.load() > 0) {
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
    if (!task_) return;

    Bytes total_downloaded = 0;
    std::size_t completed = 0;
    {
        std::lock_guard<std::mutex> lock(segments_mutex_);
        for (const auto& segment : segments_) {
            total_downloaded += std::min(segment->downloaded.load(), segment->size());
            if (segment->completed.load()) {
                ++completed;
            }
        }
    }

    stats_.total_downloaded.store(total_downloaded);
    stats_.completed_segments.store(completed);

    task_->update_progress(
        total_downloaded,
        stats_.total_size.load(),
        current_speed_.load()
    );
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
        // 只清理普通文件：std::remove 对空目录等价 rmdir，段路径被目录
        // 占用时不得清掉不属于下载器的目录
        std::error_code reg_ec;
        if (std::filesystem::is_regular_file(segment_path, reg_ec)) {
            std::remove(segment_path.c_str());
        }
        std::string resume_path = segment_path + ".resume";
        std::error_code res_ec;
        if (std::filesystem::is_regular_file(resume_path, res_ec)) {
            std::remove(resume_path.c_str());
        }
    }
}

}  // namespace falcon
