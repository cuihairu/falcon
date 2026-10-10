/**
 * @file download_service.cpp
 * @brief DownloadService 实现
 * @author Falcon Team
 * @date 2026-09-11
 */

#include "download_service.hpp"

#include <QDebug>
#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace falcon::desktop {

namespace {

std::int64_t now_epoch_seconds()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

DownloadService::DownloadService(std::unique_ptr<IDownloadBackend> backend,
                                 std::string trash_dir,
                                 QObject* parent)
    : QObject(parent)
    , backend_(std::move(backend))
{
    if (!trash_dir.empty()) {
        trash_store_ = std::make_unique<TrashStore>(std::move(trash_dir));
    }
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
    // 启动时按保留天数清理过期回收站条目（days <= 0 不自动清理）
    if (trash_store_) {
        enqueue([this]() {
            const int days = trash_retention_days_.load();
            if (days <= 0) {
                return;
            }
            if (trash_store_->purge_expired(days, now_epoch_seconds()) > 0) {
                emit trash_changed();
            }
        });
    }
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
        // add_task 可能抛(协议无 handler 的 UnsupportedProtocolException
        // 等)——worker 线程异常逃逸即 std::terminate 整个进程,转失败信号
        try {
            auto result = backend_->add_task(url_utf8, options, start_immediately);
            if (!result.ok) {
                emit task_add_failed(QString::fromStdString(url_utf8),
                                     QString::fromStdString(result.error));
            }
        } catch (const std::exception& e) {
            emit task_add_failed(QString::fromStdString(url_utf8),
                                 QString::fromUtf8(e.what()));
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

void DownloadService::apply_seed_defaults(double seed_ratio,
                                          std::size_t seed_time_minutes)
{
    enqueue([this, seed_ratio, seed_time_minutes]() {
        backend_->apply_seed_defaults(seed_ratio, seed_time_minutes);
    });
}

void DownloadService::stop_seeding(falcon::TaskId id)
{
    enqueue([this, id]() { (void)backend_->stop_seeding(id); });
}

void DownloadService::remove_task_to_trash(falcon::TaskId id)
{
    enqueue([this, id]() {
        if (!trash_store_) {
            // 无回收站能力（daemon 后端路径）：保持旧语义直接移除
            (void)backend_->remove_task(id);
            return;
        }

        // 快照先行：终态前元数据（取消/失败后快照仍含该任务一轮）
        TrashEntry entry;
        bool was_active = false;
        try {
            for (const auto& snap : backend_->fetch_tasks()) {
                if (snap.id != id) {
                    continue;
                }
                entry.id = id;
                entry.url = snap.url;
                entry.output_path = snap.output_path;
                entry.total_bytes = snap.total_bytes;
                was_active = !snap.is_finished();
                if (was_active) {
                    entry.status = "cancelled";
                } else if (snap.status == falcon::TaskStatus::Completed) {
                    entry.status = "completed";
                } else if (snap.status == falcon::TaskStatus::Failed) {
                    entry.status = "failed";
                } else {
                    entry.status = "cancelled";
                }
                break;
            }
        } catch (const std::exception&) {
            // 快照拉取失败也照常走移除（条目缺元数据可接受）
        }

        // 活动/暂停任务先取消（引擎 remove_task 只收终态；daemon 后端
        // 无 cancel_task 语义，aria2.remove 本就对活动任务生效）
        if (was_active) {
            (void)backend_->cancel_task(id);
        }
        if (!backend_->remove_task(id)) {
            return; // 移除失败不入回收站（任务仍在列表，用户可重试）
        }

        entry.deleted_at = now_epoch_seconds();
        // 仅 Completed 且成品文件在位才移入；取消/失败的半成品不动
        std::string move_file;
        std::error_code ec;
        if (entry.status == "completed" && !entry.output_path.empty()
            && std::filesystem::is_regular_file(entry.output_path, ec)) {
            move_file = entry.output_path;
        }
        if (!trash_store_->add(std::move(entry), move_file)) {
            qWarning() << "trash: failed to add entry for task" << id;
        }
        emit trash_changed();
    });
}

std::vector<TrashEntry> DownloadService::trash_list() const
{
    return trash_store_ ? trash_store_->list() : std::vector<TrashEntry>{};
}

void DownloadService::trash_restore(std::uint64_t id)
{
    enqueue([this, id]() {
        if (trash_store_->restore(id)) {
            emit trash_changed();
        }
    });
}

void DownloadService::trash_purge(std::uint64_t id)
{
    enqueue([this, id]() {
        if (trash_store_->purge(id)) {
            emit trash_changed();
        }
    });
}

void DownloadService::trash_clear()
{
    enqueue([this]() {
        if (trash_store_->clear() > 0) {
            emit trash_changed();
        }
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

        // 命令级兜底:任何 job 异常不得逃出 worker 线程(逃逸即
        // std::terminate 杀死整个进程);具体命令自带的失败信号不受影响
        while (!jobs.empty()) {
            auto job = std::move(jobs.front());
            jobs.pop_front();
            try {
                job();
            } catch (const std::exception& e) {
                qWarning() << "DownloadService command failed:" << e.what();
            } catch (...) {
                qWarning() << "DownloadService command failed: unknown exception";
            }
        }

        // 拉取一轮快照并推送（信号从本线程发出，跨线程自动排队）；
        // 后端故障不杀进程,本轮跳过等下个周期重试
        try {
            auto tasks = backend_->fetch_tasks();
            publish_transitions(tasks);
            emit tasks_refreshed(tasks);
            if (auto stats = backend_->fetch_stats()) {
                emit stats_refreshed(*stats);
            }
        } catch (const std::exception& e) {
            qWarning() << "DownloadService fetch failed:" << e.what();
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

    // 做种停止（seeding_active 翻转；任务保持 Completed 终态不触发上面的
    // 状态迁移）——达标自动停止与手动停止同路径提示
    for (const auto& [id, snap] : current) {
        const auto prev = last_snapshot_.find(id);
        if (prev == last_snapshot_.end()) {
            continue;
        }
        if (prev->second.seeding_active && !snap.seeding_active) {
            emit seeding_stopped(id, QString::fromStdString(snap.output_path),
                                 QString::fromStdString(snap.seed_stop_reason));
        }
    }

    last_snapshot_ = std::move(current);
}

} // namespace falcon::desktop
