/**
 * @file main_window_settings.cpp
 * @brief MainWindow settings persistence and startup update check
 */

#include "main_window.hpp"
#include "pages/download_page.hpp"
#include "pages/settings_page.hpp"
#include "services/download_service.hpp"
#include "services/task_order.hpp"
#include "services/update_checker.hpp"
#include "utils/clipboard_monitor.hpp"
#include "widgets/speed_float_widget.hpp"

#include <QDir>
#include <QSettings>
#include <QSystemTrayIcon>

using namespace falcon::desktop;

void MainWindow::load_settings()
{
    if (!settings_page_) {
        return;
    }

    QSettings settings;
    settings.beginGroup("desktop");

    // 剪贴板监听默认开启（主流下载器语义）：首次启动（无持久化记录）
    // 即生效；用户显式关闭后按持久化值保持关闭
    settings_page_->set_clipboard_monitoring_enabled(
        settings.value("clipboard_monitoring_enabled", true).toBool());
    settings_page_->set_clipboard_detection_delay(
        settings.value("clipboard_detection_delay_ms", 1000).toInt());
    settings_page_->set_default_download_dir(
        settings.value("default_download_dir", QDir::homePath() + "/Downloads").toString());
    settings_page_->set_max_concurrent_downloads(
        settings.value("max_concurrent_downloads", 3).toInt());
    settings_page_->set_default_connections(
        settings.value("default_connections", 4).toInt());
    settings_page_->set_connection_timeout(
        settings.value("connection_timeout_seconds", 30).toInt());
    settings_page_->set_retry_count(
        settings.value("retry_count", 3).toInt());
    settings_page_->set_task_speed_limit(
        settings.value("task_speed_limit_kb", 0).toInt());
    settings_page_->set_global_speed_limit(
        settings.value("global_speed_limit_kb", 0).toInt());
    settings_page_->set_seed_ratio(
        settings.value("seed_ratio", 1.0).toDouble());
    settings_page_->set_seed_time_minutes(
        settings.value("seed_time_minutes", 0).toInt());
    settings_page_->set_action_when_completed(
        settings.value("action_when_completed", 0).toInt());
    settings_page_->set_completed_double_click_action(
        settings.value("completed_double_click_action", 0).toInt());
    settings_page_->set_notifications_enabled(
        settings.value("notifications_enabled", true).toBool());
    settings_page_->set_sound_notifications_enabled(
        settings.value("sound_notifications_enabled", false).toBool());
    settings_page_->set_daemon_mode_enabled(
        settings.value("daemon_mode_enabled", false).toBool());
    settings_page_->set_daemon_rpc_url(
        settings.value("daemon_rpc_url", "http://127.0.0.1:6800/jsonrpc").toString());
    settings_page_->set_daemon_rpc_secret(
        settings.value("daemon_rpc_secret", "").toString());
    settings_page_->set_check_updates_on_startup_enabled(
        settings.value("check_updates_on_startup", true).toBool());
    settings_page_->set_trash_retention_days(
        settings.value("trash_retention_days", 7).toInt());
    settings_page_->set_float_widget_enabled(
        settings.value("float_widget_enabled", true).toBool());
    settings_page_->set_float_show_active_tasks(
        settings.value("float_show_active_tasks", true).toBool());
    settings_page_->set_float_show_total_progress(
        settings.value("float_show_total_progress", true).toBool());
    settings_page_->set_float_size_preset(
        settings.value("float_size_preset", 0).toInt()); // B21①: 默认紧凑档
    settings_page_->set_float_opacity_percent(
        settings.value("float_opacity_percent", 90).toInt());
    settings_page_->set_float_click_through(
        settings.value("float_click_through", false).toBool());
    // 手动排序（"3,1,2" 逗号串；无记录/全垃圾 → 空序 = id 升序默认行为）
    const QString task_order_text = settings.value("task_order", QString()).toString();
    // 任务列表视图：首次/无记录默认卡片（grid）；"table" = 用户手动切过列表
    const bool task_view_grid =
        settings.value("task_display_style", QStringLiteral("grid")).toString() !=
        QStringLiteral("table");

    settings.endGroup();

    if (download_page_) {
        download_page_->set_task_order(
            task_order::deserialize(task_order_text.toStdString()));
        download_page_->set_display_style(
            task_view_grid ? TaskDisplayStyle::Grid : TaskDisplayStyle::Table);
    }
    if (settings_page_) {
        settings_page_->set_task_view_grid(task_view_grid);
    }
}

