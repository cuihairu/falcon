/**
 * @file task_storage_listener.cpp
 * @brief Download engine event listener that persists task state to TaskStorage
 * @author Falcon Team
 * @date 2026-09-11
 */

#include "storage/task_storage_listener.hpp"

#include "storage/task_storage.hpp"

namespace falcon::daemon {

TaskStorageListener::TaskStorageListener(
    TaskStorage* storage, std::chrono::milliseconds progress_flush_interval)
    : storage_(storage), progress_flush_interval_(progress_flush_interval) {}

void TaskStorageListener::on_status_changed(TaskId task_id, TaskStatus /*old_status*/,
                                            TaskStatus new_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || !storage_) return;

    switch (new_status) {
    case TaskStatus::Completed:
        // mark_completed() 记录终态并写入 completed_at
        storage_->mark_completed(task_id);
        last_error_.erase(task_id);
        break;
    case TaskStatus::Failed: {
        // 错误消息来自此前 on_error() 的缓存
        std::string message;
        if (auto it = last_error_.find(task_id); it != last_error_.end()) {
            message = std::move(it->second);
            last_error_.erase(it);
        }
        storage_->mark_failed(task_id, message);
        break;
    }
    case TaskStatus::Cancelled:
        storage_->update_status(task_id, new_status);
        last_error_.erase(task_id);
        break;
    default:
        // Pending/Preparing/Downloading/Paused：直接同步状态
        storage_->update_status(task_id, new_status);
        break;
    }
}

void TaskStorageListener::on_progress(const ProgressInfo& info) {
    if (info.task_id == INVALID_TASK_ID) return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || !storage_) return;

    // 同一任务的进度按时间节流落库
    auto [it, inserted] = last_flush_.try_emplace(info.task_id, now);
    if (!inserted) {
        if (now - it->second < progress_flush_interval_) return;
        it->second = now;
    }

    storage_->update_progress(info.task_id, info.downloaded_bytes,
                              static_cast<double>(info.progress), info.speed);
}

void TaskStorageListener::on_error(TaskId task_id, const std::string& error_message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_) return;
    // 仅缓存，等任务进入 Failed 时随 mark_failed 一并落库
    last_error_[task_id] = error_message;
}

void TaskStorageListener::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    storage_ = nullptr;
    last_flush_.clear();
    last_error_.clear();
}

}  // namespace falcon::daemon
