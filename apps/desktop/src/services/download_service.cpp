/**
 * @file download_service.cpp
 * @brief DownloadService 实现
 * @author Falcon Team
 * @date 2026-09-11
 */

#include "download_service.hpp"

namespace falcon::desktop {

DownloadService::DownloadService(std::unique_ptr<IDownloadBackend> backend,
                                 QObject* parent)
    : QObject(parent)
    , backend_(std::move(backend))
{
}

DownloadService::~DownloadService()
{
    stop();
    // 先于 backend_ 成员析构解除事件回调（析构顺序上 mutex_ 会先于 backend_
    // 销毁，必须保证此后后端线程不再进入 request_refresh）。
    // set_wake_callback 返回后保证无在途调用。
    backend_->set_wake_callback({});
}

void DownloadService::start(int poll_interval_ms)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) {
            return;
        }
        poll_interval_ = std::chrono::milliseconds(poll_interval_ms);
        stop_flag_ = false;
        started_ = true;
    }
    // 事件驱动：后端有事件源（daemon 通知）时即时刷新，轮询降为兜底
    backend_->set_wake_callback([this]() { request_refresh(); });
    worker_ = std::thread([this]() { worker_loop(); });
}

void DownloadService::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) {
            return;
        }
        stop_flag_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false;
}

void DownloadService::enqueue(std::function<void()>&& job)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push_back(std::move(job));
    }
    cv_.notify_all();
}

void DownloadService::add_task(const QString& url,
                               const falcon::DownloadOptions& options,
                               bool start_immediately)
{
    const std::string url_utf8 = url.toStdString();
    enqueue([this, url_utf8, options, start_immediately]() {
        auto result = backend_->add_task(url_utf8, options, start_immediately);
        if (!result.ok) {
            emit task_add_failed(QString::fromStdString(url_utf8),
                                 QString::fromStdString(result.error));
        }
    });
}

void DownloadService::pause_task(falcon::TaskId id)
{
    enqueue([this, id]() { (void)backend_->pause_task(id); });
}

void DownloadService::resume_task(falcon::TaskId id)
{
    enqueue([this, id]() { (void)backend_->resume_task(id); });
}

void DownloadService::remove_task(falcon::TaskId id)
{
    enqueue([this, id]() { (void)backend_->remove_task(id); });
}

void DownloadService::remove_finished_tasks()
{
    enqueue([this]() { (void)backend_->remove_finished_tasks(); });
}

void DownloadService::set_priority(falcon::TaskId id, falcon::TaskPriority priority)
{
    enqueue([this, id, priority]() { (void)backend_->set_priority(id, priority); });
}

void DownloadService::apply_global_settings(std::size_t max_concurrent_tasks,
                                            std::size_t global_speed_limit_bytes)
{
    enqueue([this, max_concurrent_tasks, global_speed_limit_bytes]() {
        backend_->apply_global_settings(max_concurrent_tasks, global_speed_limit_bytes);
    });
}

void DownloadService::request_refresh()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_ || refresh_requested_) {
            return;
        }
        refresh_requested_ = true;
    }
    cv_.notify_all();
}

void DownloadService::worker_loop()
{
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stop_flag_) {
        // 等待命令 / 事件到达 / 轮询周期到点（兜底）
        cv_.wait_for(lock, poll_interval_,
                     [this]() {
                         return stop_flag_ || !queue_.empty() || refresh_requested_;
                     });
        if (stop_flag_) {
            return;
        }
        std::deque<std::function<void()>> jobs;
        jobs.swap(queue_);
        refresh_requested_ = false;  // 合并期间到达的重复事件
        lock.unlock();

        while (!jobs.empty()) {
            jobs.front()();
            jobs.pop_front();
        }

        // 拉取一轮快照并推送（信号从本线程发出，跨线程自动排队）
        auto tasks = backend_->fetch_tasks();
        publish_transitions(tasks);
        emit tasks_refreshed(tasks);
        if (auto stats = backend_->fetch_stats()) {
            emit stats_refreshed(*stats);
        }

        lock.lock();
    }
}

void DownloadService::publish_transitions(
    const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks)
{
    std::map<falcon::TaskId, falcon::daemon::rpc::TaskSnapshot> current;
    for (const auto& snap : tasks) {
        current.emplace(snap.id, snap);
    }

    for (const auto& [id, snap] : current) {
        if (!snap.is_finished()) {
            continue;
        }
        // 只在本轮亲眼见过该任务处于未完成状态时才通知；
        // 首轮轮询发现的历史终态任务仅作为基线记录
        auto prev = last_snapshot_.find(id);
        if (prev == last_snapshot_.end() || prev->second.is_finished()) {
            continue;
        }
        if (snap.status == falcon::TaskStatus::Completed) {
            emit task_completed(id, QString::fromStdString(snap.output_path));
        } else if (snap.status == falcon::TaskStatus::Failed) {
            emit task_failed(id, QString::fromStdString(snap.error_message));
        }
    }

    last_snapshot_ = std::move(current);
}

} // namespace falcon::desktop
