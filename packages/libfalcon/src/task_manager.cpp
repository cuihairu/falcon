/**
 * @file task_manager.cpp
 * @brief 任务管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/protocol_handler.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace falcon {

/**
 * @brief 任务管理器实现类
 */
class TaskManager::Impl : public IEventListener {
public:
    Impl(const TaskManagerConfig& config, EventDispatcher* event_dispatcher)
        : config_(config)
        , event_dispatcher_(event_dispatcher)
        , next_task_id_(1)
        , running_(false) {}

    ~Impl() {
        stop();
    }

    void start() {
        if (running_) return;

        running_ = true;

        // 启动工作线程
        worker_thread_ = std::thread([this] { worker_loop(); });

        // 启动清理线程
        if (config_.cleanup_interval > std::chrono::seconds{0}) {
            cleanup_thread_ = std::thread([this] { cleanup_loop(); });
        }
    }

    void stop() {
        if (!running_) return;

        running_ = false;

        // 通知工作线程退出
        cv_.notify_all();

        // 等待工作线程退出
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }

        // 停止所有任务
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (auto& [id, task] : tasks_) {
            if (task->is_active()) {
                task->cancel();
            }
        }
    }

    TaskId add_task(DownloadTask::Ptr task, TaskPriority priority) {
        if (!task) return INVALID_TASK_ID;

        std::lock_guard<std::mutex> lock(tasks_mutex_);

        TaskId id = next_task_id_++;
        task->set_listener(this);
        tasks_[id] = std::move(task);

        // 不自动添加到队列，等待 start_task 调用

        // 如果启用了自动保存，保存状态
        if (config_.auto_save_state && !config_.state_file.empty()) {
            save_state_async();
        }

        return id;
    }

    DownloadTask::Ptr get_task(TaskId id) const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        auto it = tasks_.find(id);
        return it != tasks_.end() ? it->second : nullptr;
    }

    std::vector<DownloadTask::Ptr> get_all_tasks() const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        std::vector<DownloadTask::Ptr> result;
        result.reserve(tasks_.size());

        for (const auto& [id, task] : tasks_) {
            result.push_back(task);
        }

        return result;
    }

    std::vector<DownloadTask::Ptr> get_tasks_by_status(TaskStatus status) const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        std::vector<DownloadTask::Ptr> result;

        for (const auto& [id, task] : tasks_) {
            if (task->status() == status) {
                result.push_back(task);
            }
        }

        return result;
    }

    std::vector<DownloadTask::Ptr> get_active_tasks() const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        std::vector<DownloadTask::Ptr> result;

        for (const auto& [id, task] : tasks_) {
            if (task->is_active()) {
                result.push_back(task);
            }
        }

        return result;
    }

    bool start_task(TaskId id) {
        auto task = get_task(id);
        if (!task || task->is_active() || task->is_finished()) {
            return false;
        }

        // 添加到优先级队列
        std::lock_guard<std::mutex> lock(queue_mutex_);
        TaskQueueItem item{id, TaskPriority::Normal, std::chrono::steady_clock::now()};
        task_queue_.push(item);

        // 通知工作线程
        cv_.notify_one();

        return true;
    }

    bool start_task(TaskId id, TaskPriority priority) {
        auto task = get_task(id);
        if (!task || task->is_active() || task->is_finished()) {
            return false;
        }

        // 添加到优先级队列
        std::lock_guard<std::mutex> lock(queue_mutex_);
        TaskQueueItem item{id, priority, std::chrono::steady_clock::now()};
        task_queue_.push(item);

        // 通知工作线程
        cv_.notify_one();

        return true;
    }

    bool remove_task(TaskId id) {
        std::lock_guard<std::mutex> lock(tasks_mutex_);

        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            return false;
        }

        // 只能移除已完成的任务
        if (!it->second->is_finished()) {
            return false;
        }

        tasks_.erase(it);

        // 如果启用了自动保存，保存状态
        if (config_.auto_save_state && !config_.state_file.empty()) {
            save_state_async();
        }

        return true;
    }

    size_t cleanup_finished_tasks() {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        size_t count = 0;

        for (auto it = tasks_.begin(); it != tasks_.end();) {
            if (it->second->is_finished()) {
                it = tasks_.erase(it);
                ++count;
            } else {
                ++it;
            }
        }

        return count;
    }

    bool start_task(TaskId id) {
        auto task = get_task(id);
        if (!task) return false;

        return resume_task(id);
    }

    bool pause_task(TaskId id) {
        auto task = get_task(id);
        if (!task) return false;

        // 从活动任务中移除
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.erase(id);
        }

        // 暂停任务
        task->pause();

        // 通知等待队列
        cv_.notify_one();

        return true;
    }

    bool resume_task(TaskId id) {
        auto task = get_task(id);
        if (!task) return false;

        // 添加到等待队列
        TaskQueueItem item{id, TaskPriority::Normal, std::chrono::steady_clock::now()};
        task_queue_.push(item);

        // 通知工作线程
        cv_.notify_one();

        return true;
    }

    bool cancel_task(TaskId id) {
        auto task = get_task(id);
        if (!task) return false;

        // 从活动任务中移除
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.erase(id);
        }

        // 取消任务
        task->cancel();

        return true;
    }

    void pause_all() {
        auto active_tasks = get_active_tasks();
        for (auto& task : active_tasks) {
            pause_task(task->id());
        }
    }

    void resume_all() {
        auto paused_tasks = get_tasks_by_status(TaskStatus::Paused);
        for (auto& task : paused_tasks) {
            resume_task(task->id());
        }
    }

    void cancel_all() {
        auto tasks = get_all_tasks();
        for (auto& task : tasks) {
            cancel_task(task->id());
        }
    }

    bool wait_all(std::chrono::milliseconds timeout) {
        auto start_time = std::chrono::steady_clock::now();

        while (true) {
            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                bool all_finished = std::all_of(
                    tasks_.begin(), tasks_.end(),
                    [](const auto& pair) { return pair.second->is_finished(); });

                if (all_finished) return true;
            }

            if (timeout != std::chrono::milliseconds::max()) {
                auto elapsed = std::chrono::steady_clock::now() - start_time;
                if (elapsed >= timeout) return false;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    size_t get_queue_size() const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return task_queue_.size();
    }

    size_t get_active_task_count() const {
        std::lock_guard<std::mutex> lock(active_mutex_);
        return active_tasks_.size();
    }

    size_t get_max_concurrent_tasks() const {
        return config_.max_concurrent_tasks;
    }

    void set_max_concurrent_tasks(size_t max_tasks) {
        config_.max_concurrent_tasks = max_tasks;
    }

    bool adjust_task_priority(TaskId id, TaskPriority priority) {
        // 需要找到并调整任务在队列中的优先级
        // 这里简化实现，实际可能需要更复杂的优先级队列
        return false;
    }

    TaskManager::Statistics get_statistics() const {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        TaskManager::Statistics stats;

        stats.total_tasks = tasks_.size();

        for (const auto& [id, task] : tasks_) {
            switch (task->status()) {
                case TaskStatus::Pending:
                    stats.pending_tasks++;
                    break;
                case TaskStatus::Preparing:
                case TaskStatus::Downloading:
                    stats.downloading_tasks++;
                    stats.total_speed += task->speed();
                    break;
                case TaskStatus::Paused:
                    stats.paused_tasks++;
                    break;
                case TaskStatus::Completed:
                    stats.completed_tasks++;
                    stats.total_downloaded += task->downloaded_bytes();
                    break;
                case TaskStatus::Failed:
                    stats.failed_tasks++;
                    break;
                case TaskStatus::Cancelled:
                    stats.cancelled_tasks++;
                    break;
            }
        }

        return stats;
    }

    bool save_state(const std::string& file_path) {
        std::ofstream file(file_path);
        if (!file.is_open()) {
            return false;
        }

        // 保存任务状态
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (const auto& [id, task] : tasks_) {
            // TODO: 实现任务状态序列化
            file << id << " " << static_cast<int>(task->status()) << "\n";
        }

        return true;
    }

    bool load_state(const std::string& file_path) {
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return false;
        }

        // TODO: 实现任务状态反序列化
        return true;
    }

    void set_state_file(const std::string& file_path) {
        config_.state_file = file_path;
    }

    bool is_running() const {
        return running_;
    }

    // IEventListener 实现
    void on_status_changed(TaskId task_id, TaskStatus old_status, TaskStatus new_status) {
        // 更新活动任务列表
        if (new_status == TaskStatus::Downloading) {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.insert(task_id);
        } else {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.erase(task_id);
        }

        // 分发事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_status_changed(task_id, old_status, new_status);
        }

        // 任务完成后通知等待队列
        if (new_status == TaskStatus::Completed || new_status == TaskStatus::Failed ||
            new_status == TaskStatus::Cancelled) {
            cv_.notify_one();
        }
    }

    void on_progress(TaskId task_id, const ProgressInfo& progress) {
        // 分发进度事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_progress(task_id, progress);
        }
    }

    void on_error(TaskId task_id, const std::string& error_message) {
        // 分发错误事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_error(task_id, error_message);
        }
    }

    void on_completed(TaskId task_id, const std::string& output_path) {
        // 分发完成事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_completed(task_id, output_path, 0, Duration{0});
        }
    }

    void on_file_info(TaskId task_id, const FileInfo& info) {
        // 分发文件信息事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_file_info(task_id, info);
        }
    }

