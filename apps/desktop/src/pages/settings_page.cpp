/**
 * @file settings_page.cpp
 * @brief Settings page implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "settings_page.hpp"
#include "../styles.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QLabel>
#include <QHeaderView>

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
    , notifications_checkbox_(nullptr)
    , sound_notification_checkbox_(nullptr)
    , apply_button_(nullptr)
    , reset_button_(nullptr)
{
    setup_ui();
}

//==============================================================================
// Getters
//==============================================================================

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

bool SettingsPage::is_notifications_enabled() const
{
    return notifications_checkbox_->isChecked();
}

//==============================================================================
// Private Slots
//==============================================================================

void SettingsPage::browse_download_dir()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "选择默认下载目录",
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

    // Connection settings
    default_connections_spin_->setValue(4);
    connection_timeout_spin_->setValue(30);
    retry_count_spin_->setValue(3);

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
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(20);

    // Page title
    auto* title_label = new QLabel("设置", this);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 24px;
            font-weight: 700;
            color: #323130;
        }
    )");
    main_layout->addWidget(title_label);

    // Scroll area for settings
    auto* scroll_content = new QWidget(this);
    auto* scroll_layout = new QVBoxLayout(scroll_content);
    scroll_layout->setSpacing(20);
    scroll_layout->setContentsMargins(0, 0, 0, 0);

    // Create setting sections
    scroll_layout->addWidget(create_clipboard_section_widget());
    scroll_layout->addWidget(create_download_section_widget());
    scroll_layout->addWidget(create_connection_section_widget());
    scroll_layout->addWidget(create_notification_section_widget());

    scroll_layout->addStretch();

    // Action buttons
    scroll_layout->addLayout(create_action_buttons_layout());

    main_layout->addWidget(scroll_content, 1);
}

QWidget* SettingsPage::create_clipboard_section_widget()
{
    auto* group = new QGroupBox("剪切板监听", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 16px;
            font-weight: 600;
            color: #323130;
            border: 1px solid #E2E8F0;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px 0 4px;
        }
    )");

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);

    // Enable monitoring checkbox
    clipboard_monitoring_checkbox_ = new QCheckBox("启用剪切板监听", this);
    clipboard_monitoring_checkbox_->setStyleSheet(R"(
        QCheckBox {
            font-size: 14px;
            color: #2D3748;
            spacing: 8px;
        }
        QCheckBox::indicator {
            width: 18px;
            height: 18px;
            border-radius: 4px;
            border: 2px solid #CBD5E0;
            background: white;
        }
        QCheckBox::indicator:checked {
            background: #5C6BC0;
            border-color: #5C6BC0;
            image: url(data:image/svg+xml;base64,PHN2ZyB3aWR0aD0iMTgiIGhlaWdodD0iMTgiIHZpZXdCb3g9IjAgMCAxOCAxOCIgZmlsbD0ibm9uZSIgeG1sbnM9Imh0dHA6Ly93d3cudzMub3JnLzIwMDAvc3ZnIj4KPHBhdGggZD0iTTE0IDZMNyAxM0w0IDEwIiBzdHJva2U9IndoaXRlIiBzdHJva2Utd2lkdGg9IjIiIHN0cm9rZS1saW5lY2FwPSJyb3VuZCIgc3Ryb2tlLWxpbmVqb2luPSJyb3VuZCIvPgo8L3N2Zz4K);
        }
        QCheckBox::indicator:hover {
            border-color: #5C6BC0;
        }
    )");

    auto* desc_label = new QLabel(
        "启用后将自动检测剪切板中的下载链接（HTTP、FTP、磁力链接等）",
        this
    );
    desc_label->setStyleSheet("color: #718096; font-size: 13px; padding-left: 26px;");
    desc_label->setWordWrap(true);

    layout->addWidget(clipboard_monitoring_checkbox_);
    layout->addWidget(desc_label);

    // Detection delay
    auto* delay_layout = new QHBoxLayout();
    auto* delay_label = new QLabel("检测间隔:", this);
    delay_label->setStyleSheet("color: #605e5c; font-size: 13px;");

    clipboard_delay_spin_ = new QSpinBox(this);
    clipboard_delay_spin_->setRange(500, 10000);
    clipboard_delay_spin_->setValue(1000);
    clipboard_delay_spin_->setSuffix(" ms");
    clipboard_delay_spin_->setStyleSheet(get_combo_stylesheet());

    auto* delay_hint = new QLabel("(避免重复检测)", this);
    delay_hint->setStyleSheet("color: #A0AEC0; font-size: 12px;");

    delay_layout->addWidget(delay_label);
    delay_layout->addWidget(clipboard_delay_spin_);
    delay_layout->addWidget(delay_hint);
    delay_layout->addStretch();

    layout->addLayout(delay_layout);

    return group;
}

QWidget* SettingsPage::create_download_section_widget()
{
    auto* group = new QGroupBox("下载设置", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 16px;
            font-weight: 600;
            color: #323130;
            border: 1px solid #E2E8F0;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px 0 4px;
        }
    )");

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default download directory
    auto* dir_label = new QLabel("默认下载目录:", this);
    dir_label->setStyleSheet("color: #605e5c; font-size: 13px;");

    auto* dir_layout = new QHBoxLayout();
    dir_layout->setSpacing(8);
    download_dir_edit_ = new QLineEdit(QDir::homePath() + "/Downloads", this);
    download_dir_edit_->setStyleSheet(get_input_stylesheet());
    download_dir_edit_->setReadOnly(true);
    dir_layout->addWidget(download_dir_edit_, 1);

    auto* browse_btn = new QPushButton("浏览...", this);
    browse_btn->setStyleSheet(get_button_stylesheet(false));
    browse_btn->setCursor(Qt::PointingHandCursor);
    connect(browse_btn, &QPushButton::clicked, this, &SettingsPage::browse_download_dir);
    dir_layout->addWidget(browse_btn);

    layout->addRow(dir_label, dir_layout);

    // Maximum concurrent downloads
    auto* max_label = new QLabel("最大并发下载数:", this);
    max_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    max_downloads_spin_ = new QSpinBox(this);
    max_downloads_spin_->setRange(1, 10);
    max_downloads_spin_->setValue(3);
    max_downloads_spin_->setStyleSheet(get_combo_stylesheet());
    layout->addRow(max_label, max_downloads_spin_);

    return group;
}

QWidget* SettingsPage::create_connection_section_widget()
{
    auto* group = new QGroupBox("连接设置", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 16px;
            font-weight: 600;
            color: #323130;
            border: 1px solid #E2E8F0;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px 0 4px;
        }
    )");

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default connections
    auto* conn_label = new QLabel("默认连接数:", this);
    conn_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    default_connections_spin_ = new QSpinBox(this);
    default_connections_spin_->setRange(1, 16);
    default_connections_spin_->setValue(4);
    default_connections_spin_->setStyleSheet(get_combo_stylesheet());
    layout->addRow(conn_label, default_connections_spin_);

    // Connection timeout
    auto* timeout_label = new QLabel("连接超时:", this);
    timeout_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    connection_timeout_spin_ = new QSpinBox(this);
    connection_timeout_spin_->setRange(5, 120);
    connection_timeout_spin_->setValue(30);
    connection_timeout_spin_->setSuffix(" s");
    connection_timeout_spin_->setStyleSheet(get_combo_stylesheet());
    layout->addRow(timeout_label, connection_timeout_spin_);

    // Retry count
    auto* retry_label = new QLabel("重试次数:", this);
    retry_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    retry_count_spin_ = new QSpinBox(this);
    retry_count_spin_->setRange(0, 10);
    retry_count_spin_->setValue(3);
    retry_count_spin_->setStyleSheet(get_combo_stylesheet());
    layout->addRow(retry_label, retry_count_spin_);

    return group;
}

QWidget* SettingsPage::create_notification_section_widget()
{
    auto* group = new QGroupBox("通知设置", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 16px;
            font-weight: 600;
            color: #323130;
            border: 1px solid #E2E8F0;
            border-radius: 8px;
            margin-top: 12px;
            padding-top: 8px;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 12px;
            padding: 0 4px 0 4px;
        }
    )");

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);

    notifications_checkbox_ = new QCheckBox("启用通知", this);
    notifications_checkbox_->setChecked(true);
    notifications_checkbox_->setStyleSheet(R"(
        QCheckBox {
            font-size: 14px;
            color: #2D3748;
            spacing: 8px;
        }
        QCheckBox::indicator {
            width: 18px;
            height: 18px;
            border-radius: 4px;
            border: 2px solid #CBD5E0;
            background: white;
        }
        QCheckBox::indicator:checked {
            background: #5C6BC0;
            border-color: #5C6BC0;
        }
        QCheckBox::indicator:hover {
            border-color: #5C6BC0;
        }
    )");
    layout->addWidget(notifications_checkbox_);

    sound_notification_checkbox_ = new QCheckBox("提示音", this);
    sound_notification_checkbox_->setStyleSheet(R"(
        QCheckBox {
            font-size: 14px;
            color: #2D3748;
            spacing: 8px;
            padding-left: 20px;
        }
        QCheckBox::indicator {
            width: 18px;
            height: 18px;
            border-radius: 4px;
            border: 2px solid #CBD5E0;
            background: white;
        }
        QCheckBox::indicator:checked {
            background: #5C6BC0;
            border-color: #5C6BC0;
        }
        QCheckBox::indicator:hover {
            border-color: #5C6BC0;
        }
    )");
    layout->addWidget(sound_notification_checkbox_);

    return group;
}

QLayout* SettingsPage::create_action_buttons_layout()
{
    auto* layout = new QHBoxLayout();
    layout->setSpacing(12);
    layout->setContentsMargins(0, 20, 0, 0);

    layout->addStretch();

    reset_button_ = new QPushButton("恢复默认", this);
    reset_button_->setStyleSheet(get_button_stylesheet(false));
    reset_button_->setCursor(Qt::PointingHandCursor);
    reset_button_->setMinimumWidth(100);
    connect(reset_button_, &QPushButton::clicked, this, &SettingsPage::reset_to_defaults);
    layout->addWidget(reset_button_);

    apply_button_ = new QPushButton("应用设置", this);
    apply_button_->setStyleSheet(get_button_stylesheet(true));
    apply_button_->setCursor(Qt::PointingHandCursor);
    apply_button_->setMinimumWidth(100);
    connect(apply_button_, &QPushButton::clicked, this, &SettingsPage::apply_settings);
    layout->addWidget(apply_button_);

    return layout;
}

} // namespace falcon::desktop
