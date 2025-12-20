// Falcon Download Task - Core Implementation
// Copyright (c) 2025 Falcon Project

#include <falcon/download_task.hpp>
#include <falcon/protocol_handler.hpp>

namespace falcon {

DownloadTask::DownloadTask(TaskId id, std::string url, DownloadOptions options)
    : id_(id), url_(std::move(url)), options_(std::move(options)) {}

DownloadTask::~DownloadTask() {
    // Ensure task is cancelled if still active
    if (is_active()) {
        cancel();
    }
}

float DownloadTask::progress() const noexcept {
    Bytes total = total_bytes_.load();
    if (total == 0) {
        return 0.0f;
    }
    Bytes downloaded = downloaded_bytes_.load();
    return static_cast<float>(downloaded) / static_cast<float>(total);
}

Duration DownloadTask::elapsed() const noexcept {
    if (start_time_ == TimePoint{}) {
        return Duration::zero();
    }
    if (is_finished()) {
        return last_progress_time_ - start_time_;
    }
    return std::chrono::steady_clock::now() - start_time_;
}

Duration DownloadTask::estimated_remaining() const noexcept {
    BytesPerSecond spd = current_speed_.load();
    if (spd == 0) {
        return Duration::zero();
    }

    Bytes total = total_bytes_.load();
    Bytes downloaded = downloaded_bytes_.load();
    if (total <= downloaded) {
        return Duration::zero();
    }

    Bytes remaining = total - downloaded;
    auto seconds = remaining / spd;
    return std::chrono::seconds(seconds);
}

bool DownloadTask::is_active() const noexcept {
    TaskStatus s = status_.load();
    return s == TaskStatus::Downloading || s == TaskStatus::Preparing;
}

bool DownloadTask::is_finished() const noexcept {
    TaskStatus s = status_.load();
    return s == TaskStatus::Completed || s == TaskStatus::Failed ||
           s == TaskStatus::Cancelled;
}

bool DownloadTask::pause() {
    TaskStatus current = status_.load();
    if (current != TaskStatus::Downloading && current != TaskStatus::Preparing) {
        return false;
    }

    pause_requested_.store(true);

    if (handler_) {
        handler_->pause(shared_from_this());
    } else {
        // No handler, directly set status to paused
        set_status(TaskStatus::Paused);
    }

    return true;
}

bool DownloadTask::resume() {
    TaskStatus current = status_.load();
    if (current != TaskStatus::Paused) {
        return false;
    }

    pause_requested_.store(false);

    if (handler_) {
        handler_->resume(shared_from_this(), listener_);
    } else {
        // No handler, directly set status back to downloading
        set_status(TaskStatus::Downloading);
    }

    return true;
}

bool DownloadTask::cancel() {
    if (is_finished()) {
        return false;
    }

    cancel_requested_.store(true);

    if (handler_) {
        handler_->cancel(shared_from_this());
    }

    set_status(TaskStatus::Cancelled);
    return true;
}

void DownloadTask::wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [this] { return is_finished(); });
}

bool DownloadTask::wait_for(Duration timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return is_finished(); });
}

void DownloadTask::set_handler(std::shared_ptr<IProtocolHandler> handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

void DownloadTask::set_listener(IEventListener* listener) {
    std::lock_guard<std::mutex> lock(mutex_);
    listener_ = listener;
}

void DownloadTask::set_status(TaskStatus new_status) {
    TaskStatus old_status = status_.exchange(new_status);

    if (old_status == new_status) {
        return;
    }

    // Notify listener
    if (listener_) {
        listener_->on_status_changed(id_, old_status, new_status);
    }

    // Notify waiters if finished
    if (new_status == TaskStatus::Completed || new_status == TaskStatus::Failed ||
        new_status == TaskStatus::Cancelled) {
        std::lock_guard<std::mutex> lock(mutex_);
        cv_.notify_all();
    }
}

void DownloadTask::update_progress(Bytes downloaded, Bytes total,
                                   BytesPerSecond speed) {
    downloaded_bytes_.store(downloaded);
    total_bytes_.store(total);
    current_speed_.store(speed);
    last_progress_time_ = std::chrono::steady_clock::now();

    if (listener_) {
        listener_->on_progress(get_progress_info());
    }
}

void DownloadTask::set_file_info(const FileInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    file_info_ = info;
    total_bytes_.store(info.total_size);

    if (listener_) {
        listener_->on_file_info(id_, info);
    }
}

void DownloadTask::set_output_path(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    output_path_ = path;
}

void DownloadTask::set_error(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    error_message_ = message;

    if (listener_) {
        listener_->on_error(id_, message);
    }
}

void DownloadTask::mark_started() {
    start_time_ = std::chrono::steady_clock::now();
    last_progress_time_ = start_time_;
}

ProgressInfo DownloadTask::get_progress_info() const {
    ProgressInfo info;
    info.task_id = id_;
    info.downloaded_bytes = downloaded_bytes_.load();
    info.total_bytes = total_bytes_.load();
    info.speed = current_speed_.load();
    info.progress = progress();
    info.elapsed = elapsed();
    info.estimated_remaining = estimated_remaining();
    return info;
}

}  // namespace falcon
