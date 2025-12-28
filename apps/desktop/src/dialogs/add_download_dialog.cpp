/**
 * @file add_download_dialog.cpp
 * @brief Add download dialog implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "add_download_dialog.hpp"
#include "../styles.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QGroupBox>
#include <QDialogButtonBox>

namespace falcon::desktop {

//==============================================================================
// Constructor / Destructor
//==============================================================================

AddDownloadDialog::AddDownloadDialog(const UrlInfo& url_info, QWidget* parent)
    : QDialog(parent)
    , url_info_(url_info)
    , url_label_(nullptr)
    , url_edit_(nullptr)
    , protocol_label_(nullptr)
    , file_name_edit_(nullptr)
    , save_path_edit_(nullptr)
    , browse_button_(nullptr)
    , connections_spin_(nullptr)
    , user_agent_combo_(nullptr)
    , start_button_(nullptr)
    , cancel_button_(nullptr)
{
    setup_ui();
    setWindowTitle("添加下载任务");
    setModal(true);
    resize(600, 450);
}

//==============================================================================
// Getters
//==============================================================================

QString AddDownloadDialog::get_url() const
{
    return url_edit_->text();
}

QString AddDownloadDialog::get_save_path() const
{
    return save_path_edit_->text();
}

QString AddDownloadDialog::get_file_name() const
{
    return file_name_edit_->text();
}

int AddDownloadDialog::get_connections() const
{
    return connections_spin_->value();
}

QString AddDownloadDialog::get_user_agent() const
{
    return user_agent_combo_->currentText();
}

//==============================================================================
// Private Slots
//==============================================================================

void AddDownloadDialog::browse_directory()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "选择保存目录",
        save_path_edit_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty()) {
        save_path_edit_->setText(dir);
    }
}

void AddDownloadDialog::start_download()
{
    // Validate inputs
    if (url_edit_->text().isEmpty()) {
        url_edit_->setFocus();
        return;
    }

    if (file_name_edit_->text().isEmpty()) {
        file_name_edit_->setFocus();
        return;
    }

    if (save_path_edit_->text().isEmpty()) {
        save_path_edit_->setFocus();
        return;
    }

    accept();
}

void AddDownloadDialog::cancel_dialog()
{
    reject();
}

//==============================================================================
// Private Methods
//==============================================================================

void AddDownloadDialog::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(20);

    // Header
    auto* header_layout = new QHBoxLayout();
    auto* icon_label = new QLabel("⬇️", this);
    icon_label->setStyleSheet("font-size: 32px;");
    header_layout->addWidget(icon_label);

    auto* title_label = new QLabel("添加下载任务", this);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 20px;
            font-weight: 700;
            color: #323130;
        }
    )");
    header_layout->addWidget(title_label);
    header_layout->addStretch();
    main_layout->addLayout(header_layout);

    // URL Section
    main_layout->addWidget(create_url_section_widget());

    // File Section
    main_layout->addWidget(create_file_section_widget());

    // Options Section
    main_layout->addWidget(create_options_section_widget());

    main_layout->addStretch();

    // Buttons
    main_layout->addLayout(create_button_layout());
}

QWidget* AddDownloadDialog::create_url_section_widget()
{
    auto* group = new QGroupBox("下载链接", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 14px;
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
    layout->setSpacing(12);

    // Protocol label
    protocol_label_ = new QLabel(
        QString("协议类型: %1").arg(UrlDetector::get_protocol_name(url_info_.protocol)),
        this
    );
    protocol_label_->setStyleSheet("color: #605e5c; font-size: 13px;");
    layout->addWidget(protocol_label_);

    // URL input
    url_edit_ = new QLineEdit(url_info_.decoded_url, this);
    url_edit_->setStyleSheet(get_input_stylesheet());
    url_edit_->setReadOnly(true);
    layout->addWidget(url_edit_);

    return group;
}

QWidget* AddDownloadDialog::create_file_section_widget()
{
    auto* group = new QGroupBox("保存设置", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 14px;
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
    layout->setSpacing(12);
    layout->setLabelAlignment(Qt::AlignRight);
    layout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    // File name
    auto* file_label = new QLabel("文件名:", this);
    file_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    file_name_edit_ = new QLineEdit(url_info_.file_name, this);
    file_name_edit_->setStyleSheet(get_input_stylesheet());
    layout->addRow(file_label, file_name_edit_);

    // Save path
    auto* path_label = new QLabel("保存路径:", this);
    path_label->setStyleSheet("color: #605e5c; font-size: 13px;");

    auto* path_layout = new QHBoxLayout();
    path_layout->setSpacing(8);
    save_path_edit_ = new QLineEdit(QDir::homePath() + "/Downloads", this);
    save_path_edit_->setStyleSheet(get_input_stylesheet());
    path_layout->addWidget(save_path_edit_, 1);

    browse_button_ = new QPushButton("浏览...", this);
    browse_button_->setStyleSheet(get_button_stylesheet(false));
    browse_button_->setCursor(Qt::PointingHandCursor);
    connect(browse_button_, &QPushButton::clicked, this, &AddDownloadDialog::browse_directory);
    path_layout->addWidget(browse_button_);

    layout->addRow(path_label, path_layout);

    return group;
}

QWidget* AddDownloadDialog::create_options_section_widget()
{
    auto* group = new QGroupBox("高级选项", this);
    group->setStyleSheet(R"(
        QGroupBox {
            font-size: 14px;
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
    layout->setSpacing(12);
    layout->setLabelAlignment(Qt::AlignRight);

    // Connections
    auto* conn_label = new QLabel("连接数:", this);
    conn_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    connections_spin_ = new QSpinBox(this);
    connections_spin_->setRange(1, 16);
    connections_spin_->setValue(4);
    connections_spin_->setStyleSheet(get_combo_stylesheet());
    layout->addRow(conn_label, connections_spin_);

    // User agent
    auto* ua_label = new QLabel("User Agent:", this);
    ua_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    user_agent_combo_ = new QComboBox(this);
    user_agent_combo_->setStyleSheet(get_combo_stylesheet());
    user_agent_combo_->setEditable(true);
    user_agent_combo_->addItem("Falcon/1.0");
    user_agent_combo_->addItem("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    user_agent_combo_->addItem("curl/7.68.0");
    layout->addRow(ua_label, user_agent_combo_);

    return group;
}

QLayout* AddDownloadDialog::create_button_layout()
{
    auto* layout = new QHBoxLayout();
    layout->setSpacing(12);

    layout->addStretch();

    cancel_button_ = new QPushButton("取消", this);
    cancel_button_->setStyleSheet(get_button_stylesheet(false));
    cancel_button_->setCursor(Qt::PointingHandCursor);
    cancel_button_->setMinimumWidth(100);
    connect(cancel_button_, &QPushButton::clicked, this, &AddDownloadDialog::cancel_dialog);
    layout->addWidget(cancel_button_);

    start_button_ = new QPushButton("开始下载", this);
    start_button_->setStyleSheet(get_button_stylesheet(true));
    start_button_->setCursor(Qt::PointingHandCursor);
    start_button_->setMinimumWidth(100);
    start_button_->setDefault(true);
    connect(start_button_, &QPushButton::clicked, this, &AddDownloadDialog::start_download);
    layout->addWidget(start_button_);

    return layout;
}

} // namespace falcon::desktop
