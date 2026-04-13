/**
 * @file event_dispatcher.cpp
 * @brief 事件分发器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/event_dispatcher.hpp>
#include <algorithm>
#include <stdexcept>

namespace falcon {

/**
 * @brief 事件分发器实现类
 */
class EventDispatcher::Impl {
public:
    explicit Impl(const EventDispatcherConfig& config)
        : config_(config)
        , running_(false)
        , processed_count_(0)
        , dropped_count_(0) {}

    ~Impl() {
        stop(false);
    }

    void start() {
        if (running_) return;

        running_ = true;

        if (config_.enable_async_dispatch) {
            // 启动工作线程
            for (size_t i = 0; i < config_.thread_pool_size; ++i) {
                workers_.emplace_back([this] { worker_loop(); });
            }
        }
    }

    void stop(bool wait_for_completion) {
        if (!running_) return;

        running_ = false;
        cv_.notify_all();

        // 等待所有工作线程退出（running_ 置为 false 后不会再处理队列）
        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers_.clear();

        if (wait_for_completion) {
            // 同步清空队列，避免遗留事件导致 stop(true) 卡住
            while (true) {
                std::shared_ptr<EventData> event;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex_);
                    if (event_queue_.empty()) break;
                    event = event_queue_.front();
                    event_queue_.pop();
                }
                if (event) {
                    process_event(event);
                    ++processed_count_;
                }
            }
        }
    }

    bool dispatch(std::shared_ptr<EventData> event) {
        if (!event) return false;

        if (!config_.enable_async_dispatch) {
            // 同步模式下立即处理（不入队）
            process_event(event);
            ++processed_count_;
        } else {
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                // 检查队列大小
                if (event_queue_.size() >= config_.max_queue_size) {
                    ++dropped_count_;
                    return false;
                }

                event_queue_.push(event);
            }
            // 异步模式下通知工作线程
            cv_.notify_one();
        }

        return true;
    }

    void dispatch_sync(std::shared_ptr<EventData> event) {
        if (!event) return;
        process_event(event);
        ++processed_count_;
    }

    void add_listener(IEventListener* listener) {
        if (!listener) return;

        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.push_back(listener);
    }

    void remove_listener(IEventListener* listener) {
        if (!listener) return;

        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.erase(
            std::remove(listeners_.begin(), listeners_.end(), listener),
            listeners_.end()
        );
    }

    void clear_listeners() {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        listeners_.clear();
    }

    size_t get_listener_count() const {
        std::lock_guard<std::mutex> lock(listeners_mutex_);
        return listeners_.size();
    }

    size_t get_queue_size() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return event_queue_.size();
    }

    uint64_t get_processed_count() const {
        return processed_count_;
    }

    uint64_t get_dropped_count() const {
        return dropped_count_;
    }

    bool is_running() const {
        return running_;
    }

