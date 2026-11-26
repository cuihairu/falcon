/**
 * @file main_window_service.cpp
 * @brief MainWindow download service wiring, request handlers and task events
 */

#include "main_window.hpp"
#include "dialogs/add_download_dialog.hpp"
#include "ipc/http_server.hpp"
#include "navigation/sidebar.hpp"
#include "pages/download_page.hpp"
#include "pages/settings_page.hpp"
#include "pages/trash_page.hpp"
#include "services/download_backend.hpp"
#include "services/download_service.hpp"
#include "utils/url_detector.hpp"
#include "widgets/speed_float_widget.hpp"
#include "widgets/status_bar.hpp"

#include <QDesktopServices>
#include <QFileInfo>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include <QUrl>

using namespace falcon::desktop;

void MainWindow::open_url(const QString& url)
{
    const UrlInfo url_info = UrlDetector::parse_url(url);
    if (!url_info.is_valid) {
        QMessageBox::warning(this, tr("Invalid URL"), tr("Unrecognized download URL:\n%1").arg(url));
        return;
    }
    on_url_detected(url_info);
}

void MainWindow::ensure_download_service()
{
    if (download_service_) {
        return;
    }

    // 按设置选择后端：daemon 模式经 JSON-RPC 访问 falcon-daemon，
    // 否则维持进程内引擎直连。回收站仅进程内后端启用（目录为
    // <下载目录>/.falcon-trash）；daemon 路径保持旧行为（remove 即删）。
    std::unique_ptr<IDownloadBackend> backend;
    std::string trash_dir;
    if (settings_page_ && settings_page_->is_daemon_mode_enabled()) {
        falcon::daemon::rpc::JsonRpcClientConfig config;
        config.url = settings_page_->get_daemon_rpc_url().toStdString();
        config.secret = settings_page_->get_daemon_rpc_secret().toStdString();
        config.timeout_seconds = 5;
        backend = make_daemon_rpc_backend(std::move(config));
    } else {
        backend = make_inprocess_backend();
        if (settings_page_) {
            trash_dir = settings_page_->get_default_download_dir().toStdString()
                        + "/.falcon-trash";
        }
    }

    download_service_ = new DownloadService(std::move(backend), std::move(trash_dir), this);
    connect(download_service_, &DownloadService::tasks_refreshed,
            this, &MainWindow::on_tasks_refreshed);
    connect(download_service_, &DownloadService::stats_refreshed,
            this, &MainWindow::on_stats_refreshed);
    connect(download_service_, &DownloadService::task_add_failed,
            this, &MainWindow::on_task_add_failed);
    connect(download_service_, &DownloadService::task_completed,
            this, &MainWindow::on_task_completed);
    connect(download_service_, &DownloadService::task_failed,
            this, &MainWindow::on_task_failed);
    connect(download_service_, &DownloadService::seeding_stopped,
            this, [this](falcon::TaskId id, const QString& output_path,
                        const QString& stop_reason) {
                Q_UNUSED(id);
                if (!settings_page_ || !settings_page_->is_notifications_enabled()) {
                    return;
                }
                if (system_tray_ && system_tray_->isVisible()) {
                    QString reason_text;
                    if (stop_reason == QLatin1String("manual")) {
                        reason_text = tr("做种已手动停止。");
                    } else if (stop_reason == QLatin1String("limit_reached")) {
                        reason_text = tr("做种已达份额/时长上限，自动停止。");
                    } else {
                        reason_text = tr("做种已结束（达标或手动停止）。");
                    }
                    system_tray_->showMessage(
                        tr("做种已停止"),
                        tr("%1\n%2").arg(output_path, reason_text),
                        QSystemTrayIcon::Information,
                        3000);
                }
            });
    // 回收站内容变化（入站/恢复/清理）→ 全量重拉并刷新回收站页
    connect(download_service_, &DownloadService::trash_changed,
            this, [this]() {
                if (trash_page_) {
                    trash_page_->set_entries(download_service_->trash_list());
                }
            });
    // 首次填充：清单在 TrashStore 构造时已加载，等变更才有数据会空一屏
    if (trash_page_) {
        trash_page_->set_entries(download_service_->trash_list());
    }

    // 全局设置（并发数/限速/做种默认值/回收站保留天数）需在首次轮询前生效
    if (settings_page_) {
        download_service_->apply_global_settings(
            static_cast<std::size_t>(settings_page_->get_max_concurrent_downloads()),
            static_cast<std::size_t>(settings_page_->get_global_speed_limit()) * 1024);
        download_service_->apply_seed_defaults(
            settings_page_->get_seed_ratio(),
            static_cast<std::size_t>(settings_page_->get_seed_time_minutes()));
        download_service_->set_trash_retention_days(
            settings_page_->get_trash_retention_days());
    }

    // 500ms 仅作兜底轮询：daemon RPC 后端经 WebSocket 事件流收到通知
    // （任务状态变更/进度推送）会即时触发刷新，进程内后端保持周期驱动
    download_service_->start(500);
}

