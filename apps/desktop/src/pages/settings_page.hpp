/**
 * @file settings_page.hpp
 * @brief Settings page for application configuration
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <QWidget>

#include <string>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>

namespace falcon::desktop {

class UpdateChecker;

/**
 * @brief Settings page
 *
 * Provides configuration options for the application including:
 * - Clipboard monitoring
 * - Download settings
 * - Connection settings
 * - Notification settings
 */
class SettingsPage : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPage(QWidget* parent = nullptr);
    ~SettingsPage() override = default;

    void set_clipboard_monitoring_enabled(bool enabled);
    void set_clipboard_detection_delay(int delay_ms);
    void set_default_download_dir(const QString& path);
    void set_max_concurrent_downloads(int count);
    void set_default_connections(int count);
    void set_connection_timeout(int seconds);
    void set_retry_count(int count);
    void set_notifications_enabled(bool enabled);
    void set_sound_notifications_enabled(bool enabled);
    void set_task_speed_limit(int kb_per_sec);
    void set_global_speed_limit(int kb_per_sec);
    void set_seed_ratio(double ratio);
    void set_seed_time_minutes(int minutes);
    void set_trash_retention_days(int days);
    void set_open_file_when_completed(bool enabled);
    void set_action_when_completed(int action);
    void set_theme_display(bool dark_mode);
    /// 任务列表视图（true=卡片视图，默认；与顶栏/页内切换共用同一份记忆）
    void set_task_view_grid(bool grid_view);
    void set_float_widget_enabled(bool enabled);
    void set_float_show_active_tasks(bool show);
    void set_float_show_total_progress(bool show);
    void set_float_size_preset(int preset);
    void set_float_opacity_percent(int percent);
    void set_float_click_through(bool enable);
    void set_daemon_mode_enabled(bool enabled);
    void set_daemon_rpc_url(const QString& url);
    void set_daemon_rpc_secret(const QString& secret);
    void set_check_updates_on_startup_enabled(bool enabled);

    /**
     * @brief Get clipboard monitoring enabled state
     * @return true if enabled
     */
    bool is_clipboard_monitoring_enabled() const;

    /**
     * @brief Get clipboard detection delay
     * @return Delay in milliseconds
     */
    int get_clipboard_detection_delay() const;

    /**
     * @brief Get default download directory
     * @return Directory path
     */
    QString get_default_download_dir() const;

    /**
     * @brief Get maximum concurrent downloads
     * @return Maximum number
     */
    int get_max_concurrent_downloads() const;

    /**
     * @brief Get default connection count per download
     * @return Connection count
     */
    int get_default_connections() const;

    int get_connection_timeout() const;

    int get_retry_count() const;

    /**
     * @brief Get whether to show notifications
     * @return true if notifications enabled
     */
    bool is_notifications_enabled() const;

    bool is_sound_notifications_enabled() const;

    /**
     * @brief Get task speed limit
     * @return Speed limit in KB/s (0 = unlimited)
     */
    int get_task_speed_limit() const;

    /**
     * @brief Get global speed limit
     * @return Speed limit in KB/s (0 = unlimited)
     */
    int get_global_speed_limit() const;

    /// 做种全局默认：份额比（新 BT 任务的 seed-ratio 缺省值）
    double get_seed_ratio() const;
    /// 做种全局默认：时长上限（分钟；0 = 不限时）
    int get_seed_time_minutes() const;

    /// 回收站自动清理保留天数（0 = 不自动清理）
    int get_trash_retention_days() const;

    /**
     * @brief Get whether to open file when download completes
     * @return true if should open file
     */
    bool is_open_file_when_completed() const;

    /**
     * @brief Get action to perform when download completes
     * @return Action index (0=none, 1=open file, 2=open folder, 3=notify only)
     */
    int get_action_when_completed() const;

    /// daemon RPC 模式开关（切换在下次启动应用时生效）
    bool is_daemon_mode_enabled() const;
    QString get_daemon_rpc_url() const;
    QString get_daemon_rpc_secret() const;

    /// 启动时静默检查更新（仅弹托盘通知，不打扰其余场景）
    bool is_check_updates_on_startup_enabled() const;

    /// 悬浮速度窗（开关/显示内容/大小档 0小1中2大/透明度 %/鼠标穿透）
    bool is_float_widget_enabled() const;
    bool is_float_show_active_tasks() const;
    bool is_float_show_total_progress() const;
    int get_float_size_preset() const;
    int get_float_opacity_percent() const;
    bool is_float_click_through() const;

    /// 当前任务列表视图是否为卡片（默认 true）
    bool is_task_view_grid() const;

