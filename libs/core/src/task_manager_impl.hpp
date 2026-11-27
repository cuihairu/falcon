#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>
#include <falcon/protocol_handler.hpp>
#include "internal/thread_pool.hpp"
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <utility>
#include <unordered_set>
#include <unordered_map>

#include "task_manager_persist_helpers.hpp"

namespace falcon {

/**
 * @brief 任务管理器实现类
 */
class TaskManager::Impl : public IEventListener {
public:
    Impl(const TaskManagerConfig& config, EventDispatcher* event_dispatcher)
        : config_(config)
        , event_dispatcher_(event_dispatcher)
        , max_concurrent_tasks_(std::max<size_t>(1, config.max_concurrent_tasks))
        , running_(false)
        , state_pool_(1)
        , download_pool_(0) {}

    ~Impl() {
        stop();
        unbind_all_task_listeners();
    }

    /// 注入 this 作为任务 listener，并登记弱引用。cleanup_finished_tasks
    /// 会把终态任务移出 tasks_，但宿主可能通过 get_task/get_all_tasks 仍
    /// 持有其 Ptr；因此需独立跟踪所有绑定过的任务，析构时逐一解绑。
    void bind_task_listener(const DownloadTask::Ptr& task) {
        task->set_listener(this);
        std::lock_guard<std::mutex> lock(listener_registry_mutex_);
        listener_bound_tasks_.push_back(task);
    }

    /// 解绑所有任务的 listener 回调：宿主保留的 DownloadTask::Ptr 可能活得
    /// 比 TaskManager 久，若 listener_ 仍指向已析构的 Impl，后续
    /// set_status/set_error 会造成悬垂调用。stop() 已 join 全部工作线程并
    /// 排空线程池，此处不会再有并发回调。
    void unbind_all_task_listeners() {
        std::lock_guard<std::mutex> lock(listener_registry_mutex_);
        for (auto& weak_task : listener_bound_tasks_) {
            if (auto task = weak_task.lock()) {
                task->set_listener(nullptr);
            }
        }
        listener_bound_tasks_.clear();
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

        // Stop active downloads before tearing down scheduler threads so the
        // background download pool can drain promptly during shutdown.
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

        // Flush pending state saves to avoid dangling async work
        state_pool_.wait();
        download_pool_.wait();
    }

    TaskId add_task(DownloadTask::Ptr task, TaskPriority priority) {
        if (!task) return INVALID_TASK_ID;

        std::lock_guard<std::mutex> lock(tasks_mutex_);

        TaskId id = task->id();
        if (id == INVALID_TASK_ID) {
            return INVALID_TASK_ID;
        }
        if (tasks_.find(id) != tasks_.end()) {
            return INVALID_TASK_ID;
        }
        task->set_priority(priority);
        bind_task_listener(task);
        tasks_[id] = std::move(task);

        // 不自动添加到队列，等待 start_task 调用

        // 如果启用了自动保存，保存状态（条件与路径读取在锁内完成）
        save_state_async();

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
        if (is_task_enqueued(id)) {
            return false;
        }

        if (task->status() == TaskStatus::Paused) {
            task->set_status(TaskStatus::Pending);
        }

        task->set_priority(priority);
        enqueue_task(id, priority);
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

        clear_enqueued_task(id);
        tasks_.erase(it);

        // 如果启用了自动保存，保存状态（条件与路径读取在锁内完成）
        save_state_async();

        return true;
    }

    size_t cleanup_finished_tasks() {
        std::lock_guard<std::mutex> lock(tasks_mutex_);
        size_t count = 0;

        for (auto it = tasks_.begin(); it != tasks_.end();) {
            if (it->second->is_finished()) {
                clear_enqueued_task(it->first);
                it = tasks_.erase(it);
                ++count;
            } else {
                ++it;
            }
        }

        return count;
    }

