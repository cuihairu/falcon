/**
 * @file task_manager.cpp
 * @brief 任务管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/protocol_handler.hpp>
#include "internal/thread_pool.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <unordered_set>

namespace falcon {

namespace {
constexpr int kTaskManagerStateVersion = 1;
constexpr const char* kTaskManagerStateMagic = "falcon_task_state";

template <typename Int>
bool read_int(std::istream& in, Int& out) {
    long long value = 0;
    if (!(in >> value)) {
        return false;
    }
    out = static_cast<Int>(value);
    return true;
}

TaskStatus sanitize_status_for_restore(TaskStatus status) {
    if (status == TaskStatus::Downloading || status == TaskStatus::Preparing) {
        return TaskStatus::Paused;
    }
    return status;
}
} // namespace

/**
 * @brief 任务管理器实现类
 */
class TaskManager::Impl : public IEventListener {
public:
    Impl(const TaskManagerConfig& config, EventDispatcher* event_dispatcher)
        : config_(config)
        , event_dispatcher_(event_dispatcher)
        , running_(false)
        , state_pool_(1)
        , download_pool_(0) {}

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
        cleanup_cv_.notify_all();

        // 等待工作线程退出
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }

        if (cleanup_thread_.joinable()) {
            cleanup_thread_.join();
        }

        // 停止所有任务
        std::vector<DownloadTask::Ptr> to_cancel;
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            for (auto& [id, task] : tasks_) {
                if (task->is_active()) {
                    to_cancel.push_back(task);
                }
            }
        }
        for (auto& task : to_cancel) {
            task->cancel();
        }

        // Flush pending state saves to avoid dangling async work
        state_pool_.wait();
    }

    TaskId add_task(DownloadTask::Ptr task, TaskPriority /*priority*/) {
        if (!task) return INVALID_TASK_ID;

        std::lock_guard<std::mutex> lock(tasks_mutex_);

        TaskId id = task->id();
        if (id == INVALID_TASK_ID) {
            return INVALID_TASK_ID;
        }
        if (tasks_.find(id) != tasks_.end()) {
            return INVALID_TASK_ID;
        }
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
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            TaskQueueItem item{id, TaskPriority::Normal, std::chrono::steady_clock::now()};
            task_queue_.push(item);
        }

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
        auto all_tasks = get_all_tasks();
        for (auto& task : all_tasks) {
            if (task->status() == TaskStatus::Pending || task->status() == TaskStatus::Downloading ||
                task->status() == TaskStatus::Preparing) {
                pause_task(task->id());
            }
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
        config_.max_concurrent_tasks = std::max<size_t>(1, max_tasks);
        cv_.notify_one();
    }

    bool adjust_task_priority(TaskId /*id*/, TaskPriority /*priority*/) {
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
        std::ofstream file(file_path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }

        file << kTaskManagerStateMagic << " " << kTaskManagerStateVersion << "\n";

        // 保存任务状态
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        for (const auto& [id, task] : tasks_) {
            const auto& options = task->options();

            file << "task "
                 << id << " "
                 << static_cast<int>(task->status()) << " "
                 << task->downloaded_bytes() << " "
                 << task->total_bytes() << " "
                 << std::quoted(task->url()) << " "
                 << std::quoted(task->output_path()) << " "
                 << std::quoted(task->error_message()) << " "
                 << options.max_connections << " "
                 << options.timeout_seconds << " "
                 << options.max_retries << " "
                 << options.retry_delay_seconds << " "
                 << std::quoted(options.output_directory) << " "
                 << std::quoted(options.output_filename) << " "
                 << options.speed_limit << " "
                 << (options.resume_enabled ? 1 : 0) << " "
                 << std::quoted(options.user_agent) << " "
                 << std::quoted(options.proxy) << " "
                 << std::quoted(options.proxy_type) << " "
                 << (options.verify_ssl ? 1 : 0) << " "
                 << std::quoted(options.referer) << " "
                 << std::quoted(options.cookie_file) << " "
                 << std::quoted(options.cookie_jar) << " "
                 << options.min_segment_size << " "
                 << (options.adaptive_segment_sizing ? 1 : 0) << " "
                 << options.progress_interval_ms << " "
                 << (options.create_directory ? 1 : 0) << " "
                 << (options.overwrite_existing ? 1 : 0) << " "
                 << options.headers.size();

            for (const auto& [k, v] : options.headers) {
                file << " " << std::quoted(k) << " " << std::quoted(v);
            }
            file << "\n";
        }

        return true;
    }

    bool load_state(const std::string& file_path) {
        std::ifstream file(file_path, std::ios::binary);
        if (!file.is_open()) {
            return false;
        }

        std::string magic;
        if (!(file >> magic)) {
            return false;
        }

        if (magic != kTaskManagerStateMagic) {
            // Legacy format: "<id> <status>"
            file.clear();
            file.seekg(0);
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty()) continue;
                std::istringstream in(line);
                TaskId id = INVALID_TASK_ID;
                int status_int = 0;
                if (!(in >> id >> status_int)) {
                    continue;
                }
                auto task = get_task(id);
                if (!task) {
                    continue;
                }
                if (status_int < 0 || status_int > 6) {
                    continue;
                }
                task->set_status(static_cast<TaskStatus>(status_int));
            }
            return true;
        }

        int version = 0;
        if (!(file >> version) || version != kTaskManagerStateVersion) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            task_queue_ = std::priority_queue<TaskQueueItem>{};
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.clear();
            active_count_.store(0);
        }

        std::string line;
        std::getline(file, line); // consume remainder of header line
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            std::istringstream in(line);
            std::string tag;
            if (!(in >> tag) || tag != "task") {
                continue;
            }

            TaskId id = INVALID_TASK_ID;
            int status_int = 0;
            Bytes downloaded_bytes = 0;
            Bytes total_bytes = 0;
            std::string url;
            std::string output_path;
            std::string error_message;

            if (!(in >> id >> status_int >> downloaded_bytes >> total_bytes)) {
                continue;
            }
            if (!(in >> std::quoted(url) >> std::quoted(output_path) >> std::quoted(error_message))) {
                continue;
            }

            DownloadOptions options;

            if (!(in >> options.max_connections >> options.timeout_seconds >> options.max_retries >>
                  options.retry_delay_seconds)) {
                continue;
            }
            if (!(in >> std::quoted(options.output_directory) >> std::quoted(options.output_filename))) {
                continue;
            }
            if (!(in >> options.speed_limit)) {
                continue;
            }

            int resume_enabled = 1;
            if (!read_int(in, resume_enabled)) {
                continue;
            }
            options.resume_enabled = resume_enabled != 0;

            if (!(in >> std::quoted(options.user_agent) >> std::quoted(options.proxy) >>
                  std::quoted(options.proxy_type))) {
                continue;
            }

            int verify_ssl = 1;
            if (!read_int(in, verify_ssl)) {
                continue;
            }
            options.verify_ssl = verify_ssl != 0;

            if (!(in >> std::quoted(options.referer) >> std::quoted(options.cookie_file) >>
                  std::quoted(options.cookie_jar))) {
                continue;
            }

            if (!(in >> options.min_segment_size)) {
                continue;
            }

            int adaptive = 1;
            if (!read_int(in, adaptive)) {
                continue;
            }
            options.adaptive_segment_sizing = adaptive != 0;

            if (!(in >> options.progress_interval_ms)) {
                continue;
            }

            int create_dir = 1;
            int overwrite = 0;
            if (!read_int(in, create_dir) || !read_int(in, overwrite)) {
                continue;
            }
            options.create_directory = create_dir != 0;
            options.overwrite_existing = overwrite != 0;

            std::size_t header_count = 0;
            if (!(in >> header_count)) {
                continue;
            }
            for (std::size_t i = 0; i < header_count; ++i) {
                std::string k;
                std::string v;
                if (!(in >> std::quoted(k) >> std::quoted(v))) {
                    break;
                }
                options.headers.emplace(std::move(k), std::move(v));
            }

            if (id == INVALID_TASK_ID || url.empty()) {
                continue;
            }

            TaskStatus status = TaskStatus::Pending;
            if (status_int >= 0 && status_int <= 6) {
                status = static_cast<TaskStatus>(status_int);
            }
            status = sanitize_status_for_restore(status);

            auto task = std::make_shared<DownloadTask>(id, url, options);
            task->set_output_path(output_path);
            if (!error_message.empty()) {
                task->set_error(error_message);
            }
            task->update_progress(downloaded_bytes, total_bytes, 0);
            task->set_status(status);

            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                if (tasks_.find(id) != tasks_.end()) {
                    continue;
                }
                task->set_listener(this);
                tasks_.emplace(id, std::move(task));
            }
        }

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
        auto is_active_status = [](TaskStatus s) {
            return s == TaskStatus::Downloading || s == TaskStatus::Preparing;
        };

        // 更新活动任务列表
        if (is_active_status(new_status) && !is_active_status(old_status)) {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.insert(task_id);
            active_count_.fetch_add(1);
        } else if (!is_active_status(new_status) && is_active_status(old_status)) {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.erase(task_id);
            active_count_.fetch_sub(1);
        }

        // 分发事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_status_changed(task_id, old_status, new_status);
        }

        // 任务完成时分发完成事件
        if (new_status == TaskStatus::Completed && event_dispatcher_) {
            auto task = get_task(task_id);
            if (task) {
                event_dispatcher_->dispatch_completed(
                    task_id,
                    task->output_path(),
                    task->total_bytes(),
                    task->elapsed()
                );
            }
        }

        // 任务完成后通知等待队列
        if (new_status == TaskStatus::Completed || new_status == TaskStatus::Failed ||
            new_status == TaskStatus::Cancelled) {
            cv_.notify_one();
        }
    }

    void on_progress(const ProgressInfo& progress) {
        // 分发进度事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_progress(progress.task_id, progress);
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
                    return !running_ ||
                           (!task_queue_.empty() &&
                            active_count_.load() < config_.max_concurrent_tasks);
                });

                if (!running_) break;

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
            task->mark_started();
            task->set_status(TaskStatus::Downloading);

            try {
                download_pool_.submit([this, task]() {
                    try {
                        auto handler = task->get_handler();
                        if (handler) {
                            handler->download(task, this);
                        } else {
                            throw std::runtime_error("No protocol handler available for task");
                        }
                    } catch (const std::exception& e) {
                        task->set_error(e.what());
                        task->set_status(TaskStatus::Failed);
                    }
                });
            } catch (const std::exception& e) {
                task->set_error(e.what());
                task->set_status(TaskStatus::Failed);
            }
        }
    }

    void cleanup_loop() {
        while (running_) {
            {
                std::unique_lock<std::mutex> lock(cleanup_mutex_);
                cleanup_cv_.wait_for(lock, config_.cleanup_interval, [this] { return !running_; });
            }
            if (!running_) {
                break;
            }

            // 清理完成的任务
            cleanup_finished_tasks();

            // 保存状态
            if (config_.auto_save_state && !config_.state_file.empty()) {
                save_state_async();
            }
        }
    }

    void save_state_async() {
        if (config_.state_file.empty()) {
            return;
        }

        bool expected = false;
        if (!state_save_scheduled_.compare_exchange_strong(expected, true)) {
            return;
        }

        const std::string path = config_.state_file;
        state_pool_.submit([this, path] {
            save_state(path);
            state_save_scheduled_.store(false);
        });
    }

    TaskManagerConfig config_;
    EventDispatcher* event_dispatcher_;
    std::atomic<size_t> active_count_{0};

    // 任务存储
    mutable std::mutex tasks_mutex_;
    std::unordered_map<TaskId, DownloadTask::Ptr> tasks_;

    // 任务队列
    mutable std::mutex queue_mutex_;
    std::priority_queue<TaskQueueItem> task_queue_;
    std::condition_variable cv_;

    // 清理线程唤醒
    mutable std::mutex cleanup_mutex_;
    std::condition_variable cleanup_cv_;

    // 活动任务
    mutable std::mutex active_mutex_;
    std::unordered_set<TaskId> active_tasks_;

    // 工作线程
    std::atomic<bool> running_;
    std::thread worker_thread_;
    std::thread cleanup_thread_;

    // Async state saving (must be destroyed before other members)
    std::atomic<bool> state_save_scheduled_{false};
    internal::ThreadPool state_pool_;
    internal::ThreadPool download_pool_;
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

void TaskManager::on_task_progress(TaskId /*task_id*/, const ProgressInfo& progress) {
    impl_->on_progress(progress);
}

bool TaskManager::start_task(TaskId id) {
    return impl_->start_task(id, TaskPriority::Normal);
}

bool TaskManager::start_task(TaskId id, TaskPriority priority) {
    return impl_->start_task(id, priority);
}

} // namespace falcon
