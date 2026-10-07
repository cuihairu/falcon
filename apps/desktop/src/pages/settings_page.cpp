/**
 * @file settings_page.cpp
 * @brief Settings page implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "settings_page.hpp"

#include "../dialogs/search_engine_dialog.hpp"
#include "../services/search_engine_catalog.hpp"
#include "../services/update_checker.hpp"

#include <QFontMetrics>
#include <QMessageBox>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QLabel>
#include <QHeaderView>
#include <QScrollArea>
#include <QFrame>
#include <QStyle>

namespace falcon::desktop {

//==============================================================================
// Constructor / Destructor
//==============================================================================

SettingsPage::SettingsPage(QWidget* parent)
    : QWidget(parent)
    , clipboard_monitoring_checkbox_(nullptr)
    , clipboard_delay_spin_(nullptr)
    , download_dir_edit_(nullptr)
    , max_downloads_spin_(nullptr)
    , default_connections_spin_(nullptr)
    , connection_timeout_spin_(nullptr)
    , retry_count_spin_(nullptr)
    , daemon_enabled_checkbox_(nullptr)
    , daemon_url_edit_(nullptr)
    , daemon_secret_edit_(nullptr)
    , task_speed_limit_spin_(nullptr)
    , global_speed_limit_spin_(nullptr)
    , completion_action_combo_(nullptr)
    , completed_double_click_combo_(nullptr)
    , notifications_checkbox_(nullptr)
    , sound_notification_checkbox_(nullptr)
    , float_enabled_checkbox_(nullptr)
    , float_show_active_checkbox_(nullptr)
    , float_show_progress_checkbox_(nullptr)
    , float_size_combo_(nullptr)
    , float_opacity_spin_(nullptr)
    , float_click_through_checkbox_(nullptr)
    , current_theme_label_(nullptr)
    , theme_toggle_button_(nullptr)
    , task_view_combo_(nullptr)
    , check_updates_button_(nullptr)
    , update_result_label_(nullptr)
    , check_updates_on_startup_checkbox_(nullptr)
    , update_checker_(nullptr)
    , apply_button_(nullptr)
    , reset_button_(nullptr)
{
    setup_ui();
}

//==============================================================================
// Getters
//==============================================================================

void SettingsPage::set_clipboard_monitoring_enabled(bool enabled)
{
    clipboard_monitoring_checkbox_->setChecked(enabled);
}

void SettingsPage::set_clipboard_detection_delay(int delay_ms)
{
    clipboard_delay_spin_->setValue(delay_ms);
}

void SettingsPage::set_default_download_dir(const QString& path)
{
    download_dir_edit_->setText(path);
}

void SettingsPage::set_max_concurrent_downloads(int count)
{
    max_downloads_spin_->setValue(count);
}

void SettingsPage::set_default_connections(int count)
{
    default_connections_spin_->setValue(count);
}

void SettingsPage::set_notifications_enabled(bool enabled)
{
    notifications_checkbox_->setChecked(enabled);
}

void SettingsPage::set_sound_notifications_enabled(bool enabled)
{
    sound_notification_checkbox_->setChecked(enabled);
}

void SettingsPage::set_float_widget_enabled(bool enabled)
{
    float_enabled_checkbox_->setChecked(enabled);
}

void SettingsPage::set_float_show_active_tasks(bool show)
{
    float_show_active_checkbox_->setChecked(show);
}

void SettingsPage::set_float_show_total_progress(bool show)
{
    float_show_progress_checkbox_->setChecked(show);
}

void SettingsPage::set_float_size_preset(int preset)
{
    float_size_combo_->setCurrentIndex(qBound(0, preset, 2));
}

void SettingsPage::set_float_opacity_percent(int percent)
{
    float_opacity_spin_->setValue(qBound(30, percent, 100));
}

void SettingsPage::set_float_click_through(bool enable)
{
    float_click_through_checkbox_->setChecked(enable);
}

void SettingsPage::set_task_speed_limit(int kb_per_sec)
{
    task_speed_limit_spin_->setValue(kb_per_sec);
}

void SettingsPage::set_global_speed_limit(int kb_per_sec)
{
    global_speed_limit_spin_->setValue(kb_per_sec);
}

void SettingsPage::set_seed_ratio(double ratio)
{
    seed_ratio_spin_->setValue(ratio < 0.0 ? 1.0 : ratio);
}

void SettingsPage::set_seed_time_minutes(int minutes)
{
    seed_time_spin_->setValue(minutes < 0 ? 0 : minutes);
}

void SettingsPage::set_trash_retention_days(int days)
{
    trash_retention_spin_->setValue(days < 0 ? 7 : days);
}

void SettingsPage::set_open_file_when_completed(bool enabled)
{
    set_action_when_completed(enabled ? 1 : 0);
}

void SettingsPage::set_action_when_completed(int action)
{
    if (completion_action_combo_) {
        completion_action_combo_->setCurrentIndex(action);
    }
}

void SettingsPage::set_completed_double_click_action(int action)
{
    if (completed_double_click_combo_) {
        completed_double_click_combo_->setCurrentIndex(
            qBound(0, action, completed_double_click_combo_->count() - 1));
    }
}

void SettingsPage::set_theme_display(bool dark_mode)
{
    if (current_theme_label_) {
        current_theme_label_->setText(dark_mode ? tr("深色") : tr("浅色"));
    }
    if (theme_toggle_button_) {
        theme_toggle_button_->setText(dark_mode ? tr("切换浅色") : tr("切换深色"));
    }
}

void SettingsPage::set_daemon_mode_enabled(bool enabled)
{
    if (daemon_enabled_checkbox_) {
        daemon_enabled_checkbox_->setChecked(enabled);
    }
}

void SettingsPage::set_daemon_rpc_url(const QString& url)
{
    if (daemon_url_edit_) {
        daemon_url_edit_->setText(url);
    }
}

void SettingsPage::set_daemon_rpc_secret(const QString& secret)
{
    if (daemon_secret_edit_) {
        daemon_secret_edit_->setText(secret);
    }
}

void SettingsPage::set_check_updates_on_startup_enabled(bool enabled)
{
    if (check_updates_on_startup_checkbox_) {
        check_updates_on_startup_checkbox_->setChecked(enabled);
    }
}

bool SettingsPage::is_clipboard_monitoring_enabled() const
{
    return clipboard_monitoring_checkbox_->isChecked();
}

int SettingsPage::get_clipboard_detection_delay() const
{
    return clipboard_delay_spin_->value();
}

QString SettingsPage::get_default_download_dir() const
{
    return download_dir_edit_->text();
}

int SettingsPage::get_max_concurrent_downloads() const
{
    return max_downloads_spin_->value();
}

int SettingsPage::get_default_connections() const
{
    return default_connections_spin_->value();
}

int SettingsPage::get_connection_timeout() const
{
    return connection_timeout_spin_->value();
}

int SettingsPage::get_retry_count() const
{
    return retry_count_spin_->value();
}

void SettingsPage::set_connection_timeout(int seconds)
{
    connection_timeout_spin_->setValue(seconds);
}

void SettingsPage::set_retry_count(int count)
{
    retry_count_spin_->setValue(count);
}

bool SettingsPage::is_notifications_enabled() const
{
    return notifications_checkbox_->isChecked();
}

bool SettingsPage::is_sound_notifications_enabled() const
{
    return sound_notification_checkbox_->isChecked();
}

bool SettingsPage::is_float_widget_enabled() const
{
    return float_enabled_checkbox_->isChecked();
}

bool SettingsPage::is_float_show_active_tasks() const
{
    return float_show_active_checkbox_->isChecked();
}

bool SettingsPage::is_float_show_total_progress() const
{
    return float_show_progress_checkbox_->isChecked();
}

int SettingsPage::get_float_size_preset() const
{
    return float_size_combo_->currentIndex();
}

int SettingsPage::get_float_opacity_percent() const
{
    return float_opacity_spin_->value();
}

bool SettingsPage::is_float_click_through() const
{
    return float_click_through_checkbox_->isChecked();
}

int SettingsPage::get_task_speed_limit() const
{
    return task_speed_limit_spin_->value();
}

int SettingsPage::get_global_speed_limit() const
{
    return global_speed_limit_spin_->value();
}

double SettingsPage::get_seed_ratio() const
{
    return seed_ratio_spin_->value();
}

int SettingsPage::get_seed_time_minutes() const
{
    return seed_time_spin_->value();
}

int SettingsPage::get_trash_retention_days() const
{
    return trash_retention_spin_->value();
}

bool SettingsPage::is_open_file_when_completed() const
{
    return completion_action_combo_ ? completion_action_combo_->currentIndex() == 1 : false;
}

int SettingsPage::get_action_when_completed() const
{
    return completion_action_combo_ ? completion_action_combo_->currentIndex() : 0;
}

int SettingsPage::get_completed_double_click_action() const
{
    return completed_double_click_combo_ ? completed_double_click_combo_->currentIndex() : 0;
}

bool SettingsPage::is_daemon_mode_enabled() const
{
    return daemon_enabled_checkbox_ ? daemon_enabled_checkbox_->isChecked() : false;
}

QString SettingsPage::get_daemon_rpc_url() const
{
    return daemon_url_edit_ ? daemon_url_edit_->text().trimmed()
                            : QString("http://127.0.0.1:6800/jsonrpc");
}

QString SettingsPage::get_daemon_rpc_secret() const
{
    return daemon_secret_edit_ ? daemon_secret_edit_->text() : QString();
}

bool SettingsPage::is_check_updates_on_startup_enabled() const
{
    return check_updates_on_startup_checkbox_
               ? check_updates_on_startup_checkbox_->isChecked()
               : false;
}

//==============================================================================
// Private Slots
//==============================================================================

void SettingsPage::browse_download_dir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("选择默认下载目录"),
        download_dir_edit_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty()) {
        download_dir_edit_->setText(dir);
    }
}

void SettingsPage::reset_to_defaults()
{
    // Clipboard settings（默认开启，与 load_settings 出厂默认一致——
    // 主流下载器语义）
    clipboard_monitoring_checkbox_->setChecked(true);
    clipboard_delay_spin_->setValue(1000);

    // Download settings
    download_dir_edit_->setText(QDir::homePath() + "/Downloads");
    max_downloads_spin_->setValue(3);

    // Speed limit settings (0 = unlimited)
    task_speed_limit_spin_->setValue(0);
    global_speed_limit_spin_->setValue(0);

    // Seeding defaults（aria2 同语义：ratio 1.0 / 不限时）
    seed_ratio_spin_->setValue(1.0);
    seed_time_spin_->setValue(0);

    // Trash defaults（保留 7 天自动清理）
    trash_retention_spin_->setValue(7);

    // Completion action settings (0 = do nothing)
    completion_action_combo_->setCurrentIndex(0);

    // B18: 已完成双击行为（默认打开文件）
    completed_double_click_combo_->setCurrentIndex(0);

    // Connection settings
    default_connections_spin_->setValue(4);
    connection_timeout_spin_->setValue(30);
    retry_count_spin_->setValue(3);

    // Daemon RPC settings（默认进程内引擎）
    daemon_enabled_checkbox_->setChecked(false);
    daemon_url_edit_->setText("http://127.0.0.1:6800/jsonrpc");
    daemon_secret_edit_->setText("");

    // Notification settings
    notifications_checkbox_->setChecked(true);
    sound_notification_checkbox_->setChecked(false);

    // Floating speed widget defaults（悬浮速度窗：开/全显示/中/90%/不穿透）
    float_enabled_checkbox_->setChecked(true);
    float_show_active_checkbox_->setChecked(true);
    float_show_progress_checkbox_->setChecked(true);
    float_size_combo_->setCurrentIndex(0); // B21①: 默认紧凑档 = 小
    float_opacity_spin_->setValue(90);
    float_click_through_checkbox_->setChecked(false);

    // About & update settings
    check_updates_on_startup_checkbox_->setChecked(true);

    // 任务列表视图（默认卡片）——set_task_view_grid 程序化回写不发信号，
    // 恢复默认需显式回发让下载页即时回到卡片视图
    if (task_view_combo_ && task_view_combo_->currentIndex() != 0) {
        set_task_view_grid(true);
        emit task_view_grid_changed(true);
    }
}

void SettingsPage::apply_settings()
{
    emit settings_changed();
    emit clipboard_monitoring_toggled(clipboard_monitoring_checkbox_->isChecked());
}

//==============================================================================
// Private Methods
//==============================================================================

void SettingsPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(18, 18, 18, 18);
    main_layout->setSpacing(14);

    main_layout->addWidget(create_page_hero());

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* scroll_content = new QWidget(scroll_area);
    auto* scroll_layout = new QVBoxLayout(scroll_content);
    scroll_layout->setSpacing(16);
    scroll_layout->setContentsMargins(0, 0, 0, 0);

    // Create setting sections
    scroll_layout->addWidget(create_appearance_section_widget());
    scroll_layout->addWidget(create_clipboard_section_widget());
    scroll_layout->addWidget(create_download_section_widget());
    scroll_layout->addWidget(create_speed_limit_section_widget());
    scroll_layout->addWidget(create_seeding_section_widget());
    scroll_layout->addWidget(create_trash_section_widget());
    scroll_layout->addWidget(create_search_engines_section_widget());
    scroll_layout->addWidget(create_completion_action_section_widget());
    scroll_layout->addWidget(create_connection_section_widget());
    scroll_layout->addWidget(create_notification_section_widget());
    scroll_layout->addWidget(create_float_widget_section_widget());
    scroll_layout->addWidget(create_about_section_widget());

    scroll_layout->addStretch();

    // Action buttons
    scroll_layout->addLayout(create_action_buttons_layout());

    scroll_area->setWidget(scroll_content);
    main_layout->addWidget(scroll_area, 1);
}

QWidget* SettingsPage::create_page_hero()
{
    auto* hero = new QWidget(this);
    hero->setObjectName("downloadHero");

    auto* layout = new QVBoxLayout(hero);
    layout->setContentsMargins(20, 18, 20, 18);
    layout->setSpacing(4);

    auto* title = new QLabel(tr("下载器设置"), hero);
    title->setObjectName("heroTitle");
    layout->addWidget(title);

    auto* desc = new QLabel(tr("统一管理主题、连接、限速、通知与下载目录。"), hero);
    desc->setObjectName("heroDescription");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    return hero;
}

QWidget* SettingsPage::create_clipboard_section_widget()
{
    auto* group = new QGroupBox(tr("剪切板监听"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // Enable monitoring checkbox
    clipboard_monitoring_checkbox_ = new QCheckBox(tr("自动检测剪切板链接"), this);
    // 勾选即时生效（沿搜索引擎复选框范式）：不经「应用」按钮立即启停监听；
    // 持久化由 MainWindow 的 toggled 处理器即时落盘（悬浮窗位置同款范式）
    connect(clipboard_monitoring_checkbox_, &QCheckBox::toggled, this,
            [this](bool checked) { emit clipboard_monitoring_toggled(checked); });

    auto* desc_label = new QLabel(
        tr("自动从剪切板检测下载链接（HTTP、FTP、磁力链等）。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");

    layout->addWidget(clipboard_monitoring_checkbox_);
    layout->addWidget(desc_label);

    // Detection delay
    auto* delay_layout = new QHBoxLayout();
    auto* delay_label = new QLabel(tr("检测间隔:"), this);

    clipboard_delay_spin_ = new QSpinBox(this);
    clipboard_delay_spin_->setRange(500, 10000);
    clipboard_delay_spin_->setValue(1000);
    clipboard_delay_spin_->setSuffix(" ms");

    auto* delay_hint = new QLabel(tr("（避免重复触发）"), this);
    delay_hint->setObjectName("cardInfoLabel");

    delay_layout->addWidget(delay_label);
    delay_layout->addWidget(clipboard_delay_spin_);
    delay_layout->addWidget(delay_hint);
    delay_layout->addStretch();

    layout->addLayout(delay_layout);

    return group;
}

QWidget* SettingsPage::create_download_section_widget()
{
    auto* group = new QGroupBox(tr("下载"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default download directory
    auto* dir_label = new QLabel(tr("默认下载目录:"), this);

    auto* dir_layout = new QHBoxLayout();
    dir_layout->setSpacing(8);
    download_dir_edit_ = new QLineEdit(QDir::homePath() + "/Downloads", this);
    download_dir_edit_->setReadOnly(true);
    dir_layout->addWidget(download_dir_edit_, 1);

    auto* browse_btn = new QPushButton(tr("浏览..."), this);
    browse_btn->setCursor(Qt::PointingHandCursor);
    browse_btn->setObjectName("toolButton");
    connect(browse_btn, &QPushButton::clicked, this, &SettingsPage::browse_download_dir);
    dir_layout->addWidget(browse_btn);

    layout->addRow(dir_label, dir_layout);

    // Maximum concurrent downloads
    auto* max_label = new QLabel(tr("最大并发下载数:"), this);
    auto* max_layout = new QHBoxLayout();
    max_layout->setSpacing(8);

    max_downloads_spin_ = new QSpinBox(this);
    max_downloads_spin_->setRange(1, 10);
    max_downloads_spin_->setValue(3);
    max_layout->addWidget(max_downloads_spin_);
    max_layout->addStretch();

    layout->addRow(max_label, max_layout);

    return group;
}

QWidget* SettingsPage::create_speed_limit_section_widget()
{
    auto* group = new QGroupBox(tr("限速"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Task speed limit
    auto* task_limit_label = new QLabel(tr("单任务限速:"), this);
    auto* task_limit_layout = new QHBoxLayout();
    task_limit_layout->setSpacing(8);

    task_speed_limit_spin_ = new QSpinBox(this);
    task_speed_limit_spin_->setRange(0, 100000);
    task_speed_limit_spin_->setValue(0);
    task_speed_limit_spin_->setSuffix(" KB/s");
    task_speed_limit_spin_->setSpecialValueText(tr("不限速"));
    task_limit_layout->addWidget(task_speed_limit_spin_);

    auto* task_limit_hint = new QLabel(tr("（0 = 不限速）"), this);
    task_limit_hint->setObjectName("cardInfoLabel");
    task_limit_layout->addWidget(task_limit_hint);
    task_limit_layout->addStretch();

    layout->addRow(task_limit_label, task_limit_layout);

    // Global speed limit
    auto* global_limit_label = new QLabel(tr("全局限速:"), this);
    auto* global_limit_layout = new QHBoxLayout();
    global_limit_layout->setSpacing(8);

    global_speed_limit_spin_ = new QSpinBox(this);
    global_speed_limit_spin_->setRange(0, 100000);
    global_speed_limit_spin_->setValue(0);
    global_speed_limit_spin_->setSuffix(" KB/s");
    global_speed_limit_spin_->setSpecialValueText(tr("不限速"));
    global_limit_layout->addWidget(global_speed_limit_spin_);

    auto* global_limit_hint = new QLabel(tr("（0 = 不限速）"), this);
    global_limit_hint->setObjectName("cardInfoLabel");
    global_limit_layout->addWidget(global_limit_hint);
    global_limit_layout->addStretch();

    layout->addRow(global_limit_label, global_limit_layout);

    // 说明文字
    auto* desc_label = new QLabel(
        tr("限速有助于控制带宽占用；全局限速对所有下载任务生效。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addRow("", desc_label);

    return group;
}

QWidget* SettingsPage::create_seeding_section_widget()
{
    auto* group = new QGroupBox(tr("做种（BitTorrent）"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // seed-ratio 默认值
    auto* ratio_label = new QLabel(tr("做种份额比:"), this);
    auto* ratio_layout = new QHBoxLayout();
    ratio_layout->setSpacing(8);

    seed_ratio_spin_ = new QDoubleSpinBox(this);
    seed_ratio_spin_->setRange(0.0, 999.0);
    seed_ratio_spin_->setSingleStep(0.05);
    seed_ratio_spin_->setDecimals(2);
    seed_ratio_spin_->setValue(1.0);
    ratio_layout->addWidget(seed_ratio_spin_);

    auto* ratio_hint = new QLabel(tr("（0 = 不按份额停止）"), this);
    ratio_hint->setObjectName("cardInfoLabel");
    ratio_layout->addWidget(ratio_hint);
    ratio_layout->addStretch();

    layout->addRow(ratio_label, ratio_layout);

    // seed-time 默认值（分钟）
    auto* time_label = new QLabel(tr("做种时长:"), this);
    auto* time_layout = new QHBoxLayout();
    time_layout->setSpacing(8);

    seed_time_spin_ = new QSpinBox(this);
    seed_time_spin_->setRange(0, 525600);
    seed_time_spin_->setValue(0);
    seed_time_spin_->setSuffix(tr(" 分钟"));
    seed_time_spin_->setSpecialValueText(tr("不限时"));
    time_layout->addWidget(seed_time_spin_);

    auto* time_hint = new QLabel(tr("（0 = 不限时，仅按份额比停止）"), this);
    time_hint->setObjectName("cardInfoLabel");
    time_layout->addWidget(time_hint);
    time_layout->addStretch();

    layout->addRow(time_label, time_layout);

    // 说明文字
    auto* desc_label = new QLabel(
        tr("做种默认值对新 BitTorrent 任务生效；满足任一条件（份额比或时长）即停止做种。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addRow("", desc_label);

    return group;
}

QWidget* SettingsPage::create_trash_section_widget()
{
    auto* group = new QGroupBox(tr("回收站"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // 保留天数
    auto* days_label = new QLabel(tr("文件保留:"), this);
    auto* days_layout = new QHBoxLayout();
    days_layout->setSpacing(8);

    trash_retention_spin_ = new QSpinBox(this);
    trash_retention_spin_->setRange(0, 365);
    trash_retention_spin_->setValue(7);
    trash_retention_spin_->setSuffix(tr(" 天"));
    trash_retention_spin_->setSpecialValueText(tr("不自动清理"));
    days_layout->addWidget(trash_retention_spin_);

    auto* days_hint = new QLabel(tr("（0 = 不自动清理，仅手动清空）"), this);
    days_hint->setObjectName("cardInfoLabel");
    days_layout->addWidget(days_hint);
    days_layout->addStretch();

    layout->addRow(days_label, days_layout);

    // 说明文字
    auto* desc_label = new QLabel(
        tr("删除任务时，成品文件移入下载目录的 .falcon-trash 暂存；超过保留天数后启动时自动清理，期间可随时恢复。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addRow("", desc_label);

    return group;
}

QWidget* SettingsPage::create_search_engines_section_widget()
{
    auto* group = new QGroupBox(tr("资源搜索"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(12);
    layout->setContentsMargins(16, 8, 16, 16);

    // 首次使用生成全 disabled 示例模板（已有配置绝不覆盖）
    search_config_path_ = SearchEngineCatalog::default_path();
    SearchEngineCatalog::ensure_default(search_config_path_);

    // 引擎行容器（rebuild_search_engine_rows 单点重建；勾选态以磁盘为准，
    // 与发现页搜索同读一份文件）
    search_engine_rows_host_ = new QWidget(this);
    auto* rows_layout = new QVBoxLayout(search_engine_rows_host_);
    rows_layout->setContentsMargins(0, 0, 0, 0);
    rows_layout->setSpacing(8);
    layout->addWidget(search_engine_rows_host_);
    rebuild_search_engine_rows();

    // 说明文字
    auto* desc_label = new QLabel(
        tr("搜索引擎由 engines.json 配置驱动（正则规则解析页面结果），Falcon 不内置任何第三方站点；此处增删改与勾选均即时生效（写盘 + 下次搜索生效）。"),
        this);
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    // 操作行：添加引擎 + 配置路径（文本可选中，方便高级编辑）
    auto* action_row = new QHBoxLayout;
    auto* add_button = new QPushButton(tr("添加引擎"), this);
    add_button->setObjectName("primaryButton");
    connect(add_button, &QPushButton::clicked,
            this, &SettingsPage::on_add_search_engine);
    action_row->addWidget(add_button);
    action_row->addStretch();
    auto* path_label = new QLabel(
        tr("配置文件: %1").arg(QString::fromStdString(search_config_path_)), this);
    path_label->setWordWrap(true);
    path_label->setObjectName("cardInfoLabel");
    path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    action_row->addWidget(path_label, 1);
    layout->addLayout(action_row);

    return group;
}

void SettingsPage::rebuild_search_engine_rows()
{
    if (!search_engine_rows_host_) {
        return;
    }
    auto* rows_layout =
        static_cast<QVBoxLayout*>(search_engine_rows_host_->layout());
    while (auto* item = rows_layout->takeAt(0)) {
        if (auto* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    SearchEngineCatalog catalog(search_config_path_);
    if (!catalog.load()) {
        auto* err_label = new QLabel(
            tr("engines.json 无法解析（格式无效），请手工修正文件内容后重试（勾选/编辑等操作会给出提示）。"),
            this);
        err_label->setWordWrap(true);
        err_label->setObjectName("cardInfoLabel");
        rows_layout->addWidget(err_label);
        return;
    }

    if (catalog.engines().empty()) {
        auto* empty_label = new QLabel(
            tr("暂无搜索源——点下方「添加引擎」按表单填写，无需手工编辑配置文件。"),
            this);
        empty_label->setWordWrap(true);
        empty_label->setObjectName("cardInfoLabel");
        rows_layout->addWidget(empty_label);
    }

    const QFontMetrics metrics(font());
    for (const auto& engine : catalog.engines()) {
        const QString engine_name = QString::fromStdString(engine.name);
        auto* row = new QWidget(search_engine_rows_host_);
        auto* row_layout = new QHBoxLayout(row);
        row_layout->setContentsMargins(0, 0, 0, 0);
        row_layout->setSpacing(8);

        // 勾选 = 启停（set_enabled 单键翻转，其余字段原样保留）
        auto* box = new QCheckBox(engine_name, row);
        box->setChecked(engine.enabled);
        box->setToolTip(tr("%1\n勾选即启用该引擎（即时生效，下次搜索可见）。")
                            .arg(QString::fromStdString(engine.base_url)));
        connect(box, &QCheckBox::toggled, this,
                [path = search_config_path_, engine_name](bool checked) {
                    SearchEngineCatalog(path)
                        .set_enabled(engine_name.toStdString(), checked);
                });
        row_layout->addWidget(box);

        const QString base_url = QString::fromStdString(engine.base_url);
        auto* url_label = new QLabel(
            metrics.elidedText(base_url, Qt::ElideMiddle, 280), row);
        url_label->setObjectName("cardInfoLabel");
        url_label->setToolTip(base_url);
        row_layout->addWidget(url_label, 1);

        auto* edit_button = new QPushButton(tr("编辑"), row);
        edit_button->setObjectName("rowActionButton");
        connect(edit_button, &QPushButton::clicked, this,
                [this, engine_name] { on_edit_search_engine(engine_name); });
        row_layout->addWidget(edit_button);

        auto* delete_button = new QPushButton(tr("删除"), row);
        delete_button->setObjectName("rowActionButton");
        connect(delete_button, &QPushButton::clicked, this,
                [this, engine_name] { on_delete_search_engine(engine_name); });
        row_layout->addWidget(delete_button);

        rows_layout->addWidget(row);
    }
    rows_layout->addStretch();
}

void SettingsPage::on_add_search_engine()
{
    SearchEngineCatalog catalog(search_config_path_);
    QStringList existing_names;
    if (catalog.load()) {
        for (const auto& engine : catalog.engines()) {
            existing_names << QString::fromStdString(engine.name);
        }
    }
    SearchEngineDialog dialog(tr("添加搜索引擎"), CatalogEngine{},
                              existing_names, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    if (!SearchEngineCatalog(search_config_path_).upsert_engine(dialog.engine())) {
        QMessageBox::warning(this, tr("添加失败"),
                             tr("engines.json 无法解析或写入失败，请手工检查该文件。"));
        return;
    }
    rebuild_search_engine_rows();
}

void SettingsPage::on_edit_search_engine(const QString& name)
{
    SearchEngineCatalog catalog(search_config_path_);
    if (!catalog.load()) {
        QMessageBox::warning(this, tr("编辑失败"),
                             tr("engines.json 无法解析，请手工修正后再试。"));
        return;
    }
    const CatalogEngine* found = nullptr;
    for (const auto& engine : catalog.engines()) {
        if (engine.name == name.toStdString()) {
            found = &engine;
            break;
        }
    }
    if (!found) {
        rebuild_search_engine_rows();
        return;
    }
    const CatalogEngine original = *found;
    QStringList existing_names;
    for (const auto& engine : catalog.engines()) {
        if (engine.name != original.name) {
            existing_names << QString::fromStdString(engine.name);
        }
    }
    SearchEngineDialog dialog(tr("编辑搜索引擎"), original, existing_names, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    const CatalogEngine updated = dialog.engine();
    if (updated.name != original.name
        && !SearchEngineCatalog(search_config_path_).remove_engine(original.name)) {
        QMessageBox::warning(this, tr("编辑失败"),
                             tr("engines.json 无法解析或写入失败，请手工检查该文件。"));
        return;
    }
    if (!SearchEngineCatalog(search_config_path_).upsert_engine(updated)) {
        QMessageBox::warning(this, tr("编辑失败"),
                             tr("engines.json 无法解析或写入失败，请手工检查该文件。"));
        return;
    }
    rebuild_search_engine_rows();
}

void SettingsPage::on_delete_search_engine(const QString& name)
{
    const auto answer = QMessageBox::question(
        this, tr("删除搜索引擎"),
        tr("确定删除「%1」吗？将直接从 engines.json 移除该条目。").arg(name));
    if (answer != QMessageBox::Yes) {
        return;
    }
    if (!SearchEngineCatalog(search_config_path_).remove_engine(name.toStdString())) {
        QMessageBox::warning(this, tr("删除失败"),
                             tr("engines.json 无法解析、写入失败或条目已不存在，请手工检查该文件。"));
        rebuild_search_engine_rows();
        return;
    }
    rebuild_search_engine_rows();
}

QWidget* SettingsPage::create_completion_action_section_widget()
{
    auto* group = new QGroupBox(tr("下载完成后"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // Action selection
    auto* action_layout = new QHBoxLayout();
    action_layout->setSpacing(12);

    auto* action_label = new QLabel(tr("完成后动作:"), this);
    action_layout->addWidget(action_label);

    completion_action_combo_ = new QComboBox(this);
    completion_action_combo_->addItem(tr("无操作"));
    completion_action_combo_->addItem(tr("打开文件"));
    completion_action_combo_->addItem(tr("打开文件夹"));
    completion_action_combo_->addItem(tr("仅显示通知"));
    action_layout->addWidget(completion_action_combo_);

    action_layout->addStretch();
    layout->addLayout(action_layout);

    // B18: 已完成任务双击行为（默认打开文件，可切换为打开所在文件夹）
    auto* dbl_layout = new QHBoxLayout();
    dbl_layout->setSpacing(12);

    auto* dbl_label = new QLabel(tr("已完成双击:"), this);
    dbl_layout->addWidget(dbl_label);

    completed_double_click_combo_ = new QComboBox(this);
    completed_double_click_combo_->addItem(tr("打开文件（默认）"));
    completed_double_click_combo_->addItem(tr("打开所在文件夹"));
    dbl_layout->addWidget(completed_double_click_combo_);

    dbl_layout->addStretch();
    layout->addLayout(dbl_layout);

    // 说明文字
    auto* desc_label = new QLabel(
        tr("选择下载完成后的动作；多文件任务仅显示通知。「已完成双击」决定在下载页双击已完成任务时打开文件还是所在文件夹，改动立即生效。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    // B18: 双击行为变更即时上抛（MainWindow 落 QSettings；下载页双击时
    // 即时读取，无需向下载页回写）
    connect(completed_double_click_combo_, &QComboBox::currentIndexChanged,
            this, [this](int index) { emit completed_double_click_changed(index); });

    return group;
}

QWidget* SettingsPage::create_connection_section_widget()
{
    auto* group = new QGroupBox(tr("连接"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default connections
    auto* conn_label = new QLabel(tr("默认连接数:"), this);
    default_connections_spin_ = new QSpinBox(this);
    default_connections_spin_->setRange(1, 16);
    default_connections_spin_->setValue(4);
    layout->addRow(conn_label, default_connections_spin_);

    // Connection timeout
    auto* timeout_label = new QLabel(tr("连接超时:"), this);
    connection_timeout_spin_ = new QSpinBox(this);
    connection_timeout_spin_->setRange(5, 120);
    connection_timeout_spin_->setValue(30);
    connection_timeout_spin_->setSuffix(" s");
    layout->addRow(timeout_label, connection_timeout_spin_);

    // Retry count
    auto* retry_label = new QLabel(tr("重试次数:"), this);
    retry_count_spin_ = new QSpinBox(this);
    retry_count_spin_->setRange(0, 10);
    retry_count_spin_->setValue(3);
    layout->addRow(retry_label, retry_count_spin_);

    // Daemon RPC 模式：经 aria2 兼容 JSON-RPC 连接 falcon-daemon。
    // 后端在应用启动时创建，切换需重启应用生效。
    auto* daemon_label = new QLabel(tr("Daemon 模式:"), this);
    daemon_enabled_checkbox_ = new QCheckBox(
        tr("连接 falcon-daemon（重启后生效）"), this);
    layout->addRow(daemon_label, daemon_enabled_checkbox_);

    auto* daemon_url_label = new QLabel(tr("Daemon RPC URL:"), this);
    daemon_url_edit_ = new QLineEdit(this);
    daemon_url_edit_->setPlaceholderText("http://127.0.0.1:6800/jsonrpc");
    layout->addRow(daemon_url_label, daemon_url_edit_);

    auto* daemon_secret_label = new QLabel(tr("Daemon RPC 密钥:"), this);
    daemon_secret_edit_ = new QLineEdit(this);
    daemon_secret_edit_->setEchoMode(QLineEdit::Password);
    daemon_secret_edit_->setPlaceholderText(tr("token 密钥（可选）"));
    layout->addRow(daemon_secret_label, daemon_secret_edit_);

    return group;
}

QWidget* SettingsPage::create_notification_section_widget()
{
    auto* group = new QGroupBox(tr("通知"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    notifications_checkbox_ = new QCheckBox(tr("启用通知"), this);
    notifications_checkbox_->setChecked(true);
    layout->addWidget(notifications_checkbox_);

    sound_notification_checkbox_ = new QCheckBox(tr("提示音"), this);
    layout->addWidget(sound_notification_checkbox_);

    return group;
}

QWidget* SettingsPage::create_float_widget_section_widget()
{
    auto* group = new QGroupBox(tr("悬浮速度窗"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    float_enabled_checkbox_ = new QCheckBox(
        tr("显示悬浮速度窗（始终置顶，可拖动到任意位置）"), this);
    float_enabled_checkbox_->setChecked(true);
    // 勾选即时生效（沿剪切板监听范式）：不经「应用」按钮立即显隐；
    // 持久化与显隐由 MainWindow 的 toggled 处理器收口（B21③）
    connect(float_enabled_checkbox_, &QCheckBox::toggled, this,
            [this](bool checked) { emit float_widget_toggled(checked); });
    layout->addWidget(float_enabled_checkbox_);

    float_show_active_checkbox_ = new QCheckBox(tr("显示活跃任务数"), this);
    float_show_active_checkbox_->setChecked(true);
    layout->addWidget(float_show_active_checkbox_);

    float_show_progress_checkbox_ = new QCheckBox(tr("显示总进度"), this);
    float_show_progress_checkbox_->setChecked(true);
    layout->addWidget(float_show_progress_checkbox_);

    auto* size_row = new QHBoxLayout();
    size_row->addWidget(new QLabel(tr("大小:"), this));
    float_size_combo_ = new QComboBox(this);
    float_size_combo_->addItem(tr("小"));
    float_size_combo_->addItem(tr("中"));
    float_size_combo_->addItem(tr("大"));
    float_size_combo_->setCurrentIndex(0); // B21①: 默认紧密档 = 小
    size_row->addWidget(float_size_combo_);
    size_row->addStretch();
    layout->addLayout(size_row);

    auto* opacity_row = new QHBoxLayout();
    opacity_row->addWidget(new QLabel(tr("透明度:"), this));
    float_opacity_spin_ = new QSpinBox(this);
    float_opacity_spin_->setRange(30, 100);
    float_opacity_spin_->setSuffix(tr(" %"));
    float_opacity_spin_->setValue(90);
    opacity_row->addWidget(float_opacity_spin_);
    opacity_row->addStretch();
    layout->addLayout(opacity_row);

    float_click_through_checkbox_ = new QCheckBox(
        tr("鼠标穿透（开启后点击会穿过悬浮窗，需回到此处关闭）"), this);
    layout->addWidget(float_click_through_checkbox_);

    return group;
}

QWidget* SettingsPage::create_about_section_widget()
{
    auto* group = new QGroupBox(tr("关于与更新"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // 版本行：当前版本 + 检查按钮
    auto* version_layout = new QHBoxLayout();
    version_layout->setSpacing(12);

    version_layout->addWidget(new QLabel(tr("当前版本:"), this));

    auto* version_label = new QLabel(UpdateChecker::current_version(), this);
    version_layout->addWidget(version_label);

    version_layout->addStretch();

    check_updates_button_ = new QPushButton(tr("检查更新"), this);
    check_updates_button_->setCursor(Qt::PointingHandCursor);
    check_updates_button_->setObjectName("toolButton");
    version_layout->addWidget(check_updates_button_);

    layout->addLayout(version_layout);

    // 检查结果（链接跳 GitHub 发布页——无自更新，aria2 同姿态）
    update_result_label_ = new QLabel(tr("尚未检查更新。"), this);
    update_result_label_->setObjectName("cardInfoLabel");
    update_result_label_->setWordWrap(true);
    update_result_label_->setTextInteractionFlags(Qt::TextBrowserInteraction);
    update_result_label_->setOpenExternalLinks(true);
    layout->addWidget(update_result_label_);

    check_updates_on_startup_checkbox_ = new QCheckBox(tr("启动时自动检查更新"), this);
    check_updates_on_startup_checkbox_->setChecked(true);
    layout->addWidget(check_updates_on_startup_checkbox_);

    auto* desc_label = new QLabel(
        tr("发现新版本时提供 GitHub 发布页链接，应用不会自动下载或安装更新。"), this);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    // 页面内自持一个检查器（MainWindow 另持一个做启动静默检查）
    update_checker_ = new UpdateChecker(this);
    connect(check_updates_button_, &QPushButton::clicked, this, [this]() {
        if (update_checker_->checking()) {
            return;
        }
        check_updates_button_->setEnabled(false);
        check_updates_button_->setText(tr("正在检查…"));
        update_result_label_->setText(tr("正在连接 GitHub Releases…"));
        update_checker_->check_for_updates();
    });
    connect(update_checker_, &UpdateChecker::update_available, this,
            [this](const QString& latest, const QString& url) {
        check_updates_button_->setEnabled(true);
        check_updates_button_->setText(tr("检查更新"));
        update_result_label_->setText(
            tr("发现新版本 %1，可<a href=\"%2\">前往发布页下载</a>。").arg(latest, url));
    });
    connect(update_checker_, &UpdateChecker::up_to_date, this, [this]() {
        check_updates_button_->setEnabled(true);
        check_updates_button_->setText(tr("检查更新"));
        update_result_label_->setText(tr("当前已是最新版本。"));
    });
    connect(update_checker_, &UpdateChecker::check_failed, this,
            [this](const QString& reason) {
        check_updates_button_->setEnabled(true);
        check_updates_button_->setText(tr("检查更新"));
        update_result_label_->setText(tr("检查失败：%1").arg(reason));
    });

    return group;
}

QLayout* SettingsPage::create_action_buttons_layout()
{
    auto* layout = new QHBoxLayout();
    layout->setSpacing(12);
    layout->setContentsMargins(0, 20, 0, 0);

    layout->addStretch();

    reset_button_ = new QPushButton(tr("重置"), this);
    reset_button_->setCursor(Qt::PointingHandCursor);
    reset_button_->setMinimumWidth(100);
    reset_button_->setObjectName("toolButton");
    connect(reset_button_, &QPushButton::clicked, this, &SettingsPage::reset_to_defaults);
    layout->addWidget(reset_button_);

    apply_button_ = new QPushButton(tr("应用"), this);
    apply_button_->setCursor(Qt::PointingHandCursor);
    apply_button_->setMinimumWidth(100);
    apply_button_->setObjectName("primaryButton");
    connect(apply_button_, &QPushButton::clicked, this, &SettingsPage::apply_settings);
    layout->addWidget(apply_button_);

    return layout;
}

QWidget* SettingsPage::create_appearance_section_widget()
{
    auto* group = new QGroupBox(tr("外观"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 8, 16, 16);

    // 主题设置行
    auto* theme_layout = new QHBoxLayout();
    theme_layout->setSpacing(12);

    auto* theme_label = new QLabel(tr("主题:"), this);
    theme_layout->addWidget(theme_label);

    current_theme_label_ = new QLabel(tr("浅色"), this);
    theme_layout->addWidget(current_theme_label_);

    theme_layout->addStretch();

    theme_toggle_button_ = new QPushButton(tr("切换深色"), this);
    theme_toggle_button_->setCursor(Qt::PointingHandCursor);
    theme_toggle_button_->setObjectName("toolButton");
    connect(theme_toggle_button_, &QPushButton::clicked, this, &SettingsPage::on_theme_button_clicked);
    theme_layout->addWidget(theme_toggle_button_);

    layout->addLayout(theme_layout);

    // 说明文字
    auto* desc_label = new QLabel(tr("在浅色与深色主题之间切换。"), this);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    // 任务列表视图（默认卡片；与顶栏/下载页内的切换按钮共用同一份记忆，
    // 手动切换后此处同步显示，首次/无记录即「卡片视图（默认）」）
    auto* view_layout = new QHBoxLayout();
    view_layout->setSpacing(12);

    auto* view_label = new QLabel(tr("任务列表视图:"), this);
    view_layout->addWidget(view_label);

    task_view_combo_ = new QComboBox(this);
    task_view_combo_->addItem(tr("卡片视图（默认）"), true);
    task_view_combo_->addItem(tr("列表视图"), false);
    task_view_combo_->setCursor(Qt::PointingHandCursor);
    connect(task_view_combo_, &QComboBox::currentIndexChanged, this,
            [this](int) { emit task_view_grid_changed(is_task_view_grid()); });
    view_layout->addWidget(task_view_combo_);

    view_layout->addStretch();
    layout->addLayout(view_layout);

    auto* view_desc = new QLabel(tr("下载页任务的显示方式；切换后立即生效并记住你的选择。"), this);
    view_desc->setObjectName("cardInfoLabel");
    view_desc->setWordWrap(true);
    layout->addWidget(view_desc);

    return group;
}

void SettingsPage::set_task_view_grid(bool grid_view)
{
    if (!task_view_combo_) {
        return;
    }
    const int want = task_view_combo_->findData(grid_view);
    if (want < 0 || task_view_combo_->currentIndex() == want) {
        return;
    }
    // 程序化回写不反向触发 task_view_grid_changed（避免设置页→下载页→设置页回环）
    QSignalBlocker blocker(task_view_combo_);
    task_view_combo_->setCurrentIndex(want);
}

bool SettingsPage::is_task_view_grid() const
{
    if (!task_view_combo_) {
        return true;  // 默认卡片视图
    }
    return task_view_combo_->currentData().toBool();
}

void SettingsPage::on_theme_button_clicked()
{
    emit theme_toggle_requested();
}

} // namespace falcon::desktop