    // 置变化标志并唤醒清理线程：进行中的旧等待立即被打断，但变化轮
    // 只重新计时、不执行清扫——改周期绝不追加一次清扫（设大周期保留
    // 终态任务的宿主不希望 set 那一刻反向清掉它们）。标志位解决 notify
    // 落在清理线程非等待期（正在清扫/收尾）时丢失的问题。
    void set_cleanup_interval(std::chrono::seconds interval) {
        {
            std::lock_guard<std::mutex> lock(cleanup_mutex_);
            config_.cleanup_interval = interval;
            cleanup_interval_changed_.store(true, std::memory_order_release);
        }
        cleanup_cv_.notify_all();
    }

    bool start_task(TaskId id) {
        auto task = get_task(id);
        if (!task || task->is_active() || task->is_finished()) {
            return false;
        }

        return start_task(id, task->get_priority());
    }

    bool pause_task(TaskId id) {
        auto task = get_task(id);
        if (!task) return false;

        clear_enqueued_task(id);

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
        if (!task || task->status() != TaskStatus::Paused) return false;

        return start_task(id, task->get_priority());
    }

    bool cancel_task(TaskId id) {
        auto task = get_task(id);
        if (!task) return false;

        clear_enqueued_task(id);

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
        return queued_task_versions_.size();
    }

    size_t get_active_task_count() const {
        std::lock_guard<std::mutex> lock(active_mutex_);
        return active_tasks_.size();
    }

    size_t get_max_concurrent_tasks() const {
        return max_concurrent_tasks_.load(std::memory_order_relaxed);
    }

    void set_max_concurrent_tasks(size_t max_tasks) {
        max_concurrent_tasks_.store(std::max<size_t>(1, max_tasks), std::memory_order_relaxed);
        cv_.notify_one();
    }

    void set_global_speed_source(std::atomic<std::uint64_t>* source) {
        global_speed_source_ = source;
    }

    bool adjust_task_priority(TaskId id, TaskPriority priority) {
        auto task = get_task(id);
        if (!task) return false;

        // 更新任务对象的优先级
        task->set_priority(priority);

        if (is_task_enqueued(id)) {
            enqueue_task(id, priority);
        }
        return true;
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

            // 逐字段独立语句：输出格式（空格分隔、顺序）与序列化契约不变，
            // 但消除单语句跨多行的 gcc 行归属测量伪影
            auto write_quoted = [&file](const std::string& value) {
                file << std::quoted(value) << " ";
            };
            file << "task " << id << " ";
            file << static_cast<int>(task->status()) << " ";
            file << task->downloaded_bytes() << " ";
            file << task->total_bytes() << " ";
            file << task->speed() << " ";
            file << static_cast<int>(task->get_priority()) << " ";
            write_quoted(task->url());
            write_quoted(task->output_path());
            write_quoted(task->error_message());
            file << options.max_connections << " ";
            file << options.timeout_seconds << " ";
            file << options.max_retries << " ";
            file << options.retry_delay_seconds << " ";
            write_quoted(options.output_directory);
            write_quoted(options.output_filename);
            file << options.speed_limit << " ";
            file << (options.resume_enabled ? 1 : 0) << " ";
            write_quoted(options.user_agent);
            write_quoted(options.proxy);
            write_quoted(options.proxy_type);
            write_quoted(options.proxy_username);
            write_quoted(options.proxy_password);
            file << (options.verify_ssl ? 1 : 0) << " ";
            write_quoted(options.referer);
            write_quoted(options.cookie_file);
            write_quoted(options.cookie_jar);
            file << options.min_segment_size << " ";
            file << (options.adaptive_segment_sizing ? 1 : 0) << " ";
            file << options.progress_interval_ms << " ";
            file << (options.create_directory ? 1 : 0) << " ";
            file << (options.overwrite_existing ? 1 : 0) << " ";
            file << (options.auto_file_renaming ? 1 : 0) << " ";
            file << (options.conditional_get ? 1 : 0) << " ";
            write_quoted(options.client_certificate);
            write_quoted(options.client_private_key);
            file << options.headers.size();

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
            // Invalid magic - reject the file
            return false;
        }

