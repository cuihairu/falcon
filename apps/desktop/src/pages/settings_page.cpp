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
        tr("Select default download directory"),
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
    auto* title_label = new QLabel(tr("Settings"), this);
    auto title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
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
    auto* group = new QGroupBox(tr("Clipboard Monitoring"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);

    // Enable monitoring checkbox
    clipboard_monitoring_checkbox_ = new QCheckBox(tr("Enable clipboard monitoring"), this);

    auto* desc_label = new QLabel(
        tr("Detect download links from clipboard automatically (HTTP, FTP, magnet, etc.)."),
        this
    );
    desc_label->setWordWrap(true);

    layout->addWidget(clipboard_monitoring_checkbox_);
    layout->addWidget(desc_label);

    // Detection delay
    auto* delay_layout = new QHBoxLayout();
    auto* delay_label = new QLabel(tr("Detection interval:"), this);

    clipboard_delay_spin_ = new QSpinBox(this);
    clipboard_delay_spin_->setRange(500, 10000);
    clipboard_delay_spin_->setValue(1000);
    clipboard_delay_spin_->setSuffix(" ms");

    auto* delay_hint = new QLabel(tr("(avoid duplicate triggers)"), this);

    delay_layout->addWidget(delay_label);
    delay_layout->addWidget(clipboard_delay_spin_);
    delay_layout->addWidget(delay_hint);
    delay_layout->addStretch();

    layout->addLayout(delay_layout);

    return group;
}

QWidget* SettingsPage::create_download_section_widget()
{
    auto* group = new QGroupBox(tr("Downloads"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default download directory
    auto* dir_label = new QLabel(tr("Default download directory:"), this);

    auto* dir_layout = new QHBoxLayout();
    dir_layout->setSpacing(8);
    download_dir_edit_ = new QLineEdit(QDir::homePath() + "/Downloads", this);
    download_dir_edit_->setReadOnly(true);
    dir_layout->addWidget(download_dir_edit_, 1);

    auto* browse_btn = new QPushButton(tr("Browse..."), this);
    browse_btn->setIcon(style()->standardIcon(QStyle::SP_DirOpenIcon));
    browse_btn->setCursor(Qt::PointingHandCursor);
    connect(browse_btn, &QPushButton::clicked, this, &SettingsPage::browse_download_dir);
    dir_layout->addWidget(browse_btn);

    layout->addRow(dir_label, dir_layout);

    // Maximum concurrent downloads
    auto* max_label = new QLabel(tr("Max concurrent downloads:"), this);
    max_downloads_spin_ = new QSpinBox(this);
    max_downloads_spin_->setRange(1, 10);
    max_downloads_spin_->setValue(3);
    layout->addRow(max_label, max_downloads_spin_);

    return group;
}

QWidget* SettingsPage::create_connection_section_widget()
{
    auto* group = new QGroupBox(tr("Connection"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);
    layout->setLabelAlignment(Qt::AlignRight);

    // Default connections
    auto* conn_label = new QLabel(tr("Default connections:"), this);
    default_connections_spin_ = new QSpinBox(this);
    default_connections_spin_->setRange(1, 16);
    default_connections_spin_->setValue(4);
    layout->addRow(conn_label, default_connections_spin_);

    // Connection timeout
    auto* timeout_label = new QLabel(tr("Connection timeout:"), this);
    connection_timeout_spin_ = new QSpinBox(this);
    connection_timeout_spin_->setRange(5, 120);
    connection_timeout_spin_->setValue(30);
    connection_timeout_spin_->setSuffix(" s");
    layout->addRow(timeout_label, connection_timeout_spin_);

    // Retry count
    auto* retry_label = new QLabel(tr("Retry count:"), this);
    retry_count_spin_ = new QSpinBox(this);
    retry_count_spin_->setRange(0, 10);
    retry_count_spin_->setValue(3);
    layout->addRow(retry_label, retry_count_spin_);

    return group;
}

QWidget* SettingsPage::create_notification_section_widget()
{
    auto* group = new QGroupBox(tr("Notifications"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(16);
    layout->setContentsMargins(16, 20, 16, 16);

    notifications_checkbox_ = new QCheckBox(tr("Enable notifications"), this);
    notifications_checkbox_->setChecked(true);
    layout->addWidget(notifications_checkbox_);

    sound_notification_checkbox_ = new QCheckBox(tr("Sound"), this);
    layout->addWidget(sound_notification_checkbox_);

    return group;
}

QLayout* SettingsPage::create_action_buttons_layout()
{
    auto* layout = new QHBoxLayout();
    layout->setSpacing(12);
    layout->setContentsMargins(0, 20, 0, 0);

    layout->addStretch();

    reset_button_ = new QPushButton(tr("Reset"), this);
    reset_button_->setIcon(style()->standardIcon(QStyle::SP_DialogResetButton));
    reset_button_->setCursor(Qt::PointingHandCursor);
    reset_button_->setMinimumWidth(100);
    connect(reset_button_, &QPushButton::clicked, this, &SettingsPage::reset_to_defaults);
    layout->addWidget(reset_button_);

    apply_button_ = new QPushButton(tr("Apply"), this);
    apply_button_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    apply_button_->setCursor(Qt::PointingHandCursor);
    apply_button_->setMinimumWidth(100);
    connect(apply_button_, &QPushButton::clicked, this, &SettingsPage::apply_settings);
    layout->addWidget(apply_button_);

    return layout;
}

} // namespace falcon::desktop