private:
    void worker_loop() {
        while (running_) {
            TaskQueueItem item;
            DownloadTask::Ptr task;

            // 等待任务
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                cv_.wait(lock, [this] {
                    return !task_queue_.empty() || !running_;
                });

                if (!running_) break;

                // 检查并发限制
                if (get_active_task_count() >= config_.max_concurrent_tasks) {
                    continue;
                }

                // 获取任务
                item = task_queue_.top();
                task_queue_.pop();
            }

            // 获取任务对象
            task = get_task(item.task_id);
            if (!task || task->is_finished()) {
                continue;
            }

            // 启动任务
            try {
                // 添加到活动任务列表
                {
                    std::lock_guard<std::mutex> lock(active_mutex_);
                    active_tasks_.insert(item.task_id);
                }

                // 设置任务状态
                task->set_status(TaskStatus::Downloading);

                // 获取协议处理器并开始下载
                auto handler = task->get_handler();
                if (handler) {
                    // 使用当前对象（实现了 IEventListener）作为监听器
                    handler->download(task, this);
                } else {
                    throw std::runtime_error("No protocol handler available for task");
                }

            } catch (const std::exception& e) {
                task->set_status(TaskStatus::Failed);
                on_error(item.task_id, e.what());
            }
        }
    }

    void cleanup_loop() {
        while (running_) {
            std::this_thread::sleep_for(config_.cleanup_interval);

            if (!running_) break;

            // 清理完成的任务
            cleanup_finished_tasks();

            // 保存状态
            if (config_.auto_save_state && !config_.state_file.empty()) {
                save_state_async();
            }
        }
    }

    void save_state_async() {
        // 在新线程中保存状态，避免阻塞主循环
        std::thread([this] {
            if (!config_.state_file.empty()) {
                save_state(config_.state_file);
            }
        }).detach();
    }

    TaskManagerConfig config_;
    EventDispatcher* event_dispatcher_;

    // 任务存储
    mutable std::mutex tasks_mutex_;
    std::unordered_map<TaskId, DownloadTask::Ptr> tasks_;
    TaskId next_task_id_;

    // 任务队列
    mutable std::mutex queue_mutex_;
    std::priority_queue<TaskQueueItem> task_queue_;
    std::condition_variable cv_;

    // 活动任务
    mutable std::mutex active_mutex_;
    std::unordered_set<TaskId> active_tasks_;

    // 工作线程
    std::atomic<bool> running_;
    std::thread worker_thread_;
    std::thread cleanup_thread_;
};

