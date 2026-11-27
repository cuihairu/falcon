/**
 * @file main_window_ipc.cpp
 * @brief MainWindow IPC server setup and JSON snapshot serialization
 */

#include "main_window.hpp"
#include "ipc/http_server.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

using namespace falcon::desktop;

namespace {
constexpr quint16 kIpcPort = 51337;

/** TaskStatus → 扩展任务面板使用的 aria2 风格状态串 */
QByteArray ipc_status_text(falcon::TaskStatus status)
{
    switch (status) {
    case falcon::TaskStatus::Pending:
    case falcon::TaskStatus::Preparing:
        return "waiting";
    case falcon::TaskStatus::Downloading:
        return "active";
    case falcon::TaskStatus::Paused:
        return "paused";
    case falcon::TaskStatus::Completed:
        return "complete";
    case falcon::TaskStatus::Failed:
        return "error";
    case falcon::TaskStatus::Cancelled:
        return "removed";
    }
    return "unknown";
}

/** 序列化任务快照为 /v1/tasks 响应体（Qt JSON，含转义处理） */
QByteArray build_ipc_tasks_json(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks)
{
    QJsonArray arr;
    for (const auto& snap : tasks) {
        QJsonObject o;
        o.insert("id", static_cast<qint64>(snap.id));
        o.insert("url", QString::fromStdString(snap.url));
        o.insert("path", QString::fromStdString(snap.output_path));
        o.insert("status", QString::fromUtf8(ipc_status_text(snap.status)));
        o.insert("progress", snap.progress);
        o.insert("totalBytes", static_cast<qint64>(snap.total_bytes));
        o.insert("downloadedBytes", static_cast<qint64>(snap.downloaded_bytes));
        o.insert("speed", static_cast<qint64>(snap.speed));
        if (!snap.error_message.empty()) {
            o.insert("error", QString::fromStdString(snap.error_message));
        }
        arr.append(o);
    }
    QJsonObject root;
    root.insert("ok", true);
    root.insert("tasks", arr);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/** 序列化全局统计为 /v1/stats 响应体 */
QByteArray build_ipc_stats_json(const falcon::daemon::rpc::GlobalStats& stats)
{
    QJsonObject root;
    root.insert("ok", true);
    root.insert("downloadSpeed", static_cast<qint64>(stats.download_speed));
    root.insert("activeTasks", static_cast<int>(stats.active_tasks));
    root.insert("waitingTasks", static_cast<int>(stats.waiting_tasks));
    root.insert("stoppedTasks", static_cast<int>(stats.stopped_tasks));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}
} // namespace

void MainWindow::setup_ipc_server()
{
    ipc_server_ = new HttpIpcServer(this);
    connect(ipc_server_, &HttpIpcServer::download_requested, this, &MainWindow::on_download_requested);
    // 只读查询端点的数据源（缓存由 on_tasks_refreshed/on_stats_refreshed 更新，
    // 两者都在 GUI 线程，与本服务器同线程，回调内直接读无并发问题）
    ipc_server_->set_tasks_provider([this]() {
        return build_ipc_tasks_json(latest_task_snapshots_);
    });
    ipc_server_->set_stats_provider([this]() {
        return build_ipc_stats_json(latest_stats_);
    });
    ipc_server_->start(kIpcPort);
}
