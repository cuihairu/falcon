/**
 * @file download_service.hpp
 * @brief 下载服务：worker 线程封装后端，经 Qt 信号向 GUI 线程推送快照
 * @author Falcon Team
 * @date 2026-09-11
 */

#pragma once

#include <services/download_backend.hpp>

#include <QObject>
#include <QString>

#include <condition_variable>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

// 信号参数类型在 Qt6 下无需 Q_DECLARE_METATYPE：PMF connect 的跨线程
// 排队连接经 QMetaType::fromType 编译期解析（类型需可默认构造/可拷贝）。

namespace falcon::desktop {

/**
 * @brief 下载服务（QObject）
 *
 * 持有一个 IDownloadBackend，所有后端调用在内部 worker 线程执行
 * （RPC 实现会阻塞在网络往返上，不能占用 GUI 线程）。结果通过信号
 * 投递回 GUI 线程（跨线程自动 QueuedConnection）：
 * - tasks_refreshed/stats_refreshed：按轮询周期推送
 * - task_completed/task_failed：对比前后两轮快照得出（两种后端统一路径）
 *
 * 公开方法均为线程安全的命令提交，立即返回。
 */
class DownloadService : public QObject
{
    Q_OBJECT

public:
    explicit DownloadService(std::unique_ptr<IDownloadBackend> backend,
                             QObject* parent = nullptr);
    ~DownloadService() override;

    DownloadService(const DownloadService&) = delete;
    DownloadService& operator=(const DownloadService&) = delete;

    /// 启动 worker 线程与轮询（重复调用无效果）
    void start(int poll_interval_ms = 500);

    /// 停止 worker 线程（阻塞至线程退出；析构时自动调用）
    void stop();

    // ---- 命令（异步执行） ----
    void add_task(const QString& url, const falcon::DownloadOptions& options,
                  bool start_immediately);
    void pause_task(falcon::TaskId id);
    void resume_task(falcon::TaskId id);
    void remove_task(falcon::TaskId id);
    void remove_finished_tasks();
    void set_priority(falcon::TaskId id, falcon::TaskPriority priority);
    void apply_global_settings(std::size_t max_concurrent_tasks,
                               std::size_t global_speed_limit_bytes);

signals:
    /// 每轮轮询推送全量任务快照（GUI 线程接收）
    void tasks_refreshed(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks);
    /// 每轮轮询推送全局统计（不可用时当轮不推送）
    void stats_refreshed(falcon::daemon::rpc::GlobalStats stats);
    /// 新建任务被后端拒绝
    void task_add_failed(const QString& url, const QString& reason);
    /// 任务完成（从快照变化推断，进程内与 RPC 后端统一）
    void task_completed(falcon::TaskId id, const QString& output_path);
    /// 任务失败（从快照变化推断）
    void task_failed(falcon::TaskId id, const QString& error_message);

private:
    void enqueue(std::function<void()>&& job);
    void notify_worker();
    void worker_loop();
    /// 与上一轮快照对比，发出完成/失败事件
    void publish_transitions(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks);

    std::unique_ptr<IDownloadBackend> backend_;

    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::chrono::milliseconds poll_interval_{500};
    bool stop_flag_ = false;
    bool started_ = false;

    /// worker 线程私有：上一轮快照（按 id 索引）
    std::map<falcon::TaskId, falcon::daemon::rpc::TaskSnapshot> last_snapshot_;
};

} // namespace falcon::desktop
