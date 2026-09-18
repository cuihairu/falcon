/**
 * @file cloud_page.cpp
 * @brief 云盘资源浏览页面实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "cloud_page.hpp"
#include "../services/storage_service.hpp"
#include "../utils/icon_utils.hpp"
#include <falcon/storage/resource_browser.hpp>

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
#include <QSettings>

namespace falcon::desktop {

CloudPage::CloudPage(QWidget* parent)
    : QWidget(parent)
    , splitter_(nullptr)
    , empty_state_widget_(nullptr)
    , stacked_widget_(nullptr)
    , left_panel_(nullptr)
    , right_panel_(nullptr)
    , storage_service_(new StorageService(this)) {
    setup_ui();
    load_configs();  // 加载已保存的配置

    // 连接 StorageService 信号
    connect(storage_service_.get(), &StorageService::connected,
            this, &CloudPage::on_storage_connected);
    connect(storage_service_.get(), &StorageService::disconnected,
            this, &CloudPage::on_storage_disconnected);
    connect(storage_service_.get(), &StorageService::error,
            this, &CloudPage::on_storage_error);
    connect(storage_service_.get(), &StorageService::directory_loaded,
            this, &CloudPage::on_directory_loaded);

    // 连接下载请求信号到主窗口（假设主窗口会处理）
    connect(storage_service_.get(), &StorageService::download_requested,
            this, [this](const QString& url, const QString& local_path) {
                emit download_requested(url, local_path);
            });
}

CloudPage::~CloudPage() = default;

void CloudPage::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(18, 18, 18, 18);
    main_layout->setSpacing(14);

    // 页面 hero(标题 + 描述,与其余页面同一套 #downloadHero 结构)
    auto* hero = new QWidget(this);
    hero->setObjectName("downloadHero");
    auto* hero_layout = new QVBoxLayout(hero);
    hero_layout->setContentsMargins(20, 18, 20, 18);
    hero_layout->setSpacing(4);

    auto* hero_title = new QLabel(tr("云盘空间"), hero);
    hero_title->setObjectName("heroTitle");
    hero_layout->addWidget(hero_title);

    auto* hero_desc = new QLabel(tr("连接对象存储，浏览、上传与管理远端文件。"), hero);
    hero_desc->setObjectName("heroDescription");
    hero_layout->addWidget(hero_desc);

    main_layout->addWidget(hero);

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

    // 标题(字号/字重由 QSS #sectionTitle 统一管控)
    auto* title_label = new QLabel(tr("存储配置"), left_panel_);
    title_label->setObjectName("sectionTitle");
    layout->addWidget(title_label);

    // 存储类型选择
    auto* type_layout = new QHBoxLayout();
    auto* type_label = new QLabel(tr("类型:"), left_panel_);
    storage_type_combo_ = new QComboBox(left_panel_);
    for (const auto& browser : falcon::BrowserFactory::available_browsers()) {
        storage_type_combo_->addItem(
            QString::fromStdString(browser.display_name),
            QString::fromStdString(browser.protocol));
    }
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
    auto* region_label = new QLabel(tr("区域:"), left_panel_);
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
    connect_button_ = new QPushButton(tr("连接"), left_panel_);
    connect_button_->setIcon(icons::themed(icons::Id::CheckCircle, icons::ColorRole::Accent));
    connect_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(connect_button_);

    disconnect_button_ = new QPushButton(tr("断开"), left_panel_);
    disconnect_button_->setEnabled(false);
    disconnect_button_->setIcon(icons::themed(icons::Id::X, icons::ColorRole::TextSecondary));
    disconnect_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(disconnect_button_);

    save_config_button_ = new QPushButton(tr("保存配置"), left_panel_);
    save_config_button_->setIcon(icons::themed(icons::Id::File, icons::ColorRole::TextSecondary));
    save_config_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(save_config_button_);

    // 连接信号
    connect(connect_button_, &QPushButton::clicked, this, &CloudPage::connect_to_storage);
    connect(disconnect_button_, &QPushButton::clicked, this, &CloudPage::disconnect_storage);
    connect(save_config_button_, &QPushButton::clicked, this, &CloudPage::save_config);

    // 连接状态
    connection_status_label_ = new QLabel(tr("未连接"), left_panel_);
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
    auto* path_label = new QLabel(tr("路径:"), right_panel_);
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
        tr("名称"), tr("大小"), tr("修改时间"), tr("类型"), tr("操作")
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
    up_button_ = new QPushButton(tr("上级"), toolbar);
    up_button_->setEnabled(false);
    up_button_->setIcon(icons::themed(icons::Id::ArrowUp, icons::ColorRole::TextSecondary));
    up_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(up_button_);

    home_button_ = new QPushButton(tr("根目录"), toolbar);
    home_button_->setEnabled(false);
    home_button_->setIcon(icons::themed(icons::Id::Folder, icons::ColorRole::TextSecondary));
    home_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(home_button_);

    refresh_button_ = new QPushButton(tr("刷新"), toolbar);
    refresh_button_->setEnabled(false);
    refresh_button_->setIcon(icons::themed(icons::Id::Refresh, icons::ColorRole::TextSecondary));
    refresh_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(refresh_button_);

    layout->addStretch();

    // 操作按钮
    upload_button_ = new QPushButton(tr("上传"), toolbar);
    upload_button_->setEnabled(false);
    upload_button_->setIcon(icons::themed(icons::Id::Upload, icons::ColorRole::TextSecondary));
    upload_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(upload_button_);

    download_button_ = new QPushButton(tr("下载"), toolbar);
    download_button_->setEnabled(false);
    download_button_->setIcon(icons::themed(icons::Id::Download, icons::ColorRole::TextSecondary));
    download_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(download_button_);

    new_folder_button_ = new QPushButton(tr("新建文件夹"), toolbar);
    new_folder_button_->setEnabled(false);
    new_folder_button_->setIcon(icons::themed(icons::Id::FolderPlus, icons::ColorRole::TextSecondary));
    new_folder_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(new_folder_button_);

    delete_button_ = new QPushButton(tr("删除"), toolbar);
    delete_button_->setEnabled(false);
    delete_button_->setIcon(icons::themed(icons::Id::Trash, icons::ColorRole::TextSecondary));
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
    status_label_ = new QLabel(tr("就绪"), right_panel_);
}

void CloudPage::connect_to_storage()
{
    // 收集配置信息
    current_config_.protocol = storage_type_combo_->currentData().toString();
    current_config_.endpoint = endpoint_edit_->text();
    current_config_.access_key = access_key_edit_->text();
    current_config_.secret_key = secret_key_edit_->text();
    current_config_.region = region_edit_->text();
    current_config_.bucket = bucket_edit_->text();

    // 生成配置名称
    const QString protocol_name = storage_type_combo_->currentText();
    current_config_.name = QString("%1 (%2)").arg(protocol_name, current_config_.endpoint);
    current_config_name_ = current_config_.name;

    // 使用 StorageService 连接
    if (storage_service_->connect_storage(current_config_)) {
        status_label_->setText(tr("连接中..."));
    }
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

    connection_status_label_->setText(tr("未连接"));

    file_table_->setRowCount(0);
    current_path_edit_->clear();

    // 返回空状态
    show_empty_state();
}

void CloudPage::refresh_directory()
{
    if (!is_connected_ || current_config_name_.isEmpty()) {
        return;
    }

    status_label_->setText(tr("加载中..."));

    // 调用 StorageService 的 list_directory
    storage_service_->list_directory(current_config_name_, current_path_,
        [this](const QList<RemoteResourceInfo>& resources) {
            // 更新 UI 需要在主线程执行
            QMetaObject::invokeMethod(this, [this, resources]() {
                update_file_list_with_data(resources);
            }, Qt::QueuedConnection);
        });
}

void CloudPage::update_file_list(const QString& path)
{
    // 这个方法现在只更新路径，实际数据通过 list_directory 回调获取
    current_path_ = path;
    current_path_edit_->setText(path);
    refresh_directory();
}

void CloudPage::update_file_list_with_data(const QList<RemoteResourceInfo>& resources)
{
    file_table_->setRowCount(0);

    for (const auto& resource : resources) {
        int row = file_table_->rowCount();
        file_table_->insertRow(row);

        // 名称
        auto* name_item = new QTableWidgetItem(resource.name);
        if (resource.type == "directory") {
            name_item->setIcon(icons::themed(icons::Id::Folder, icons::ColorRole::TextSecondary));
        } else {
            name_item->setIcon(icons::themed(icons::Id::File, icons::ColorRole::TextSecondary));
        }
        file_table_->setItem(row, 0, name_item);

        // 大小
        QString size_str = resource.type == "directory" ? "-" :
                          (resource.size > 0 ? QString::number(resource.size) : "-");
        file_table_->setItem(row, 1, new QTableWidgetItem(size_str));

        // 修改时间
        file_table_->setItem(row, 2, new QTableWidgetItem(resource.modified_time));

        // 类型
        QString type_str = resource.type == "directory" ? tr("文件夹") : resource.type;
        file_table_->setItem(row, 3, new QTableWidgetItem(type_str));
    }

    status_label_->setText(tr("共 %1 项。").arg(resources.size()));
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
    if (type_item && type_item->text() == tr("文件夹")) {
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
    auto selected = file_table_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("请选择要下载的文件。"));
        return;
    }

    int row = selected.first()->row();
    auto* name_item = file_table_->item(row, 0);
    auto* type_item = file_table_->item(row, 3);

    if (!name_item) {
        return;
    }

    // 检查是否为文件夹
    if (type_item && type_item->text() == tr("文件夹")) {
        QMessageBox::information(this, tr("提示"), tr("文件夹不能直接下载。"));
        return;
    }

    QString file_name = name_item->text();
    QString remote_path = current_path_;
    if (!remote_path.endsWith("/")) {
        remote_path += "/";
    }
    remote_path += file_name;

    // 选择保存位置
    QString save_path = QFileDialog::getSaveFileName(
        this, tr("保存文件"),
        QDir::homePath() + "/" + file_name,
        tr("所有文件 (*.*)")
    );

    if (save_path.isEmpty()) {
        return;
    }

    // 发起下载请求
    storage_service_->request_download(current_config_name_, remote_path, save_path);

    status_label_->setText(tr("正在下载: %1").arg(file_name));
}

void CloudPage::upload_file()
{
    if (!is_connected_ || current_config_name_.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("尚未连接到任何存储。"));
        return;
    }

    QString file_path = QFileDialog::getOpenFileName(
        this, tr("选择要上传的文件"),
        QDir::homePath(),
        tr("所有文件 (*.*)")
    );

    if (file_path.isEmpty()) {
        return;
    }

    QFileInfo file_info(file_path);
    QString remote_path = current_path_;
    if (!remote_path.endsWith("/")) {
        remote_path += "/";
    }
    remote_path += file_info.fileName();

    status_label_->setText(tr("正在上传: %1").arg(file_info.fileName()));

    // 调用 StorageService 上传
    storage_service_->upload_file(current_config_name_, file_path, remote_path,
        [this, file_info](bool success, const QString& message) {
            QMetaObject::invokeMethod(this, [this, success, message, file_info]() {
                if (success) {
                    status_label_->setText(tr("上传完成: %1").arg(file_info.fileName()));
                    refresh_directory();
                } else {
                    status_label_->setText(tr("上传失败: %1").arg(message));
                    QMessageBox::warning(this, tr("上传失败"), message);
                }
            }, Qt::QueuedConnection);
        });
}

void CloudPage::delete_selected()
{
    if (!is_connected_ || current_config_name_.isEmpty()) {
        return;
    }

    auto selected = file_table_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, tr("提示"), tr("请先选择条目。"));
        return;
    }

    int row_count = file_table_->selectedItems().size() / file_table_->columnCount();
    auto reply = QMessageBox::question(
        this, tr("确认删除"),
        tr("确定删除这 %1 项吗?").arg(row_count),
        QMessageBox::Yes | QMessageBox::No
    );

    if (reply != QMessageBox::Yes) {
        return;
    }

    // 获取所有选中行的路径
    QSet<int> rows;
    for (auto* item : selected) {
        rows.insert(item->row());
    }

    // 删除每个选中的项
    for (int row : rows) {
        auto* name_item = file_table_->item(row, 0);
        if (!name_item) {
            continue;
        }

        QString name = name_item->text();
        QString remote_path = current_path_;
        if (!remote_path.endsWith("/")) {
            remote_path += "/";
        }
        remote_path += name;

        if (!storage_service_->remove_resource(current_config_name_, remote_path, false)) {
            QMessageBox::warning(this, tr("删除失败"),
                tr("删除失败: %1").arg(name));
        }
    }

    refresh_directory();
}

void CloudPage::create_folder()
{
    if (!is_connected_ || current_config_name_.isEmpty()) {
        return;
    }

    bool ok;
    QString folder_name = QInputDialog::getText(
        this, tr("新建文件夹"),
        tr("文件夹名称:"),
        QLineEdit::Normal,
        "",
        &ok
    );

    if (!ok || folder_name.isEmpty()) {
        return;
    }

    // 验证文件夹名称
    if (folder_name.contains('/') || folder_name.contains('\\')) {
        QMessageBox::warning(this, tr("名称无效"),
            tr("文件夹名称不能包含 '/' 或 '\\'"));
        return;
    }

    QString folder_path = current_path_;
    if (!folder_path.endsWith("/")) {
        folder_path += "/";
    }
    folder_path += folder_name;

    if (storage_service_->create_directory(current_config_name_, folder_path)) {
        status_label_->setText(tr("已创建文件夹: %1").arg(folder_name));
        refresh_directory();
    } else {
        QMessageBox::warning(this, tr("创建失败"),
            tr("创建文件夹失败: %1").arg(folder_name));
    }
}

void CloudPage::show_context_menu(const QPoint& pos)
{
    if (!is_connected_) {
        return;
    }

    QMenu menu(this);

    auto* download_action = menu.addAction(tr("下载"));
    auto* rename_action = menu.addAction(tr("重命名"));
    auto* delete_action = menu.addAction(tr("删除"));
    menu.addSeparator();
    auto* properties_action = menu.addAction(tr("属性"));

    QAction* action = menu.exec(file_table_->mapToGlobal(pos));

    if (action == download_action) {
        download_file();
    } else if (action == rename_action) {
        const auto selected = file_table_->selectedItems();
        if (!selected.isEmpty()) {
            rename_item(selected.first()->row());
        }
    } else if (action == delete_action) {
        delete_selected();
    } else if (action == properties_action) {
        const auto selected = file_table_->selectedItems();
        if (!selected.isEmpty()) {
            show_file_properties(selected.first()->row());
        }
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

    // 云盘图标(主题感知 SVG,64px)
    auto* icon_label = new QLabel(empty_state_widget_);
    icon_label->setPixmap(icons::themed(icons::Id::Cloud, icons::ColorRole::TextSecondary)
                              .pixmap(64, 64));
    icon_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon_label);

    // 提示文本(字号/字重由 QSS #emptyStateTitle 统一管控)
    auto* title_label = new QLabel(tr("尚未配置云存储"), empty_state_widget_);
    title_label->setObjectName("emptyStateTitle");
    title_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(title_label);

    auto* desc_label = new QLabel(tr("添加云存储配置后即可浏览与管理远端文件。"), empty_state_widget_);
    desc_label->setAlignment(Qt::AlignCenter);
    desc_label->setWordWrap(true);
    layout->addWidget(desc_label);

    layout->addSpacing(16);

    // 添加配置按钮
    auto* add_button = new QPushButton(tr("添加云存储"), empty_state_widget_);
    add_button->setIcon(icons::themed(icons::Id::FolderPlus, icons::ColorRole::TextSecondary));
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

void CloudPage::save_config()
{
    // 从输入字段获取配置
    CloudStorageConfig config;
    config.protocol = storage_type_combo_->currentData().toString();
    config.endpoint = endpoint_edit_->text().trimmed();
    config.access_key = access_key_edit_->text().trimmed();
    config.secret_key = secret_key_edit_->text().trimmed();
    config.region = region_edit_->text().trimmed();
    config.bucket = bucket_edit_->text().trimmed();

    // 生成配置名称（基于协议和端点）
    const QString protocol_name = storage_type_combo_->currentText();
    config.name = QString("%1 (%2)").arg(protocol_name, config.endpoint);

    // 验证必填字段
    if (config.endpoint.isEmpty() || config.access_key.isEmpty() || config.secret_key.isEmpty()) {
        QMessageBox::warning(this, tr("校验失败"),
            tr("请填写必填项（Endpoint、Access Key、Secret Key）。"));
        return;
    }

    // 检查是否已存在相同配置
    for (const auto& saved : saved_configs_) {
        if (saved.name == config.name && saved.endpoint == config.endpoint) {
            QMessageBox::StandardButton reply = QMessageBox::question(this,
                tr("配置已存在"),
                tr("同名配置已存在，是否覆盖?"),
                QMessageBox::Yes | QMessageBox::No);
            if (reply == QMessageBox::No) {
                return;
            }
            // 移除旧配置
            saved_configs_.removeOne(saved);
            break;
        }
    }

    // 添加到保存列表
    saved_configs_.append(config);

    // 持久化配置
    persist_configs();

    QMessageBox::information(this, tr("成功"),
        tr("配置「%1」已保存。").arg(config.name));
}

void CloudPage::load_configs()
{
    QSettings settings;
    settings.beginGroup("CloudStorage");
    const int size = settings.beginReadArray("configs");

    saved_configs_.clear();
    for (int i = 0; i < size; ++i) {
        settings.setArrayIndex(i);
        CloudStorageConfig config;
        config.name = settings.value("name").toString();
        config.protocol = settings.value("protocol").toString();
        config.endpoint = settings.value("endpoint").toString();
        config.access_key = settings.value("access_key").toString();
        config.secret_key = settings.value("secret_key").toString();
        config.region = settings.value("region").toString();
        config.bucket = settings.value("bucket").toString();
        saved_configs_.append(config);
    }

    settings.endArray();
    settings.endGroup();
}

void CloudPage::persist_configs()
{
    QSettings settings;
    settings.beginGroup("CloudStorage");
    settings.beginWriteArray("configs", saved_configs_.size());

    for (int i = 0; i < saved_configs_.size(); ++i) {
        const auto& config = saved_configs_.at(i);
        settings.setArrayIndex(i);
        settings.setValue("name", config.name);
        settings.setValue("protocol", config.protocol);
        settings.setValue("endpoint", config.endpoint);
        settings.setValue("access_key", config.access_key);
        settings.setValue("secret_key", config.secret_key);
        settings.setValue("region", config.region);
        settings.setValue("bucket", config.bucket);
    }

    settings.endArray();
    settings.endGroup();
}

void CloudPage::show_file_properties(int row)
{
    if (row < 0 || row >= file_table_->rowCount()) {
        return;
    }

    // 获取文件信息
    auto* name_item = file_table_->item(row, 0);
    auto* size_item = file_table_->item(row, 1);
    auto* date_item = file_table_->item(row, 2);
    auto* type_item = file_table_->item(row, 3);

    if (!name_item) {
        return;
    }

    QString name = name_item->text();
    QString size = size_item ? size_item->text() : "-";
    QString date = date_item ? date_item->text() : "-";
    QString type = type_item ? type_item->text() : "-";

    // 判断是文件还是文件夹
    bool is_folder = (type == tr("文件夹"));

    // 构建属性信息
    QString info;
    info += "<table border='0' cellpadding='2' cellspacing='0'>";
    info += "<tr><td colspan='2'><b>" + tr("名称") + ":</b></td></tr>";
    info += "<tr><td width='20'></td><td>" + name + "</td></tr>";
    info += "<tr><td colspan='2'>&nbsp;</td></tr>";

    info += "<tr><td colspan='2'><b>" + tr("类型") + ":</b></td></tr>";
    info += "<tr><td width='20'></td><td>" + type + "</td></tr>";
    info += "<tr><td colspan='2'>&nbsp;</td></tr>";

    info += "<tr><td colspan='2'><b>" + tr("大小") + ":</b></td></tr>";
    info += "<tr><td width='20'></td><td>" + (is_folder ? tr("（文件夹）") : size) + "</td></tr>";
    info += "<tr><td colspan='2'>&nbsp;</td></tr>";

    info += "<tr><td colspan='2'><b>" + tr("修改时间") + ":</b></td></tr>";
    info += "<tr><td width='20'></td><td>" + date + "</td></tr>";
    info += "<tr><td colspan='2'>&nbsp;</td></tr>";

    info += "<tr><td colspan='2'><b>" + tr("位置") + ":</b></td></tr>";
    info += "<tr><td width='20'></td><td>" + current_path_ + "</td></tr>";

    info += "</table>";

    // 创建对话框
    QMessageBox msg_box(this);
    msg_box.setWindowTitle(tr("属性"));
    msg_box.setText(name);
    msg_box.setInformativeText(info);
    msg_box.setTextFormat(Qt::RichText);

    // 设置图标
    if (is_folder) {
        msg_box.setIconPixmap(icons::themed(icons::Id::Folder, icons::ColorRole::TextSecondary)
                                  .pixmap(48, 48));
    } else {
        msg_box.setIconPixmap(icons::themed(icons::Id::File, icons::ColorRole::TextSecondary)
                                  .pixmap(48, 48));
    }

    msg_box.exec();
}

void CloudPage::rename_item(int row)
{
    if (!is_connected_ || current_config_name_.isEmpty()) {
        return;
    }

    if (row < 0 || row >= file_table_->rowCount()) {
        return;
    }

    auto* name_item = file_table_->item(row, 0);
    if (!name_item) {
        return;
    }

    const QString old_name = name_item->text();

    // 获取新名称
    bool ok = false;
    QString new_name = QInputDialog::getText(
        this,
        tr("重命名"),
        tr("输入新名称:"),
        QLineEdit::Normal,
        old_name,
        &ok
    );

    if (!ok || new_name.isEmpty() || new_name == old_name) {
        return;
    }

    // 验证新名称
    if (new_name.contains('/') || new_name.contains('\\') || new_name.contains(':')) {
        QMessageBox::warning(this, tr("名称无效"),
            tr("名称不能包含特殊字符: / \\ :"));
        return;
    }

    // 构建旧路径和新路径
    QString old_path = current_path_;
    if (!old_path.endsWith("/")) {
        old_path += "/";
    }
    old_path += old_name;

    QString new_path = current_path_;
    if (!new_path.endsWith("/")) {
        new_path += "/";
    }
    new_path += new_name;

    // 调用重命名 API
    if (storage_service_->rename_resource(current_config_name_, old_path, new_path)) {
        status_label_->setText(tr("已将「%1」重命名为「%2」。").arg(old_name, new_name));
        refresh_directory();
    } else {
        QMessageBox::warning(this, tr("重命名失败"),
            tr("重命名「%1」为「%2」失败。").arg(old_name, new_name));
    }
}

// ============================================================================
// StorageService 回调实现
// ============================================================================

void CloudPage::on_storage_connected(const QString& config_name) {
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

    connection_status_label_->setText(tr("已连接"));

    // 切换到浏览器面板
    show_browser_panel();

    // 加载根目录内容
    storage_service_->list_directory(config_name, "/", nullptr);
}

void CloudPage::on_storage_disconnected(const QString& config_name) {
    (void)config_name;
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

    connection_status_label_->setText(tr("未连接"));

    file_table_->setRowCount(0);
    current_path_edit_->clear();

    // 返回空状态
    show_empty_state();
}

void CloudPage::on_storage_error(const QString& config_name, const QString& message) {
    (void)config_name;
    status_label_->setText(tr("错误: %1").arg(message));
    QMessageBox::warning(this, tr("存储错误"), message);
}

void CloudPage::on_directory_loaded(const QString& config_name, const QString& path,
                                     const QList<RemoteResourceInfo>& resources) {
    (void)config_name;
    current_path_ = path;
    current_path_edit_->setText(path);

    file_table_->setRowCount(0);

    for (const auto& resource : resources) {
        int row = file_table_->rowCount();
        file_table_->insertRow(row);

        // 名称
        auto* name_item = new QTableWidgetItem(resource.name);
        if (resource.type == "directory") {
            name_item->setIcon(icons::themed(icons::Id::Folder, icons::ColorRole::TextSecondary));
        } else {
            name_item->setIcon(icons::themed(icons::Id::File, icons::ColorRole::TextSecondary));
        }
        file_table_->setItem(row, 0, name_item);

        // 大小
        QString size_str = resource.type == "directory" ? "-" : QString::number(resource.size);
        file_table_->setItem(row, 1, new QTableWidgetItem(size_str));

        // 修改时间
        file_table_->setItem(row, 2, new QTableWidgetItem(resource.modified_time));

        // 类型
        QString type_str = resource.type == "directory" ? tr("文件夹") : resource.type;
        file_table_->setItem(row, 3, new QTableWidgetItem(type_str));
    }

    status_label_->setText(tr("共 %1 项。").arg(resources.size()));
}

} // namespace falcon::desktop
