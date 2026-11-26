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

void TaskStorageListener::flush_progress_locked(TaskId task_id) {
    auto it = last_progress_.find(task_id);
    if (it == last_progress_.end()) return;
    const ProgressInfo info = it->second;
    last_progress_.erase(it);
    storage_->update_progress(info.task_id, info.downloaded_bytes,
                              static_cast<double>(info.progress), info.speed,
                              info.total_bytes);
    // 已落库，节流时间戳同步推进，避免紧随其后的事件重复写入
    last_flush_[task_id] = std::chrono::steady_clock::now();
}

void TaskStorageListener::on_status_changed(TaskId task_id, TaskStatus /*old_status*/,
                                            TaskStatus new_status) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || !storage_) return;

    switch (new_status) {
    case TaskStatus::Completed:
        // 终态进度无条件落库（final_update 穿透节流后到达的最新字节计数），
        // mark_completed() 随后记录终态与 completed_at（progress 归一 1.0）
        flush_progress_locked(task_id);
        storage_->mark_completed(task_id);
        last_error_.erase(task_id);
        break;
    case TaskStatus::Failed: {
        // 失败时已下载的部分字节同样真实——终态进度一并落库
        flush_progress_locked(task_id);
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
        // 取消时的部分进度照常落库（回收站/历史视图按字节数展示）
        flush_progress_locked(task_id);
        storage_->update_status(task_id, new_status);
        last_error_.erase(task_id);
        break;
    case TaskStatus::Paused:
        // 暂停是停机恢复的落点——最终字节计数必须落库（此前被 1s 节流
        // 吞掉的尾部进度会在重启后显示为旧值）
        flush_progress_locked(task_id);
        storage_->update_status(task_id, new_status);
        break;
    default:
        // Pending/Preparing/Downloading：直接同步状态
        storage_->update_status(task_id, new_status);
        break;
    }
}

void TaskStorageListener::on_progress(const ProgressInfo& info) {
    if (info.task_id == INVALID_TASK_ID) return;

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || !storage_) return;

    // 同一任务的进度按时间节流落库；最新值无条件进内存缓存，
    // 终态/暂停转换时由 flush_progress_locked() 无条件补写
    last_progress_[info.task_id] = info;

    auto [it, inserted] = last_flush_.try_emplace(info.task_id, now);
    if (!inserted) {
        if (now - it->second < progress_flush_interval_) return;
        it->second = now;
    }

    storage_->update_progress(info.task_id, info.downloaded_bytes,
                              static_cast<double>(info.progress), info.speed,
                              info.total_bytes);
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
    last_progress_.clear();
    last_error_.clear();
}

}  // namespace falcon::daemon