private:
    void worker_loop() {
        while (running_) {
            std::shared_ptr<EventData> event;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                cv_.wait(lock, [this] { return !event_queue_.empty() || !running_; });

                if (!running_) break;

                if (!event_queue_.empty()) {
                    event = event_queue_.front();
                    event_queue_.pop();
                }
            }

            if (event) {
                process_event(event);
                ++processed_count_;
            }
        }
    }

    void process_event(std::shared_ptr<EventData> event) {
        if (!event) return;

        std::lock_guard<std::mutex> lock(listeners_mutex_);

        for (auto* listener : listeners_) {
            if (!listener) continue;

            switch (event->type) {
                case EventType::StatusChanged: {
                    auto* e = static_cast<StatusChangedEvent*>(event.get());
                    listener->on_status_changed(e->task_id, e->old_status, e->new_status);
                    break;
                }
                case EventType::Progress: {
                    auto* e = static_cast<ProgressEvent*>(event.get());
                    listener->on_progress(e->progress);
                    break;
                }
                case EventType::Error: {
                    auto* e = static_cast<ErrorEvent*>(event.get());
                    listener->on_error(e->task_id, e->error_message);
                    break;
                }
                case EventType::Completed: {
                    auto* e = static_cast<CompletedEvent*>(event.get());
                    listener->on_completed(e->task_id, e->output_path);
                    break;
                }
                case EventType::FileInfo: {
                    auto* e = static_cast<FileInfoEvent*>(event.get());
                    listener->on_file_info(e->task_id, e->file_info);
                    break;
                }
                default:
                    // 自定义事件暂不处理
                    break;
            }
        }
    }

    EventDispatcherConfig config_;
    std::atomic<bool> running_;

    // 事件队列
    mutable std::mutex queue_mutex_;
    std::queue<std::shared_ptr<EventData>> event_queue_;
    std::condition_variable cv_;

    // 工作线程
    std::vector<std::thread> workers_;

    // 监听器列表
    mutable std::mutex listeners_mutex_;
    std::vector<IEventListener*> listeners_;

    // 统计信息
    std::atomic<uint64_t> processed_count_;
    std::atomic<uint64_t> dropped_count_;
};

// EventDispatcher 实现
EventDispatcher::EventDispatcher(const EventDispatcherConfig& config)
    : impl_(std::make_unique<Impl>(config)) {}

EventDispatcher::~EventDispatcher() = default;

void EventDispatcher::add_listener(IEventListener* listener) {
    impl_->add_listener(listener);
}

void EventDispatcher::remove_listener(IEventListener* listener) {
    impl_->remove_listener(listener);
}

void EventDispatcher::clear_listeners() {
    impl_->clear_listeners();
}

size_t EventDispatcher::get_listener_count() const {
    return impl_->get_listener_count();
}

bool EventDispatcher::dispatch(std::shared_ptr<EventData> event) {
    return impl_->dispatch(event);
}

void EventDispatcher::dispatch_sync(std::shared_ptr<EventData> event) {
    impl_->dispatch_sync(event);
}

void EventDispatcher::dispatch_status_changed(TaskId task_id,
                                              TaskStatus old_status,
                                              TaskStatus new_status) {
    auto event = std::make_shared<StatusChangedEvent>(task_id, old_status, new_status);
    dispatch(event);
}

void EventDispatcher::dispatch_progress(TaskId task_id, const ProgressInfo& progress) {
    auto event = std::make_shared<ProgressEvent>(task_id, progress);
    dispatch(event);
}

void EventDispatcher::dispatch_error(TaskId task_id, const std::string& error_message) {
    auto event = std::make_shared<ErrorEvent>(task_id, error_message);
    dispatch(event);
}

void EventDispatcher::dispatch_completed(TaskId task_id,
                                         const std::string& output_path,
                                         Bytes total_size,
                                         Duration duration) {
    auto event = std::make_shared<CompletedEvent>(task_id, output_path, total_size, duration);
    dispatch(event);
}

void EventDispatcher::dispatch_file_info(TaskId task_id, const FileInfo& info) {
    auto event = std::make_shared<FileInfoEvent>(task_id, info);
    dispatch(event);
}

void EventDispatcher::dispatch_custom(const std::string& event_name,
                                      const std::string& data,
                                      TaskId task_id) {
    auto event = std::make_shared<CustomEvent>(event_name, data, task_id);
    dispatch(event);
}

void EventDispatcher::start() {
    impl_->start();
}

void EventDispatcher::stop(bool wait_for_completion) {
    impl_->stop(wait_for_completion);
}

bool EventDispatcher::is_running() const {
    return impl_->is_running();
}

size_t EventDispatcher::get_queue_size() const {
    return impl_->get_queue_size();
}

uint64_t EventDispatcher::get_processed_count() const {
    return impl_->get_processed_count();
}

uint64_t EventDispatcher::get_dropped_count() const {
    return impl_->get_dropped_count();
}

} // namespace falcon
