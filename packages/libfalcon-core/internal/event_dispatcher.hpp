#pragma once

#include <falcon/event_listener.hpp>

#include <algorithm>
#include <mutex>
#include <vector>

namespace falcon {
namespace internal {

/// Event dispatcher for broadcasting events to multiple listeners
class EventDispatcher : public IEventListener {
public:
    EventDispatcher() = default;
    ~EventDispatcher() override = default;

    /// Add a listener
    void add_listener(IEventListener* listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(listeners_.begin(), listeners_.end(), listener) ==
            listeners_.end()) {
            listeners_.push_back(listener);
        }
    }

    /// Remove a listener
    void remove_listener(IEventListener* listener) {
        std::lock_guard<std::mutex> lock(mutex_);
        listeners_.erase(
            std::remove(listeners_.begin(), listeners_.end(), listener),
            listeners_.end());
    }

    /// Get listener count
    [[nodiscard]] std::size_t listener_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return listeners_.size();
    }

    // IEventListener implementation - broadcasts to all listeners

    void on_status_changed(TaskId task_id, TaskStatus old_status,
                           TaskStatus new_status) override {
        std::vector<IEventListener*> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = listeners_;
        }
        for (auto* listener : copy) {
            listener->on_status_changed(task_id, old_status, new_status);
        }
    }

    void on_progress(const ProgressInfo& info) override {
        std::vector<IEventListener*> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = listeners_;
        }
        for (auto* listener : copy) {
            listener->on_progress(info);
        }
    }

    void on_error(TaskId task_id, const std::string& error_message) override {
        std::vector<IEventListener*> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = listeners_;
        }
        for (auto* listener : copy) {
            listener->on_error(task_id, error_message);
        }
    }

    void on_completed(TaskId task_id, const std::string& output_path) override {
        std::vector<IEventListener*> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = listeners_;
        }
        for (auto* listener : copy) {
            listener->on_completed(task_id, output_path);
        }
    }

    void on_file_info(TaskId task_id, const FileInfo& info) override {
        std::vector<IEventListener*> copy;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            copy = listeners_;
        }
        for (auto* listener : copy) {
            listener->on_file_info(task_id, info);
        }
    }

private:
    mutable std::mutex mutex_;
    std::vector<IEventListener*> listeners_;
};

}  // namespace internal
}  // namespace falcon
