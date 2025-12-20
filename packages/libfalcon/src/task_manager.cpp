// Falcon Task Manager - Core Implementation
// Copyright (c) 2025 Falcon Project

#include "internal/task_manager.hpp"

namespace falcon {
namespace internal {

TaskManager::TaskManager(std::size_t max_concurrent)
    : max_concurrent_(max_concurrent) {}

TaskManager::~TaskManager() = default;

void TaskManager::add_task(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskId id = task->id();
    tasks_[id] = std::move(task);
    pending_queue_.push_back(id);
}

bool TaskManager::remove_task(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return false;
    }

    // Only remove finished tasks
    if (!it->second->is_finished()) {
        return false;
    }

    // Remove from pending queue if present
    pending_queue_.erase(
        std::remove(pending_queue_.begin(), pending_queue_.end(), id),
        pending_queue_.end());

    tasks_.erase(it);
    return true;
}

DownloadTask::Ptr TaskManager::get_task(TaskId id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return nullptr;
    }
    return it->second;
}

std::vector<DownloadTask::Ptr> TaskManager::get_all_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DownloadTask::Ptr> result;
    result.reserve(tasks_.size());
    for (const auto& pair : tasks_) {
        result.push_back(pair.second);
    }
    return result;
}

std::vector<DownloadTask::Ptr> TaskManager::get_tasks_by_status(
    TaskStatus status) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DownloadTask::Ptr> result;
    for (const auto& pair : tasks_) {
        if (pair.second->status() == status) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::vector<DownloadTask::Ptr> TaskManager::get_active_tasks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DownloadTask::Ptr> result;
    for (const auto& pair : tasks_) {
        if (pair.second->is_active()) {
            result.push_back(pair.second);
        }
    }
    return result;
}

std::size_t TaskManager::active_count() const {
    return active_count_.load();
}

std::size_t TaskManager::total_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void TaskManager::schedule_task(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return;
    }

    // Add to pending queue if not already there
    if (std::find(pending_queue_.begin(), pending_queue_.end(), id) ==
        pending_queue_.end()) {
        pending_queue_.push_back(id);
    }
}

DownloadTask::Ptr TaskManager::get_next_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);

    for (TaskId id : pending_queue_) {
        auto it = tasks_.find(id);
        if (it != tasks_.end() && it->second->status() == TaskStatus::Pending) {
            return it->second;
        }
    }

    return nullptr;
}

void TaskManager::set_max_concurrent(std::size_t max) {
    max_concurrent_.store(max);
}

std::size_t TaskManager::max_concurrent() const {
    return max_concurrent_.load();
}

bool TaskManager::can_start_more() const {
    return active_count_.load() < max_concurrent_.load();
}

std::size_t TaskManager::remove_finished() {
    std::lock_guard<std::mutex> lock(mutex_);

    std::size_t removed = 0;
    for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->second->is_finished()) {
            TaskId id = it->first;
            pending_queue_.erase(
                std::remove(pending_queue_.begin(), pending_queue_.end(), id),
                pending_queue_.end());
            it = tasks_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }

    return removed;
}

TaskId TaskManager::next_id() {
    return next_id_.fetch_add(1);
}

void TaskManager::wait_all() {
    std::unique_lock<std::mutex> lock(mutex_);

    cv_.wait(lock, [this] {
        for (const auto& pair : tasks_) {
            if (!pair.second->is_finished()) {
                return false;
            }
        }
        return true;
    });
}

}  // namespace internal
}  // namespace falcon