        int version = 0;
        if (!(file >> version) || version < 1 || version > kTaskManagerStateVersion) {
            return false;
        }

        // Clear existing state
        {
            std::lock_guard<std::mutex> lock(tasks_mutex_);
            tasks_.clear();
        }
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            task_queue_ = std::priority_queue<TaskQueueItem>{};
            queued_task_versions_.clear();
            queue_sequence_.store(0, std::memory_order_relaxed);
        }
        {
            std::lock_guard<std::mutex> lock(active_mutex_);
            active_tasks_.clear();
            active_count_.store(0);
        }

        // Read task lines
        std::string line;
        std::getline(file, line); // consume remainder of header line
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            std::istringstream in(line);
            std::string tag;
            if (!(in >> tag) || tag != "task") {
                // Invalid line format - reject the file
                return false;
            }

            // Read basic task data
            TaskId id = INVALID_TASK_ID;
            std::string url, output_path, error_message;
            Bytes downloaded_bytes = 0, total_bytes = 0;
            BytesPerSecond speed = 0;
            int status_int = 0;
            int priority_int = static_cast<int>(TaskPriority::Normal);

            if (!read_task_data(in, version, id, url, output_path, error_message,
                                downloaded_bytes, total_bytes, speed, status_int,
                                priority_int)) {
                continue;
            }

            // Read download options
            DownloadOptions options;
            if (!read_download_options(in, version, options)) {
                continue;
            }

            // Validate and create task
            if (id == INVALID_TASK_ID || url.empty()) {
                continue;
            }

            TaskStatus status = TaskStatus::Pending;
            if (status_int >= 0 && status_int <= 6) {
                status = static_cast<TaskStatus>(status_int);
            }
            status = sanitize_status_for_restore(status);
            const auto priority = sanitize_priority_for_restore(priority_int);

            auto task = std::make_shared<DownloadTask>(id, url, options);
            bind_task_listener(task);  // Set listener first before any operations
            task->set_priority(priority);
            task->set_output_path(output_path);
            if (!error_message.empty()) {
                task->set_error(error_message);
            }
            task->update_progress(downloaded_bytes, total_bytes, speed);
            task->set_status(status);

