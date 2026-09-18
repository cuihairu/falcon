/**
 * @file settings_page.cpp
 * @brief Settings page implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "settings_page.hpp"

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
    , notifications_checkbox_(nullptr)
    , sound_notification_checkbox_(nullptr)
    , current_theme_label_(nullptr)
    , theme_toggle_button_(nullptr)
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

void SettingsPage::set_task_speed_limit(int kb_per_sec)
{
    task_speed_limit_spin_->setValue(kb_per_sec);
}

void SettingsPage::set_global_speed_limit(int kb_per_sec)
{
    global_speed_limit_spin_->setValue(kb_per_sec);
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

bool SettingsPage::is_notifications_enabled() const
{
    return notifications_checkbox_->isChecked();
}

bool SettingsPage::is_sound_notifications_enabled() const
{
    return sound_notification_checkbox_->isChecked();
}

int SettingsPage::get_task_speed_limit() const
{
    return task_speed_limit_spin_->value();
}

int SettingsPage::get_global_speed_limit() const
{
    return global_speed_limit_spin_->value();
}

bool SettingsPage::is_open_file_when_completed() const
{
    return completion_action_combo_ ? completion_action_combo_->currentIndex() == 1 : false;
}

int SettingsPage::get_action_when_completed() const
{
    return completion_action_combo_ ? completion_action_combo_->currentIndex() : 0;
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
    // Clipboard settings
    clipboard_monitoring_checkbox_->setChecked(false);
    clipboard_delay_spin_->setValue(1000);

    // Download settings
    download_dir_edit_->setText(QDir::homePath() + "/Downloads");
    max_downloads_spin_->setValue(3);

    // Speed limit settings (0 = unlimited)
    task_speed_limit_spin_->setValue(0);
    global_speed_limit_spin_->setValue(0);

    // Completion action settings (0 = do nothing)
    completion_action_combo_->setCurrentIndex(0);

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
    scroll_layout->addWidget(create_completion_action_section_widget());
    scroll_layout->addWidget(create_connection_section_widget());
    scroll_layout->addWidget(create_notification_section_widget());

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
    layout->setContentsMargins(16, 20, 16, 16);

    // Enable monitoring checkbox
    clipboard_monitoring_checkbox_ = new QCheckBox(tr("自动检测剪切板链接"), this);

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
    layout->setContentsMargins(16, 20, 16, 16);
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
    max_downloads_spin_ = new QSpinBox(this);
    max_downloads_spin_->setRange(1, 10);
    max_downloads_spin_->setValue(3);
    layout->addRow(max_label, max_downloads_spin_);

    return group;
}

QWidget* SettingsPage::create_speed_limit_section_widget()
{
    auto* group = new QGroupBox(tr("限速"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);
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

QWidget* SettingsPage::create_completion_action_section_widget()
{
    auto* group = new QGroupBox(tr("下载完成后"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);

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

    // 说明文字
    auto* desc_label = new QLabel(
        tr("选择下载完成后的动作；多文件任务仅显示通知。"),
        this
    );
    desc_label->setWordWrap(true);
    desc_label->setObjectName("cardInfoLabel");
    layout->addWidget(desc_label);

    return group;
}

QWidget* SettingsPage::create_connection_section_widget()
{
    auto* group = new QGroupBox(tr("连接"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);
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
    layout->setContentsMargins(16, 20, 16, 16);

    notifications_checkbox_ = new QCheckBox(tr("启用通知"), this);
    notifications_checkbox_->setChecked(true);
    layout->addWidget(notifications_checkbox_);

    sound_notification_checkbox_ = new QCheckBox(tr("提示音"), this);
    layout->addWidget(sound_notification_checkbox_);

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
    layout->setContentsMargins(16, 20, 16, 16);

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

    return group;
}

void SettingsPage::on_theme_button_clicked()
{
    emit theme_toggle_requested();
}

} // namespace falcon::desktop