void MainWindow::save_settings() const
{
    if (!settings_page_) {
        return;
    }

    QSettings settings;
    settings.beginGroup("desktop");
    settings.setValue("clipboard_monitoring_enabled", settings_page_->is_clipboard_monitoring_enabled());
    settings.setValue("clipboard_detection_delay_ms", settings_page_->get_clipboard_detection_delay());
    settings.setValue("default_download_dir", settings_page_->get_default_download_dir());
    settings.setValue("max_concurrent_downloads", settings_page_->get_max_concurrent_downloads());
    settings.setValue("default_connections", settings_page_->get_default_connections());
    settings.setValue("connection_timeout_seconds", settings_page_->get_connection_timeout());
    settings.setValue("retry_count", settings_page_->get_retry_count());
    settings.setValue("task_speed_limit_kb", settings_page_->get_task_speed_limit());
    settings.setValue("global_speed_limit_kb", settings_page_->get_global_speed_limit());
    settings.setValue("seed_ratio", settings_page_->get_seed_ratio());
    settings.setValue("seed_time_minutes", settings_page_->get_seed_time_minutes());
    settings.setValue("action_when_completed", settings_page_->get_action_when_completed());
    settings.setValue("completed_double_click_action",
                      settings_page_->get_completed_double_click_action());
    settings.setValue("notifications_enabled", settings_page_->is_notifications_enabled());
    settings.setValue("sound_notifications_enabled", settings_page_->is_sound_notifications_enabled());
    settings.setValue("daemon_mode_enabled", settings_page_->is_daemon_mode_enabled());
    settings.setValue("daemon_rpc_url", settings_page_->get_daemon_rpc_url());
    settings.setValue("daemon_rpc_secret", settings_page_->get_daemon_rpc_secret());
    settings.setValue("check_updates_on_startup",
                      settings_page_->is_check_updates_on_startup_enabled());
    settings.setValue("trash_retention_days",
                      settings_page_->get_trash_retention_days());
    settings.setValue("float_widget_enabled",
                      settings_page_->is_float_widget_enabled());
    settings.setValue("float_show_active_tasks",
                      settings_page_->is_float_show_active_tasks());
    settings.setValue("float_show_total_progress",
                      settings_page_->is_float_show_total_progress());
    settings.setValue("float_size_preset",
                      settings_page_->get_float_size_preset());
    settings.setValue("float_opacity_percent",
                      settings_page_->get_float_opacity_percent());
    settings.setValue("float_click_through",
                      settings_page_->is_float_click_through());
    settings.endGroup();
    settings.sync();
}

void MainWindow::check_for_updates_on_startup()
{
    if (!settings_page_ || !settings_page_->is_check_updates_on_startup_enabled()) {
        return;
    }

    update_checker_ = new UpdateChecker(this);
    connect(update_checker_, &UpdateChecker::update_available, this,
            [this](const QString& latest, const QString& url) {
        // 启动静默检查仅托盘通知（发现页/设置页不打扰）；url 需用户
        // 自行前往设置页查看或访问 GitHub 发布页，不接入 messageClicked
        Q_UNUSED(url);
        if (system_tray_ && system_tray_->isVisible()) {
            system_tray_->showMessage(
                tr("发现新版本"),
                tr("Falcon %1 已发布，可在设置页检查更新获取发布页链接。").arg(latest),
                QSystemTrayIcon::Information, 8000);
        }
    });
    // up_to_date / check_failed 在启动路径一律静默（网络不通等情况不打扰）

    update_checker_->check_for_updates();
}

void MainWindow::apply_settings_to_runtime()
{
    if (!settings_page_) {
        return;
    }

    if (clipboard_monitor_) {
        clipboard_monitor_->set_detection_delay(settings_page_->get_clipboard_detection_delay());
        clipboard_monitor_->set_enabled(settings_page_->is_clipboard_monitoring_enabled());
    }

    if (download_service_) {
        download_service_->apply_global_settings(
            static_cast<std::size_t>(settings_page_->get_max_concurrent_downloads()),
            static_cast<std::size_t>(settings_page_->get_global_speed_limit()) * 1024);
        download_service_->apply_seed_defaults(
            settings_page_->get_seed_ratio(),
            static_cast<std::size_t>(settings_page_->get_seed_time_minutes()));
        download_service_->set_trash_retention_days(
            settings_page_->get_trash_retention_days());
    }

    if (speed_float_) {
        speed_float_->setVisible(settings_page_->is_float_widget_enabled());
        speed_float_->set_show_active_tasks(
            settings_page_->is_float_show_active_tasks());
        speed_float_->set_show_total_progress(
            settings_page_->is_float_show_total_progress());
        speed_float_->set_size_preset(
            static_cast<SpeedFloatWidget::SizePreset>(
                settings_page_->get_float_size_preset()));
        speed_float_->set_opacity_percent(
            settings_page_->get_float_opacity_percent());
        speed_float_->set_click_through(
            settings_page_->is_float_click_through());
    }
}
