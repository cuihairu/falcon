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
#include <falcon/storage/storage_presets.hpp>

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

    // 连接预设（厂商模板一键回填；首项为自定义）
    auto* preset_layout = new QHBoxLayout();
    auto* preset_label = new QLabel(tr("预设:"), left_panel_);
    preset_combo_ = new QComboBox(left_panel_);
    preset_combo_->addItem(tr("自定义（手填全部字段）"), QString());
    QString last_category;
    for (const auto& preset : falcon::storage_presets()) {
        const QString category =
            preset.category == "s3" ? tr("对象存储") : tr("网盘");
        if (category != last_category) {
            preset_combo_->insertSeparator(preset_combo_->count());
            last_category = category;
        }
        preset_combo_->addItem(QString("%1 · %2").arg(category,
                                  QString::fromStdString(preset.display_name)),
                               QString::fromStdString(preset.id));
    }
    connect(preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &CloudPage::apply_preset);
    preset_layout->addWidget(preset_label);
    preset_layout->addWidget(preset_combo_);
    layout->addLayout(preset_layout);

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

    // 预设说明（所选厂商的鉴权形态/边界一句话）
    preset_hint_label_ = new QLabel(left_panel_);
    preset_hint_label_->setWordWrap(true);
    preset_hint_label_->setObjectName("heroDescription");
    layout->addWidget(preset_hint_label_);

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

void CloudPage::apply_preset(int index)
{
    const QString preset_id = preset_combo_->itemData(index).toString();
    if (preset_id.isEmpty()) {
        // 自定义：恢复 S3 默认占位与通用提示
        endpoint_edit_->setPlaceholderText("s3.amazonaws.com");
        region_edit_->setPlaceholderText("us-east-1");
        access_key_edit_->setPlaceholderText(QString());
        secret_key_edit_->setPlaceholderText(QString());
        preset_hint_label_->setText(
            tr("手动填写端点、密钥与区域；类型决定底层协议客户端。"));
        return;
    }

    const auto* preset =
        falcon::find_storage_preset(preset_id.toStdString());
    if (!preset) {
        return;
    }

    // 类型跟随预设（工厂协议键）
    const QString protocol =
        QString::fromStdString(preset->browser_protocol);
    const int type_index = storage_type_combo_->findData(protocol);
    if (type_index >= 0) {
        storage_type_combo_->setCurrentIndex(type_index);
    }

    // 端点/区域模板回填（<...> 占位段由用户替换）
    endpoint_edit_->setText(
        QString::fromStdString(preset->endpoint_template));
    endpoint_edit_->setPlaceholderText(
        preset->endpoint_template.empty()
            ? tr("自定义端点，如 http://nas:9000")
            : QString::fromStdString(preset->endpoint_template));
    region_edit_->setText(
        QString::fromStdString(preset->region_default));

    // 凭据字段占位按预设提示（S3：Access/Secret；网盘：用户名/密码）
    if (preset->category == "webdav") {
        access_key_edit_->setPlaceholderText(
            QString::fromStdString(preset->username_hint));
        secret_key_edit_->setPlaceholderText(tr("密码 / 应用密码"));
    } else {
        access_key_edit_->setPlaceholderText(
            QString::fromStdString(preset->username_hint));
        secret_key_edit_->setPlaceholderText(tr("Secret Access Key"));
    }

    preset_hint_label_->setText(
        QString::fromStdString(preset->description));
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
        tr("文件名"), tr("大小"), tr("上传时间"), tr("存储位置"), tr("操作")
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
    status_label_ = new QLabel(QString(), right_panel_);
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

        // 上传时间（远端文件时间戳）
        file_table_->setItem(row, 2, new QTableWidgetItem(resource.modified_time));

        // 存储位置：当前连接的配置名（真实数据）；类型移入 tooltip 不丢失
        QString type_str = resource.type == "directory" ? tr("文件夹") : resource.type;
        const QString location = current_config_name_.isEmpty()
                                     ? QStringLiteral("—") : current_config_name_;
        auto* location_item = new QTableWidgetItem(location);
        location_item->setToolTip(tr("类型：%1").arg(type_str));
        file_table_->setItem(row, 3, location_item);
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

} // namespace falcon::desktop