// TaskManager 实现
TaskManager::TaskManager(const TaskManagerConfig& config, EventDispatcher* event_dispatcher)
    : impl_(std::make_unique<Impl>(config, event_dispatcher)) {}

TaskManager::~TaskManager() = default;

TaskId TaskManager::add_task(DownloadTask::Ptr task, TaskPriority priority) {
    return impl_->add_task(std::move(task), priority);
}

DownloadTask::Ptr TaskManager::get_task(TaskId id) const {
    return impl_->get_task(id);
}

std::vector<DownloadTask::Ptr> TaskManager::get_all_tasks() const {
    return impl_->get_all_tasks();
}

std::vector<DownloadTask::Ptr> TaskManager::get_tasks_by_status(TaskStatus status) const {
    return impl_->get_tasks_by_status(status);
}

std::vector<DownloadTask::Ptr> TaskManager::get_active_tasks() const {
    return impl_->get_active_tasks();
}

bool TaskManager::remove_task(TaskId id) {
    return impl_->remove_task(id);
}

size_t TaskManager::cleanup_finished_tasks() {
    return impl_->cleanup_finished_tasks();
}

bool TaskManager::start_task(TaskId id) {
    return impl_->start_task(id);
}

bool TaskManager::pause_task(TaskId id) {
    return impl_->pause_task(id);
}

bool TaskManager::resume_task(TaskId id) {
    return impl_->resume_task(id);
}

bool TaskManager::cancel_task(TaskId id) {
    return impl_->cancel_task(id);
}

void TaskManager::pause_all() {
    impl_->pause_all();
}

void TaskManager::resume_all() {
    impl_->resume_all();
}

void TaskManager::cancel_all() {
    impl_->cancel_all();
}

bool TaskManager::wait_all(std::chrono::milliseconds timeout) {
    return impl_->wait_all(timeout);
}

size_t TaskManager::get_queue_size() const {
    return impl_->get_queue_size();
}

size_t TaskManager::get_active_task_count() const {
    return impl_->get_active_task_count();
}

size_t TaskManager::get_max_concurrent_tasks() const {
    return impl_->get_max_concurrent_tasks();
}

void TaskManager::set_max_concurrent_tasks(size_t max_tasks) {
    impl_->set_max_concurrent_tasks(max_tasks);
}

bool TaskManager::adjust_task_priority(TaskId id, TaskPriority priority) {
    return impl_->adjust_task_priority(id, priority);
}

TaskManager::Statistics TaskManager::get_statistics() const {
    return impl_->get_statistics();
}

bool TaskManager::save_state(const std::string& file_path) {
    return impl_->save_state(file_path);
}

bool TaskManager::load_state(const std::string& file_path) {
    return impl_->load_state(file_path);
}

void TaskManager::set_state_file(const std::string& file_path) {
    impl_->set_state_file(file_path);
}

void TaskManager::start() {
    impl_->start();
}

void TaskManager::stop() {
    impl_->stop();
}

bool TaskManager::is_running() const {
    return impl_->is_running();
}

void TaskManager::on_task_status_changed(TaskId task_id,
                                         TaskStatus old_status,
                                         TaskStatus new_status) {
    impl_->on_status_changed(task_id, old_status, new_status);
}

void TaskManager::on_task_progress(TaskId task_id, const ProgressInfo& progress) {
    impl_->on_progress(task_id, progress);
}

} // namespace falcon