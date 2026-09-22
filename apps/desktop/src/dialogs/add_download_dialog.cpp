/**
 * @file add_download_dialog.cpp
 * @brief Add download dialog implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "add_download_dialog.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QStyle>
#include <QDir>

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
    , referrer_edit_(nullptr)
    , cookies_edit_(nullptr)
    , start_button_(nullptr)
    , cancel_button_(nullptr)
{
    setup_ui();
    setWindowTitle(tr("添加下载任务"));
    setModal(true);
    resize(600, 450);
}

void AddDownloadDialog::set_default_save_path(const QString& path)
{
    if (save_path_edit_ && !path.trimmed().isEmpty()) {
        save_path_edit_->setText(path.trimmed());
    }
}

void AddDownloadDialog::set_default_connections(int count)
{
    if (connections_spin_) {
        connections_spin_->setValue(count);
    }
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

QString AddDownloadDialog::get_referrer() const
{
    return referrer_edit_ ? referrer_edit_->text().trimmed() : QString();
}

QString AddDownloadDialog::get_cookies() const
{
    return cookies_edit_ ? cookies_edit_->toPlainText().trimmed() : QString();
}

void AddDownloadDialog::set_request_referrer(const QString& referrer)
{
    if (referrer_edit_) {
        referrer_edit_->setText(referrer);
    }
}

void AddDownloadDialog::set_request_user_agent(const QString& user_agent)
{
    if (!user_agent_combo_) {
        return;
    }

    const QString trimmed = user_agent.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    user_agent_combo_->setCurrentText(trimmed);
}

void AddDownloadDialog::set_request_cookies(const QString& cookies)
{
    if (!cookies_edit_) {
        return;
    }
    cookies_edit_->setPlainText(cookies);
}

//==============================================================================
// Private Slots
//==============================================================================

void AddDownloadDialog::browse_directory()
{
    QString dir = QFileDialog::getExistingDirectory(
        this,
        tr("选择保存目录"),
        save_path_edit_->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty()) {
        save_path_edit_->setText(dir);
    }
}

void AddDownloadDialog::start_download()
{
    // Validate inputs:URL 非空且可解析(新任务模式用户手输,格式非法
    // 不放行;解析过的只读路径重解析无损——thunder/magnet 解码态仍合法)
    if (!UrlDetector::parse_url(url_edit_->text()).is_valid) {
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
    main_layout->setContentsMargins(18, 18, 18, 18);
    main_layout->setSpacing(14);

    main_layout->addWidget(create_page_hero());

    main_layout->addWidget(create_url_section_widget());
    main_layout->addWidget(create_file_section_widget());
    main_layout->addWidget(create_options_section_widget());

    main_layout->addStretch();
    main_layout->addLayout(create_button_layout());
}

QWidget* AddDownloadDialog::create_page_hero()
{
    auto* hero = new QWidget(this);
    hero->setObjectName("downloadHero");

    auto* layout = new QVBoxLayout(hero);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(4);

    auto* title = new QLabel(tr("添加下载任务"), hero);
    title->setObjectName("heroTitle");
    layout->addWidget(title);

    auto* desc = new QLabel(tr("确认文件名、保存目录与请求参数后开始下载。"), hero);
    desc->setObjectName("heroDescription");
    desc->setWordWrap(true);
    layout->addWidget(desc);

    return hero;
}

QWidget* AddDownloadDialog::create_url_section_widget()
{
    auto* group = new QGroupBox(tr("URL"), this);

    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(12);

    // Protocol label
    protocol_label_ = new QLabel(
        tr("协议: %1").arg(UrlDetector::get_protocol_name(url_info_.protocol)),
        this
    );
    layout->addWidget(protocol_label_);

    // URL input:解析过的 URL(剪贴板/IPC 直达)只读展示;新建任务入口
    // (url_info 无效)允许用户直接输入,协议标签与文件名随输入联动
    url_edit_ = new QLineEdit(url_info_.decoded_url, this);
    if (url_info_.is_valid) {
        url_edit_->setReadOnly(true);
    } else {
        connect(url_edit_, &QLineEdit::textEdited,
                this, &AddDownloadDialog::on_url_edited);
    }
    layout->addWidget(url_edit_);

    return group;
}

void AddDownloadDialog::on_url_edited(const QString& text)
{
    const UrlInfo parsed = UrlDetector::parse_url(text);
    protocol_label_->setText(tr("协议: %1")
        .arg(UrlDetector::get_protocol_name(parsed.protocol)));

    // 文件名未被手改时随 URL 自动推断(末段路径;magnet 等无文件名
    // 形态推断为空则保留现状,由 start_download 校验兜底)
    if (!file_name_edited_ && parsed.is_valid && !parsed.file_name.isEmpty()) {
        file_name_edit_->setText(parsed.file_name);
    }
}

QWidget* AddDownloadDialog::create_file_section_widget()
{
    auto* group = new QGroupBox(tr("保存"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(12);
    layout->setLabelAlignment(Qt::AlignRight);
    layout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    // File name
    auto* file_label = new QLabel(tr("文件名:"), this);
    file_name_edit_ = new QLineEdit(url_info_.file_name, this);
    connect(file_name_edit_, &QLineEdit::textEdited, this, [this] {
        file_name_edited_ = true;
    });
    layout->addRow(file_label, file_name_edit_);

    // Save path
    auto* path_label = new QLabel(tr("保存路径:"), this);

    auto* path_layout = new QHBoxLayout();
    path_layout->setSpacing(8);
    save_path_edit_ = new QLineEdit(QDir::homePath() + "/Downloads", this);
    path_layout->addWidget(save_path_edit_, 1);

    browse_button_ = new QPushButton(tr("浏览..."), this);
    browse_button_->setObjectName("toolButton");
    browse_button_->setCursor(Qt::PointingHandCursor);
    connect(browse_button_, &QPushButton::clicked, this, &AddDownloadDialog::browse_directory);
    path_layout->addWidget(browse_button_);

    layout->addRow(path_label, path_layout);

    return group;
}

QWidget* AddDownloadDialog::create_options_section_widget()
{
    auto* group = new QGroupBox(tr("高级"), this);

    auto* layout = new QFormLayout(group);
    layout->setSpacing(12);
    layout->setLabelAlignment(Qt::AlignRight);

    // Connections
    auto* conn_label = new QLabel(tr("连接数:"), this);
    connections_spin_ = new QSpinBox(this);
    connections_spin_->setRange(1, 16);
    connections_spin_->setValue(4);
    layout->addRow(conn_label, connections_spin_);

    // User agent
    auto* ua_label = new QLabel(tr("User Agent:"), this);
    user_agent_combo_ = new QComboBox(this);
    user_agent_combo_->setEditable(true);
    user_agent_combo_->addItem("Falcon/1.0");
    user_agent_combo_->addItem("Mozilla/5.0 (Windows NT 10.0; Win64; x64)");
    user_agent_combo_->addItem("curl/7.68.0");
    layout->addRow(ua_label, user_agent_combo_);

    // Referrer
    auto* ref_label = new QLabel(tr("Referer:"), this);
    referrer_edit_ = new QLineEdit(this);
    referrer_edit_->setPlaceholderText("https://example.com/");
    layout->addRow(ref_label, referrer_edit_);

    // Cookies
    auto* cookie_label = new QLabel(tr("Cookies:"), this);
    cookies_edit_ = new QPlainTextEdit(this);
    cookies_edit_->setPlaceholderText("name=value; ...");
    cookies_edit_->setMaximumBlockCount(50);
    cookies_edit_->setFixedHeight(72);
    layout->addRow(cookie_label, cookies_edit_);

    return group;
}

QLayout* AddDownloadDialog::create_button_layout()
{
    auto* layout = new QHBoxLayout();
    layout->setSpacing(12);

    layout->addStretch();

    cancel_button_ = new QPushButton(tr("取消"), this);
    cancel_button_->setObjectName("toolButton");
    cancel_button_->setCursor(Qt::PointingHandCursor);
    cancel_button_->setMinimumWidth(100);
    connect(cancel_button_, &QPushButton::clicked, this, &AddDownloadDialog::cancel_dialog);
    layout->addWidget(cancel_button_);

    start_button_ = new QPushButton(tr("开始下载"), this);
    start_button_->setObjectName("primaryButton");
    start_button_->setCursor(Qt::PointingHandCursor);
    start_button_->setMinimumWidth(100);
    start_button_->setDefault(true);
    connect(start_button_, &QPushButton::clicked, this, &AddDownloadDialog::start_download);
    layout->addWidget(start_button_);

    return layout;
}

} // namespace falcon::desktop