signals:
    /**
     * @brief Signal emitted when settings are changed
     */
    void settings_changed();

    /**
     * @brief Signal emitted when clipboard monitoring setting is toggled
     * @param enabled true if monitoring should be enabled
     */
    void clipboard_monitoring_toggled(bool enabled);

    /**
     * @brief Signal emitted when float widget visibility setting is toggled
     * @param enabled true if the float window should be shown
     */
    void float_widget_toggled(bool enabled);

    /**
     * @brief Signal emitted when theme toggle is requested
     */
    void theme_toggle_requested();

    /**
     * @brief Signal emitted when the task list view style is changed here
     * @param grid_view true = 卡片视图
     */
    void task_view_grid_changed(bool grid_view);

private slots:
    /**
     * @brief Browse for default download directory
     */
    void browse_download_dir();

    /**
     * @brief Reset all settings to defaults
     */
    void reset_to_defaults();

    /**
     * @brief Apply settings
     */
    void apply_settings();

private:
    void setup_ui();
    QWidget* create_page_hero();
    QWidget* create_clipboard_section_widget();
    QWidget* create_download_section_widget();
    QWidget* create_speed_limit_section_widget();
    QWidget* create_seeding_section_widget();
    QWidget* create_trash_section_widget();
    QWidget* create_search_engines_section_widget();
    /// 重读 engines.json 重建引擎行（增删改后的单点刷新；勾选态以磁盘为准）
    void rebuild_search_engine_rows();
    void on_edit_search_engine(const QString& name);
    void on_delete_search_engine(const QString& name);
    void on_add_search_engine();
    QWidget* create_completion_action_section_widget();
    QWidget* create_connection_section_widget();
    QWidget* create_notification_section_widget();
    QWidget* create_float_widget_section_widget();
    QWidget* create_appearance_section_widget();
    QWidget* create_about_section_widget();
    QLayout* create_action_buttons_layout();

    void on_theme_button_clicked();

    // 资源搜索引擎行容器（engines.json 的只读投影 + 增删改入口）
    QWidget* search_engine_rows_host_ = nullptr;
    std::string search_config_path_;

    // Clipboard settings
    QCheckBox* clipboard_monitoring_checkbox_;
    QSpinBox* clipboard_delay_spin_;

    // Download settings
    QLineEdit* download_dir_edit_;
    QSpinBox* max_downloads_spin_;

    // Connection settings
    QSpinBox* default_connections_spin_;
    QSpinBox* connection_timeout_spin_;
    QSpinBox* retry_count_spin_;

    // Daemon RPC settings
    QCheckBox* daemon_enabled_checkbox_;
    QLineEdit* daemon_url_edit_;
    QLineEdit* daemon_secret_edit_;

    // Speed limit settings
    QSpinBox* task_speed_limit_spin_;
    QSpinBox* global_speed_limit_spin_;

    // Seeding defaults（BitTorrent 做种策略）
    QDoubleSpinBox* seed_ratio_spin_;
    QSpinBox* seed_time_spin_;

    // Trash retention（回收站自动清理）
    QSpinBox* trash_retention_spin_;

    // Completion action settings
    QComboBox* completion_action_combo_;

    // Notification settings
    QCheckBox* notifications_checkbox_;
    QCheckBox* sound_notification_checkbox_;

    // Floating speed widget（悬浮速度窗）
    QCheckBox* float_enabled_checkbox_;
    QCheckBox* float_show_active_checkbox_;
    QCheckBox* float_show_progress_checkbox_;
    QComboBox* float_size_combo_;
    QSpinBox* float_opacity_spin_;
    QCheckBox* float_click_through_checkbox_;

    // Appearance settings
    QLabel* current_theme_label_;
    QPushButton* theme_toggle_button_;
    QComboBox* task_view_combo_;

    // About & update settings
    QPushButton* check_updates_button_;
    QLabel* update_result_label_;
    QCheckBox* check_updates_on_startup_checkbox_;
    UpdateChecker* update_checker_;

    // Action buttons
    QPushButton* apply_button_;
    QPushButton* reset_button_;
};

} // namespace falcon::desktop