void MainWindow::show_add_download_dialog(UrlInfo url_info, const IncomingDownloadRequest* request_context)
{
    if (request_context) {
        if (!request_context->filename.trimmed().isEmpty()) {
            url_info.file_name = request_context->filename.trimmed();
        }
    }

    AddDownloadDialog dialog(url_info, this);
    if (settings_page_) {
        dialog.set_default_save_path(settings_page_->get_default_download_dir());
        dialog.set_default_connections(settings_page_->get_default_connections());
        dialog.set_seed_defaults(settings_page_->get_seed_ratio(),
                                 static_cast<std::size_t>(settings_page_->get_seed_time_minutes()));
    }

    if (request_context) {
        dialog.set_request_referrer(request_context->referrer);
        dialog.set_request_user_agent(request_context->user_agent);
        dialog.set_request_cookies(request_context->cookies);
    }

    // 主窗可能隐藏在托盘（剪贴板检测弹窗场景）：模态 exec 前显式
    // show/raise/activateWindow 保证对话框落到前台并获得焦点——否则
    // 在部分窗口管理器下对话框藏在其他窗口后面，用户看不到"没反应"
    dialog.show();
    dialog.raise();
    dialog.activateWindow();

    const int exec_result = dialog.exec();
    if (exec_result != QDialog::Accepted) {
        return;
    }

    ensure_download_service();

    falcon::DownloadOptions options;
    options.max_connections = static_cast<std::size_t>(dialog.get_connections());
    options.output_directory = dialog.get_save_path().toStdString();
    options.output_filename = dialog.get_file_name().toStdString();
    options.user_agent = dialog.get_user_agent().toStdString();
    options.referer = dialog.get_referrer().toStdString();
    // 对话框路径显式携带做种策略（用户看到并确认的值，不被全局默认覆写）
    options.seed_ratio = dialog.get_seed_ratio();
    options.seed_time_minutes = dialog.get_seed_time_minutes();

    // 应用任务速度限制（KB/s -> bytes/s）+ 连接超时（停滞超时，aria2
    // --timeout 同语义）与重试次数——设置项此前保存却从不消费
    if (settings_page_) {
        const int task_limit_kb = settings_page_->get_task_speed_limit();
        options.speed_limit = static_cast<std::size_t>(task_limit_kb * 1024);
        options.timeout_seconds =
            static_cast<std::size_t>(settings_page_->get_connection_timeout());
        options.max_retries =
            static_cast<std::size_t>(settings_page_->get_retry_count());
    }

    const QString cookies = dialog.get_cookies();
    if (!cookies.isEmpty()) {
        options.headers["Cookie"] = cookies.toStdString();
    }

    // 异步提交；被拒绝时经 task_add_failed 信号提示（成功不打扰——
    // 任务行即刻出现在列表里，旧的无样式"已添加"确认框只会盖住它）
    download_service_->add_task(dialog.get_url(), options, true);
}

