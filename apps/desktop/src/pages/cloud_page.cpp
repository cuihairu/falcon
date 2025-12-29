/**
 * @file cloud_page.cpp
 * @brief 云盘资源浏览页面实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "cloud_page.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QMenu>
#include <QStyle>
#include <QFileInfo>
#include <QDir>

namespace falcon::desktop {

CloudPage::CloudPage(QWidget* parent)
    : QWidget(parent)
    , splitter_(nullptr)
    , empty_state_widget_(nullptr)
    , stacked_widget_(nullptr)
    , left_panel_(nullptr)
    , right_panel_(nullptr)
{
    setup_ui();
}

CloudPage::~CloudPage() = default;

void CloudPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(16);

    // 页面标题
    auto* title_label = new QLabel(tr("Cloud"), this);
    auto title_font = title_label->font();
    title_font.setPointSize(20);
    title_font.setBold(true);
    title_label->setFont(title_font);
    main_layout->addWidget(title_label);

    // 创建堆叠窗口用于视图切换
    stacked_widget_ = new QStackedWidget(this);

    // 创建空状态视图
    create_empty_state();
    stacked_widget_->addWidget(empty_state_widget_);

    // 创建分割器（配置面板 + 文件浏览器）
    splitter_ = new QSplitter(Qt::Horizontal, this);

    // 创建左侧面板（存储配置）
    create_storage_selector();
    splitter_->addWidget(left_panel_);

    // 创建右侧面板（文件浏览器）
    create_file_browser();
    splitter_->addWidget(right_panel_);

    // 设置分割比例（30% : 70%）
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 7);

    // 将分割器添加到堆叠窗口
    stacked_widget_->addWidget(splitter_);

    main_layout->addWidget(stacked_widget_);

    // 初始显示空状态
    show_empty_state();
}

void CloudPage::create_storage_selector()
{
    left_panel_ = new QWidget(this);
    auto* layout = new QVBoxLayout(left_panel_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // 标题
    auto* title_label = new QLabel(tr("Cloud Storage"), left_panel_);
    auto section_font = title_label->font();
    section_font.setPointSize(14);
    section_font.setBold(true);
    title_label->setFont(section_font);
    layout->addWidget(title_label);

    // 存储类型选择
    auto* type_layout = new QHBoxLayout();
    auto* type_label = new QLabel(tr("Type:"), left_panel_);
    storage_type_combo_ = new QComboBox(left_panel_);
    storage_type_combo_->addItem(tr("Amazon S3"), "s3");
    storage_type_combo_->addItem(tr("Alibaba OSS"), "oss");
    storage_type_combo_->addItem(tr("Tencent COS"), "cos");
    storage_type_combo_->addItem(tr("Qiniu Kodo"), "kodo");
    storage_type_combo_->addItem(tr("Upyun"), "upyun");
    type_layout->addWidget(type_label);
    type_layout->addWidget(storage_type_combo_);
    layout->addLayout(type_layout);

    // 端点
    auto* endpoint_layout = new QHBoxLayout();
    auto* endpoint_label = new QLabel(tr("Endpoint:"), left_panel_);
    endpoint_edit_ = new QLineEdit(left_panel_);
    endpoint_edit_->setPlaceholderText("s3.amazonaws.com");
    endpoint_layout->addWidget(endpoint_label);
    endpoint_layout->addWidget(endpoint_edit_);
    layout->addLayout(endpoint_layout);

    // 访问密钥
    auto* access_key_layout = new QHBoxLayout();
    auto* access_key_label = new QLabel("Access Key:", left_panel_);
    access_key_edit_ = new QLineEdit(left_panel_);
    access_key_edit_->setEchoMode(QLineEdit::Password);
    access_key_layout->addWidget(access_key_label);
    access_key_layout->addWidget(access_key_edit_);
    layout->addLayout(access_key_layout);

    // 密钥
    auto* secret_key_layout = new QHBoxLayout();
    auto* secret_key_label = new QLabel("Secret Key:", left_panel_);
    secret_key_edit_ = new QLineEdit(left_panel_);
    secret_key_edit_->setEchoMode(QLineEdit::Password);
    secret_key_layout->addWidget(secret_key_label);
    secret_key_layout->addWidget(secret_key_edit_);
    layout->addLayout(secret_key_layout);

    // 区域
    auto* region_layout = new QHBoxLayout();
    auto* region_label = new QLabel(tr("Region:"), left_panel_);
    region_edit_ = new QLineEdit(left_panel_);
    region_edit_->setPlaceholderText("us-east-1");
    region_layout->addWidget(region_label);
    region_layout->addWidget(region_edit_);
    layout->addLayout(region_layout);

    // 存储桶
    auto* bucket_layout = new QHBoxLayout();
    auto* bucket_label = new QLabel(tr("Bucket:"), left_panel_);
    bucket_edit_ = new QLineEdit(left_panel_);
    bucket_layout->addWidget(bucket_label);
    bucket_layout->addWidget(bucket_edit_);
    layout->addLayout(bucket_layout);

    layout->addStretch();

    // 连接按钮
    connect_button_ = new QPushButton(tr("Connect"), left_panel_);
    connect_button_->setIcon(style()->standardIcon(QStyle::SP_DialogApplyButton));
    connect_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(connect_button_);

    disconnect_button_ = new QPushButton(tr("Disconnect"), left_panel_);
    disconnect_button_->setEnabled(false);
    disconnect_button_->setIcon(style()->standardIcon(QStyle::SP_DialogCancelButton));
    disconnect_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(disconnect_button_);

    save_config_button_ = new QPushButton(tr("Save Config"), left_panel_);
    save_config_button_->setIcon(style()->standardIcon(QStyle::SP_DialogSaveButton));
    save_config_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(save_config_button_);

    // 连接信号
    connect(connect_button_, &QPushButton::clicked, this, &CloudPage::connect_to_storage);
    connect(disconnect_button_, &QPushButton::clicked, this, &CloudPage::disconnect_storage);
    connect(save_config_button_, &QPushButton::clicked, this, [this]() {
        // TODO: 实现保存配置功能
        QMessageBox::information(this, tr("Notice"), tr("Save config is not implemented yet."));
    });

    // 连接状态
    connection_status_label_ = new QLabel(tr("Disconnected"), left_panel_);
    layout->addWidget(connection_status_label_);
}

void CloudPage::create_file_browser()
{
    right_panel_ = new QWidget(this);
    auto* layout = new QVBoxLayout(right_panel_);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // 工具栏
    auto* toolbar = create_toolbar();
    layout->addWidget(toolbar);

    // 路径栏
    auto* path_layout = new QHBoxLayout();
    auto* path_label = new QLabel(tr("Path:"), right_panel_);
    current_path_edit_ = new QLineEdit(right_panel_);
    current_path_edit_->setReadOnly(true);
    current_path_edit_->setText("/");
    path_layout->addWidget(path_label);
    path_layout->addWidget(current_path_edit_);
    layout->addLayout(path_layout);

    // 文件列表
    file_table_ = new QTableWidget(right_panel_);
    file_table_->setColumnCount(5);
    file_table_->setHorizontalHeaderLabels({
        tr("Name"), tr("Size"), tr("Modified"), tr("Type"), tr("Actions")
    });

    file_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    file_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    file_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    file_table_->horizontalHeader()->setStretchLastSection(false);
    file_table_->setContextMenuPolicy(Qt::CustomContextMenu);

    // 设置列宽
    file_table_->setColumnWidth(0, 300);  // 名称
    file_table_->setColumnWidth(1, 100);  // 大小
    file_table_->setColumnWidth(2, 180);  // 修改时间
    file_table_->setColumnWidth(3, 100);  // 类型
    file_table_->setColumnWidth(4, 100);  // 操作

    layout->addWidget(file_table_);

    // 状态栏
    create_status_bar();
    layout->addWidget(status_label_);

    // 连接信号
    connect(file_table_, &QTableWidget::cellDoubleClicked, this, &CloudPage::enter_directory);
    connect(file_table_, &QTableWidget::customContextMenuRequested, this, &CloudPage::show_context_menu);
}

QWidget* CloudPage::create_toolbar()
{
    auto* toolbar = new QWidget(right_panel_);
    auto* layout = new QHBoxLayout(toolbar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // 导航按钮
    up_button_ = new QPushButton(tr("Up"), toolbar);
    up_button_->setEnabled(false);
    up_button_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    up_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(up_button_);

    home_button_ = new QPushButton(tr("Home"), toolbar);
    home_button_->setEnabled(false);
    home_button_->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
    home_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(home_button_);

    refresh_button_ = new QPushButton(tr("Refresh"), toolbar);
    refresh_button_->setEnabled(false);
    refresh_button_->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    refresh_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(refresh_button_);

    layout->addStretch();

    // 操作按钮
    upload_button_ = new QPushButton(tr("Upload"), toolbar);
    upload_button_->setEnabled(false);
    upload_button_->setIcon(style()->standardIcon(QStyle::SP_ArrowUp));
    upload_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(upload_button_);

    download_button_ = new QPushButton(tr("Download"), toolbar);
    download_button_->setEnabled(false);
    download_button_->setIcon(style()->standardIcon(QStyle::SP_ArrowDown));
    download_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(download_button_);

    new_folder_button_ = new QPushButton(tr("New Folder"), toolbar);
    new_folder_button_->setEnabled(false);
    new_folder_button_->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    new_folder_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(new_folder_button_);

    delete_button_ = new QPushButton(tr("Delete"), toolbar);
    delete_button_->setEnabled(false);
    delete_button_->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
    delete_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(delete_button_);

    // 连接信号
    connect(up_button_, &QPushButton::clicked, this, &CloudPage::go_up);
    connect(home_button_, &QPushButton::clicked, this, &CloudPage::go_home);
    connect(refresh_button_, &QPushButton::clicked, this, &CloudPage::refresh_directory);
    connect(upload_button_, &QPushButton::clicked, this, &CloudPage::upload_file);
    connect(download_button_, &QPushButton::clicked, this, &CloudPage::download_file);
    connect(new_folder_button_, &QPushButton::clicked, this, &CloudPage::create_folder);
    connect(delete_button_, &QPushButton::clicked, this, &CloudPage::delete_selected);

    return toolbar;
}

void CloudPage::create_status_bar()
{
    status_label_ = new QLabel(tr("Ready"), right_panel_);
}

void CloudPage::connect_to_storage()
{
    // TODO: 调用 libfalcon 的云存储浏览器
    // 暂时模拟连接成功
    current_config_.protocol = storage_type_combo_->currentData().toString();
    current_config_.endpoint = endpoint_edit_->text();
    current_config_.access_key = access_key_edit_->text();
    current_config_.secret_key = secret_key_edit_->text();
    current_config_.region = region_edit_->text();
    current_config_.bucket = bucket_edit_->text();

    is_connected_ = true;
    current_path_ = "/";

    // 更新UI状态
    connect_button_->setEnabled(false);
    disconnect_button_->setEnabled(true);
    up_button_->setEnabled(true);
    home_button_->setEnabled(true);
    refresh_button_->setEnabled(true);
    upload_button_->setEnabled(true);
    download_button_->setEnabled(true);
    new_folder_button_->setEnabled(true);
    delete_button_->setEnabled(true);

    connection_status_label_->setText(tr("Connected"));

    // 切换到浏览器面板
    show_browser_panel();

    // 添加示例文件
    file_table_->setRowCount(0);
    update_file_list("/");
}

void CloudPage::disconnect_storage()
{
    is_connected_ = false;

    // 更新UI状态
    connect_button_->setEnabled(true);
    disconnect_button_->setEnabled(false);
    up_button_->setEnabled(false);
    home_button_->setEnabled(false);
    refresh_button_->setEnabled(false);
    upload_button_->setEnabled(false);
    download_button_->setEnabled(false);
    new_folder_button_->setEnabled(false);
    delete_button_->setEnabled(false);

    connection_status_label_->setText(tr("Disconnected"));

    file_table_->setRowCount(0);
    current_path_edit_->clear();

    // 返回空状态
    show_empty_state();
}

void CloudPage::refresh_directory()
{
    if (!is_connected_) {
        return;
    }

    // TODO: 调用 libfalcon 的 list_directory
    update_file_list(current_path_);
}

void CloudPage::update_file_list(const QString& path)
{
    file_table_->setRowCount(0);
    current_path_ = path;
    current_path_edit_->setText(path);

    // TODO: 从 libfalcon 获取实际文件列表
    // 暂时添加示例数据
    if (path == "/") {
        // 添加文件夹
        int row = file_table_->rowCount();
        file_table_->insertRow(row);
        auto* documents_item = new QTableWidgetItem("documents");
        documents_item->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
        file_table_->setItem(row, 0, documents_item);
        file_table_->setItem(row, 1, new QTableWidgetItem("-"));
        file_table_->setItem(row, 2, new QTableWidgetItem("2025-12-27 10:30"));
        file_table_->setItem(row, 3, new QTableWidgetItem(tr("Folder")));

        row = file_table_->rowCount();
        file_table_->insertRow(row);
        auto* images_item = new QTableWidgetItem("images");
        images_item->setIcon(style()->standardIcon(QStyle::SP_DirIcon));
        file_table_->setItem(row, 0, images_item);
        file_table_->setItem(row, 1, new QTableWidgetItem("-"));
        file_table_->setItem(row, 2, new QTableWidgetItem("2025-12-26 15:20"));
        file_table_->setItem(row, 3, new QTableWidgetItem(tr("Folder")));

        row = file_table_->rowCount();
        file_table_->insertRow(row);
        auto* readme_item = new QTableWidgetItem("readme.txt");
        readme_item->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
        file_table_->setItem(row, 0, readme_item);
        file_table_->setItem(row, 1, new QTableWidgetItem("1.2 KB"));
        file_table_->setItem(row, 2, new QTableWidgetItem("2025-12-25 09:15"));
        file_table_->setItem(row, 3, new QTableWidgetItem(tr("Text File")));
    }

    status_label_->setText(tr("%1 item(s).").arg(file_table_->rowCount()));
}

void CloudPage::enter_directory(int row)
{
    if (!is_connected_) {
        return;
    }

    auto* name_item = file_table_->item(row, 0);
    QString name = name_item->text();

    // 检查是否为文件夹
    auto* type_item = file_table_->item(row, 3);
    if (type_item && type_item->text() == tr("Folder")) {
        QString new_path = current_path_;
        if (!new_path.endsWith("/")) {
            new_path += "/";
        }
        new_path += name;

        update_file_list(new_path);
    } else {
        // 文件，触发下载
        download_file();
    }
}

void CloudPage::go_up()
{
    if (!is_connected_ || current_path_ == "/") {
        return;
    }

    QString new_path = current_path_;
    qsizetype last_slash = new_path.lastIndexOf('/');
    if (last_slash > 0) {
        new_path = new_path.left(last_slash);
    } else {
        new_path = "/";
    }

    update_file_list(new_path);
}

void CloudPage::go_home()
{
    if (!is_connected_) {
        return;
    }

    update_file_list("/");
}

void CloudPage::download_file()
{
    // TODO: 调用 libfalcon 下载功能
    QMessageBox::information(this, tr("Download"), tr("Download is not implemented yet."));
}

void CloudPage::upload_file()
{
    QString file_path = QFileDialog::getOpenFileName(
        this, tr("Select a file to upload"),
        QDir::homePath(),
        tr("All Files (*.*)")
    );

    if (!file_path.isEmpty()) {
        // TODO: 调用 libfalcon 上传功能
        QMessageBox::information(this, tr("Upload"), tr("Upload %1 is not implemented yet.").arg(QFileInfo(file_path).fileName()));
    }
}

void CloudPage::delete_selected()
{
    auto selected = file_table_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("Notice"), tr("Select items first."));
        return;
    }

    auto reply = QMessageBox::question(
        this, tr("Confirm Delete"),
        tr("Delete %1 item(s)?").arg(file_table_->selectedItems().size() / file_table_->columnCount()),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply == QMessageBox::Yes) {
        // TODO: 调用 libfalcon 删除功能
        refresh_directory();
    }
}

void CloudPage::create_folder()
{
    bool ok;
    QString folder_name = QInputDialog::getText(
        this, tr("New Folder"),
        tr("Folder name:"),
        QLineEdit::Normal,
        "",
        &ok
    );

    if (ok && !folder_name.isEmpty()) {
        // TODO: 调用 libfalcon 创建目录功能
        refresh_directory();
    }
}

void CloudPage::show_context_menu(const QPoint& pos)
{
    if (!is_connected_) {
        return;
    }

    QMenu menu(this);

    auto* download_action = menu.addAction(tr("Download"));
    auto* rename_action = menu.addAction(tr("Rename"));
    auto* delete_action = menu.addAction(tr("Delete"));
    menu.addSeparator();
    auto* properties_action = menu.addAction(tr("Properties"));

    QAction* action = menu.exec(file_table_->mapToGlobal(pos));

    if (action == download_action) {
        download_file();
    } else if (action == rename_action) {
        // TODO: 实现重命名功能
        QMessageBox::information(this, tr("Notice"), tr("Rename is not implemented yet."));
    } else if (action == delete_action) {
        delete_selected();
    } else if (action == properties_action) {
        // TODO: 显示文件属性
        QMessageBox::information(this, tr("Notice"), tr("Properties view is not implemented yet."));
    }
}

QString CloudPage::format_size(uint64_t bytes) const
{
    if (bytes == 0) {
        return "0 B";
    }

    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        unit_index++;
    }

    return QString("%1 %2").arg(size, 0, 'f', 1).arg(units[unit_index]);
}

void CloudPage::create_empty_state()
{
    empty_state_widget_ = new QWidget(this);
    auto* layout = new QVBoxLayout(empty_state_widget_);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(24);

    // 云盘图标
    auto* icon_label = new QLabel(empty_state_widget_);
    icon_label->setPixmap(style()->standardIcon(QStyle::SP_DriveNetIcon).pixmap(64, 64));
    icon_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon_label);

    // 提示文本
    auto* title_label = new QLabel(tr("No cloud storage configured"), empty_state_widget_);
    auto empty_title_font = title_label->font();
    empty_title_font.setPointSize(16);
    empty_title_font.setBold(true);
    title_label->setFont(empty_title_font);
    title_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(title_label);

    auto* desc_label = new QLabel(tr("Add a cloud storage configuration to browse and manage your files."), empty_state_widget_);
    desc_label->setAlignment(Qt::AlignCenter);
    desc_label->setWordWrap(true);
    layout->addWidget(desc_label);

    layout->addSpacing(16);

    // 添加配置按钮
    auto* add_button = new QPushButton(tr("Add Cloud Storage"), empty_state_widget_);
    add_button->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    add_button->setCursor(Qt::PointingHandCursor);
    add_button->setMinimumWidth(200);
    connect(add_button, &QPushButton::clicked, this, [this]() {
        show_config_panel();
    });
    layout->addWidget(add_button, 0, Qt::AlignCenter);

    layout->addStretch();
}

void CloudPage::show_empty_state()
{
    stacked_widget_->setCurrentWidget(empty_state_widget_);
}

void CloudPage::show_config_panel()
{
    stacked_widget_->setCurrentWidget(splitter_);
    // 默认显示左侧配置面板
    left_panel_->show();
    right_panel_->hide();
}

void CloudPage::show_browser_panel()
{
    stacked_widget_->setCurrentWidget(splitter_);
    // 显示完整界面
    left_panel_->show();
    right_panel_->show();
}

} // namespace falcon::desktop