            {
                std::lock_guard<std::mutex> lock(tasks_mutex_);
                if (tasks_.find(id) != tasks_.end()) {
                    continue;
                }
                tasks_[id] = task;  // Don't move, just assign
            }
        }

        return true;
    }

    void set_state_file(const std::string& file_path) {
        std::lock_guard<std::mutex> lock(state_file_mutex_);
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

    void on_file_info(TaskId task_id, const FileInfo& info) {
        // 分发文件信息事件
        if (event_dispatcher_) {
            event_dispatcher_->dispatch_file_info(task_id, info);
        }
    }

    BytesPerSecond query_speed_limit(TaskId task_id) override {
        // 全局限速按最大并发槽位均摊：并发任务数不会超过槽位数，
        // 每任务 ≤ 全局/槽位 即可保证总速率不超全局限制
        BytesPerSecond limit = 0;
        if (global_speed_source_) {
            const BytesPerSecond global =
                global_speed_source_->load(std::memory_order_relaxed);
            if (global > 0) {
                limit = global / max_concurrent_tasks_.load(std::memory_order_relaxed);
            }
        }

        // 任务自身限速更严格时以任务为准
        if (auto task = get_task(task_id)) {
            const auto own = task->options().speed_limit;
            if (own > 0 && (limit == 0 || own < limit)) {
                limit = own;
            }
        }
        return limit;
    }

private:
    void worker_loop() {
        while (running_) {
            TaskQueueItem item;
            DownloadTask::Ptr task;

            // 等待任务
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);

                cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                    return !running_ ||
                           (!task_queue_.empty() &&
                            active_count_.load() <
                                max_concurrent_tasks_.load(std::memory_order_relaxed));
                });

                if (!running_) break;

                // 超时返回时队列可能为空，再次检查
                if (task_queue_.empty()) {
                    continue;
                }

                // 获取任务
                item = task_queue_.top();
                task_queue_.pop();

                auto it = queued_task_versions_.find(item.task_id);
                if (it == queued_task_versions_.end() || it->second != item.sequence) {
                    continue;
                }
                queued_task_versions_.erase(it);
            }

            // 获取任务对象
            task = get_task(item.task_id);
            if (!task || task->is_finished() || task->is_active() ||
                task->status() != TaskStatus::Pending) {
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
                cleanup_cv_.wait_for(lock, config_.cleanup_interval, [this] {
                    return !running_ ||
                           cleanup_interval_changed_.load(std::memory_order_acquire);
                });
            }
            if (!running_) {
                break;
            }
            // 周期变化轮：仅按新周期重新计时，跳过清扫
            if (cleanup_interval_changed_.exchange(false)) {
                continue;
            }

            // 清理完成的任务
            cleanup_finished_tasks();

            // 保存状态（条件与路径读取在锁内完成）
            save_state_async();
        }
    }

    void save_state_async() {
        // 在锁下一次性读取 auto_save_state 与 state_file 快照：
        // set_state_file 可能由宿主线程并发调用，不能让异步 lambda 直接
        // 触碰 config_，否则读到的路径与写入存在数据竞争。
        std::string path;
        {
            std::lock_guard<std::mutex> lock(state_file_mutex_);
            if (!config_.auto_save_state || config_.state_file.empty()) {
                return;
            }
            path = config_.state_file;
        }

        bool expected = false;
        if (!state_save_scheduled_.compare_exchange_strong(expected, true)) {
            return;
        }

        state_pool_.submit([this, path] {
            save_state(path);
            state_save_scheduled_.store(false);
        });
    }

    void enqueue_task(TaskId id, TaskPriority priority) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            const auto sequence = ++queue_sequence_;
            queued_task_versions_[id] = sequence;
            task_queue_.push(TaskQueueItem{id, priority, std::chrono::steady_clock::now(), sequence});
        }
        cv_.notify_one();
    }

    [[nodiscard]] bool is_task_enqueued(TaskId id) const {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        return queued_task_versions_.find(id) != queued_task_versions_.end();
    }

    void clear_enqueued_task(TaskId id) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        queued_task_versions_.erase(id);
    }

    TaskManagerConfig config_;
    /// 保护 config_.state_file 的并发读写（set_state_file vs 异步保存）
    mutable std::mutex state_file_mutex_;
    EventDispatcher* event_dispatcher_;
    std::atomic<size_t> max_concurrent_tasks_{1};
    std::atomic<size_t> active_count_{0};
    /// 引擎全局限速原子（DownloadEngine::Impl 持有，非拥有）
    std::atomic<std::uint64_t>* global_speed_source_ = nullptr;

    // 任务存储
    mutable std::mutex tasks_mutex_;
    std::unordered_map<TaskId, DownloadTask::Ptr> tasks_;

    // 所有注入过本 Impl 作为 listener 的任务（弱引用，不延长生命周期）
    std::mutex listener_registry_mutex_;
    std::vector<std::weak_ptr<DownloadTask>> listener_bound_tasks_;

    // 任务队列
    mutable std::mutex queue_mutex_;
    std::priority_queue<TaskQueueItem> task_queue_;
    // 记录每个任务当前有效的入队版本，用于跳过旧条目和重复调度
    std::unordered_map<TaskId, std::uint64_t> queued_task_versions_;
    std::atomic<std::uint64_t> queue_sequence_{0};
    std::condition_variable cv_;

    // 清理线程唤醒
    mutable std::mutex cleanup_mutex_;
    std::condition_variable cleanup_cv_;
    // set_cleanup_interval 变化标志：打断旧等待并令变化轮跳过清扫
    std::atomic<bool> cleanup_interval_changed_{false};

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

} // namespace falcon