bool MainWindow::add_download_task(const QString& url, bool start_immediately)
{
    ensure_download_service();

    falcon::DownloadOptions options;
    // 无对话框路径（剪贴板/扩展/发现页直达）：seed_ratio < 0 是桌面层
    // 「未显式设置」哨兵，后端按全局做种默认填充
    options.seed_ratio = -1.0;
    if (settings_page_) {
        options.max_connections = static_cast<std::size_t>(settings_page_->get_default_connections());
        options.output_directory = settings_page_->get_default_download_dir().toStdString();
        // 应用任务速度限制（KB/s -> bytes/s）+ 连接超时与重试次数
        const int task_limit_kb = settings_page_->get_task_speed_limit();
        options.speed_limit = static_cast<std::size_t>(task_limit_kb * 1024);
        options.timeout_seconds =
            static_cast<std::size_t>(settings_page_->get_connection_timeout());
        options.max_retries =
            static_cast<std::size_t>(settings_page_->get_retry_count());
    }

    // 异步提交；被拒绝时经 task_add_failed 信号提示
    download_service_->add_task(url, options, start_immediately);
    return true;
}

void MainWindow::on_url_detected(const UrlInfo& url_info)
{
    show_add_download_dialog(url_info, nullptr);
}

void MainWindow::on_download_requested(const IncomingDownloadRequest& request)
{
    const UrlInfo url_info = UrlDetector::parse_url(request.url);
    if (!url_info.is_valid) {
        QMessageBox::warning(this, tr("Invalid URL"), tr("Unrecognized download URL:\n%1").arg(request.url));
        return;
    }

    show_add_download_dialog(url_info, &request);
}

void MainWindow::on_new_task_requested()
{
    // 直接进 Fluent 添加对话框,URL 在对话框内输入(协议/文件名随输入
    // 联动);旧 QInputDialog 两段式路径是 UI 重做前的遗留,无主题样式
    show_add_download_dialog(UrlDetector::parse_url(QString()), nullptr);
}

void MainWindow::on_configured_download_requested(const QString& url)
{
    open_url(url);
}

void MainWindow::on_direct_download_requested(const QString& url, bool start_immediately)
{
    add_download_task(url, start_immediately);
}

void MainWindow::on_remove_task_requested(falcon::TaskId id)
{
    if (!download_service_) {
        return;
    }

    // 进程内后端走回收站（文件暂存可恢复）；daemon 后端保持旧行为
    if (download_service_->trash_available()) {
        download_service_->remove_task_to_trash(id);
    } else {
        download_service_->remove_task(id);
    }
}

void MainWindow::on_remove_finished_tasks_requested()
{
    if (!download_service_) {
        return;
    }

    download_service_->remove_finished_tasks();
}

void MainWindow::on_redownload_requested(const QString& url)
{
    // 重新下载（已完成视图批量菜单）：按原 URL 新建任务并立即开始，
    // 走与「新建任务」同一条 add_download_task 路径（协议/对话框逻辑复用）
    add_download_task(url, true);
}

void MainWindow::on_priority_changed(falcon::TaskId id, falcon::TaskPriority priority)
{
    if (!download_service_) {
        return;
    }

    download_service_->set_priority(id, priority);
}

//==============================================================================
// DownloadService Events（信号已从 worker 线程排队投递到 GUI 线程）
//==============================================================================

void MainWindow::on_tasks_refreshed(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks)
{
    // 浏览器扩展 IPC /v1/tasks 的数据源
    latest_task_snapshots_ = tasks;

    // 维护 id → URL 映射，供错误通知显示文件名
    task_url_by_id_.clear();
    task_url_by_id_.reserve(static_cast<int>(tasks.size()) * 2);
    for (const auto& snap : tasks) {
        task_url_by_id_.insert(static_cast<qulonglong>(snap.id),
                               QString::fromStdString(snap.url));
    }

    if (download_page_) {
        download_page_->update_tasks(tasks);
    }

    // 悬浮速度窗预览任务:第一个 Downloading,否则第一个 Paused
    // (B21: 标题行显示任务名 + 暂停/继续钮;无活动/暂停任务时 has_task=false)
    if (speed_float_) {
        SpeedFloatWidget::TaskPreview preview;
        for (const auto& snap : tasks) {
            if (snap.status != falcon::TaskStatus::Downloading
                && snap.status != falcon::TaskStatus::Paused) {
                continue;
            }
            preview.has_task = true;
            preview.task_id = static_cast<std::uint64_t>(snap.id);
            preview.running = snap.status == falcon::TaskStatus::Downloading;
            preview.url = QString::fromStdString(snap.url);
            const QFileInfo file_info(
                QString::fromStdString(snap.output_path));
            preview.file_name = file_info.fileName();
            preview.directory = file_info.absolutePath();
            preview.progress = snap.progress;
            break;
        }
        speed_float_->update_task_preview(preview);
    }
}

void MainWindow::on_stats_refreshed(falcon::daemon::rpc::GlobalStats stats)
{
    // 浏览器扩展 IPC /v1/stats 的数据源
    latest_stats_ = stats;

    if (status_bar_) {
        status_bar_->set_download_speed(static_cast<uint64_t>(stats.download_speed));
        status_bar_->set_task_counts(static_cast<int>(stats.active_tasks),
                                     static_cast<int>(stats.stopped_tasks));
    }

    // 侧栏底部统计卡:真实活跃任务数(替代此前的硬编码假数据)
    if (side_bar_) {
        side_bar_->set_queue_count(static_cast<int>(stats.active_tasks));
    }

    // 悬浮速度窗:速度/活跃任务数取自统计快照;总进度 = 活跃任务平均进度
    // (来自最近一轮任务快照,同一刷新周期内最多滞后半拍)
    if (speed_float_) {
        SpeedFloatWidget::Stats fs;
        fs.download_speed = static_cast<std::uint64_t>(stats.download_speed);
        fs.active_tasks = static_cast<int>(stats.active_tasks);
        int downloading = 0;
        double progress_sum = 0.0;
        for (const auto& snap : latest_task_snapshots_) {
            if (snap.status == falcon::TaskStatus::Downloading) {
                ++downloading;
                progress_sum += snap.progress;
            }
        }
        if (downloading > 0) {
            fs.overall_progress = progress_sum / downloading;
        }
        speed_float_->update_stats(fs);
    }
}

void MainWindow::on_task_add_failed(const QString& url, const QString& reason)
{
    QMessageBox::warning(this, tr("Download"),
                         tr("Failed to add download task:\n%1\n\n%2").arg(url, reason));
}

void MainWindow::on_task_failed(falcon::TaskId id, const QString& error_message)
{
    // 如果通知未启用，直接返回
    if (!settings_page_ || !settings_page_->is_notifications_enabled()) {
        return;
    }

    // 从最近快照提取文件名用于显示
    QString task_name = tr("Task #%1").arg(static_cast<qulonglong>(id));
    const auto it = task_url_by_id_.constFind(static_cast<qulonglong>(id));
    if (it != task_url_by_id_.constEnd() && !it.value().isEmpty()) {
        const QString& url = it.value();
        const int last_slash = url.lastIndexOf('/');
        if (last_slash >= 0 && last_slash < url.length() - 1) {
            task_name = url.mid(last_slash + 1);
        } else {
            task_name = url;
        }
    }

    // 使用系统托盘显示错误通知
    if (system_tray_ && system_tray_->isVisible()) {
        system_tray_->showMessage(
            tr("Download Error"),
            tr("%1\nError: %2").arg(task_name, error_message),
            QSystemTrayIcon::Critical,
            5000  // 显示 5 秒
        );
    }
}

void MainWindow::on_task_completed(falcon::TaskId id, const QString& output_path)
{
    (void)id;

    if (!settings_page_) {
        return;
    }

    const int action = settings_page_->get_action_when_completed();

    switch (action) {
        case 0:  // Do nothing
            break;

        case 1:  // Open file
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(output_path));
            break;
        }

        case 2:  // Open folder
        {
            const QFileInfo file_info(output_path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(file_info.absolutePath()));
            break;
        }

        case 3:  // Show notification only
        {
            if (system_tray_ && settings_page_->is_notifications_enabled()) {
                system_tray_->showMessage(
                    tr("Download Completed"),
                    tr("A download has finished successfully."),
                    QSystemTrayIcon::Information,
                    3000
                );
            }
            break;
        }
    }
}
