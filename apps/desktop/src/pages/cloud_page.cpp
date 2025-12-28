/**
 * @file cloud_page.cpp
 * @brief 云盘资源浏览页面实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "cloud_page.hpp"
#include "../styles.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QFileDialog>
#include <QMessageBox>
#include <QMenu>

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
    auto* title_label = new QLabel("云盘资源", this);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 24px;
            font-weight: 700;
            color: #323130;
        }
    )");
    main_layout->addWidget(title_label);

    // 创建堆叠窗口用于视图切换
    stacked_widget_ = new QStackedWidget(this);

    // 创建空状态视图
    create_empty_state();
    stacked_widget_->addWidget(empty_state_widget_);

    // 创建分割器（配置面板 + 文件浏览器）
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->setStyleSheet(R"(
        QSplitter::handle {
            background-color: #e1dfdd;
            width: 1px;
        }
        QSplitter::handle:hover {
            background-color: #0078d4;
        }
    )");

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
    auto* title_label = new QLabel("云存储配置", left_panel_);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 18px;
            font-weight: 600;
            color: #323130;
            padding: 8px 0;
        }
    )");
    layout->addWidget(title_label);

    // 存储类型选择
    auto* type_layout = new QHBoxLayout();
    auto* type_label = new QLabel("类型:", left_panel_);
    type_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    storage_type_combo_ = new QComboBox(left_panel_);
    storage_type_combo_->setStyleSheet(get_combo_stylesheet());
    storage_type_combo_->addItem("Amazon S3", "s3");
    storage_type_combo_->addItem("阿里云 OSS", "oss");
    storage_type_combo_->addItem("腾讯云 COS", "cos");
    storage_type_combo_->addItem("七牛云 Kodo", "kodo");
    storage_type_combo_->addItem("又拍云 Upyun", "upyun");
    type_layout->addWidget(type_label);
    type_layout->addWidget(storage_type_combo_);
    layout->addLayout(type_layout);

    // 端点
    auto* endpoint_layout = new QHBoxLayout();
    auto* endpoint_label = new QLabel("端点:", left_panel_);
    endpoint_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    endpoint_edit_ = new QLineEdit(left_panel_);
    endpoint_edit_->setStyleSheet(get_input_stylesheet());
    endpoint_edit_->setPlaceholderText("s3.amazonaws.com");
    endpoint_layout->addWidget(endpoint_label);
    endpoint_layout->addWidget(endpoint_edit_);
    layout->addLayout(endpoint_layout);

    // 访问密钥
    auto* access_key_layout = new QHBoxLayout();
    auto* access_key_label = new QLabel("Access Key:", left_panel_);
    access_key_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    access_key_edit_ = new QLineEdit(left_panel_);
    access_key_edit_->setStyleSheet(get_input_stylesheet());
    access_key_edit_->setEchoMode(QLineEdit::Password);
    access_key_layout->addWidget(access_key_label);
    access_key_layout->addWidget(access_key_edit_);
    layout->addLayout(access_key_layout);

    // 密钥
    auto* secret_key_layout = new QHBoxLayout();
    auto* secret_key_label = new QLabel("Secret Key:", left_panel_);
    secret_key_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    secret_key_edit_ = new QLineEdit(left_panel_);
    secret_key_edit_->setStyleSheet(get_input_stylesheet());
    secret_key_edit_->setEchoMode(QLineEdit::Password);
    secret_key_layout->addWidget(secret_key_label);
    secret_key_layout->addWidget(secret_key_edit_);
    layout->addLayout(secret_key_layout);

    // 区域
    auto* region_layout = new QHBoxLayout();
    auto* region_label = new QLabel("区域:", left_panel_);
    region_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    region_edit_ = new QLineEdit(left_panel_);
    region_edit_->setStyleSheet(get_input_stylesheet());
    region_edit_->setPlaceholderText("us-east-1");
    region_layout->addWidget(region_label);
    region_layout->addWidget(region_edit_);
    layout->addLayout(region_layout);

    // 存储桶
    auto* bucket_layout = new QHBoxLayout();
    auto* bucket_label = new QLabel("存储桶:", left_panel_);
    bucket_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    bucket_edit_ = new QLineEdit(left_panel_);
    bucket_edit_->setStyleSheet(get_input_stylesheet());
    bucket_layout->addWidget(bucket_label);
    bucket_layout->addWidget(bucket_edit_);
    layout->addLayout(bucket_layout);

    layout->addStretch();

    // 连接按钮
    connect_button_ = new QPushButton("连接", left_panel_);
    connect_button_->setStyleSheet(get_button_stylesheet(true));
    connect_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(connect_button_);

    disconnect_button_ = new QPushButton("断开连接", left_panel_);
    disconnect_button_->setEnabled(false);
    disconnect_button_->setStyleSheet(get_button_stylesheet(false));
    disconnect_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(disconnect_button_);

    save_config_button_ = new QPushButton("保存配置", left_panel_);
    save_config_button_->setStyleSheet(get_button_stylesheet(false));
    save_config_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(save_config_button_);

    // 连接信号
    connect(connect_button_, &QPushButton::clicked, this, &CloudPage::connect_to_storage);
    connect(disconnect_button_, &QPushButton::clicked, this, &CloudPage::disconnect_storage);
    connect(save_config_button_, &QPushButton::clicked, this, [this]() {
        // TODO: 实现保存配置功能
        QMessageBox::information(this, "提示", "配置保存功能待实现");
    });

    // 连接状态
    connection_status_label_ = new QLabel("未连接", left_panel_);
    connection_status_label_->setStyleSheet(R"(
        QLabel {
            color: #a19f9d;
            padding: 12px;
            font-size: 13px;
        }
    )");
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
    auto* path_label = new QLabel("路径:", right_panel_);
    path_label->setStyleSheet("color: #605e5c; font-size: 13px;");
    current_path_edit_ = new QLineEdit(right_panel_);
    current_path_edit_->setStyleSheet(get_input_stylesheet());
    current_path_edit_->setReadOnly(true);
    current_path_edit_->setText("/");
    path_layout->addWidget(path_label);
    path_layout->addWidget(current_path_edit_);
    layout->addLayout(path_layout);

    // 文件列表
    file_table_ = new QTableWidget(right_panel_);
    file_table_->setColumnCount(5);
    file_table_->setHorizontalHeaderLabels({
        "名称", "大小", "修改时间", "类型", "操作"
    });

    file_table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    file_table_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    file_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    file_table_->horizontalHeader()->setStretchLastSection(false);
    file_table_->setContextMenuPolicy(Qt::CustomContextMenu);
    file_table_->setStyleSheet(get_table_stylesheet());

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
    up_button_ = new QPushButton("↑ 上级", toolbar);
    up_button_->setEnabled(false);
    up_button_->setStyleSheet(get_button_stylesheet(false));
    up_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(up_button_);

    home_button_ = new QPushButton("⌂ 根目录", toolbar);
    home_button_->setEnabled(false);
    home_button_->setStyleSheet(get_button_stylesheet(false));
    home_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(home_button_);

    refresh_button_ = new QPushButton("↻ 刷新", toolbar);
    refresh_button_->setEnabled(false);
    refresh_button_->setStyleSheet(get_button_stylesheet(false));
    refresh_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(refresh_button_);

    layout->addStretch();

    // 操作按钮
    upload_button_ = new QPushButton("↑ 上传", toolbar);
    upload_button_->setEnabled(false);
    upload_button_->setStyleSheet(get_button_stylesheet(false));
    upload_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(upload_button_);

    download_button_ = new QPushButton("↓ 下载", toolbar);
    download_button_->setEnabled(false);
    download_button_->setStyleSheet(get_button_stylesheet(false));
    download_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(download_button_);

    new_folder_button_ = new QPushButton("+ 新建文件夹", toolbar);
    new_folder_button_->setEnabled(false);
    new_folder_button_->setStyleSheet(get_button_stylesheet(false));
    new_folder_button_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(new_folder_button_);

    delete_button_ = new QPushButton("× 删除", toolbar);
    delete_button_->setEnabled(false);
    delete_button_->setStyleSheet(get_button_stylesheet(false));
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
    status_label_ = new QLabel("就绪", right_panel_);
    status_label_->setStyleSheet(R"(
        QLabel {
            padding: 8px;
            color: #605e5c;
            font-size: 13px;
        }
    )");
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

    connection_status_label_->setText("已连接");
    connection_status_label_->setStyleSheet(R"(
        QLabel {
            color: #107c10;
            padding: 12px;
            font-size: 13px;
            font-weight: 600;
        }
    )");

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

    connection_status_label_->setText("未连接");
    connection_status_label_->setStyleSheet(R"(
        QLabel {
            color: #a19f9d;
            padding: 12px;
            font-size: 13px;
        }
    )");

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
        file_table_->setItem(row, 0, new QTableWidgetItem("📁 documents"));
        file_table_->setItem(row, 1, new QTableWidgetItem("-"));
        file_table_->setItem(row, 2, new QTableWidgetItem("2025-12-27 10:30"));
        file_table_->setItem(row, 3, new QTableWidgetItem("文件夹"));

        row = file_table_->rowCount();
        file_table_->insertRow(row);
        file_table_->setItem(row, 0, new QTableWidgetItem("📁 images"));
        file_table_->setItem(row, 1, new QTableWidgetItem("-"));
        file_table_->setItem(row, 2, new QTableWidgetItem("2025-12-26 15:20"));
        file_table_->setItem(row, 3, new QTableWidgetItem("文件夹"));

        row = file_table_->rowCount();
        file_table_->insertRow(row);
        file_table_->setItem(row, 0, new QTableWidgetItem("📄 readme.txt"));
        file_table_->setItem(row, 1, new QTableWidgetItem("1.2 KB"));
        file_table_->setItem(row, 2, new QTableWidgetItem("2025-12-25 09:15"));
        file_table_->setItem(row, 3, new QTableWidgetItem("文本文件"));
    }

    status_label_->setText(QString("共 %1 项").arg(file_table_->rowCount()));
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
    if (type_item && type_item->text() == "文件夹") {
        // 移除图标前缀
        if (name.startsWith("📁 ")) {
            name = name.mid(3);
        }

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
    QMessageBox::information(this, "下载", "下载功能待实现");
}

void CloudPage::upload_file()
{
    QString file_path = QFileDialog::getOpenFileName(
        this, "选择上传文件",
        QDir::homePath(),
        "所有文件 (*.*)"
    );

    if (!file_path.isEmpty()) {
        // TODO: 调用 libfalcon 上传功能
        QMessageBox::information(this, "上传", QString("上传 %1 功能待实现").arg(QFileInfo(file_path).fileName()));
    }
}

void CloudPage::delete_selected()
{
    auto selected = file_table_->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::warning(this, "提示", "请先选择要删除的文件或文件夹");
        return;
    }

    auto reply = QMessageBox::question(
        this, "确认删除",
        QString("确定要删除选中的 %1 项吗？").arg(file_table_->selectedItems().size() / file_table_->columnCount()),
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
        this, "新建文件夹",
        "文件夹名称:",
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

    auto* download_action = menu.addAction("下载");
    auto* rename_action = menu.addAction("重命名");
    auto* delete_action = menu.addAction("删除");
    menu.addSeparator();
    auto* properties_action = menu.addAction("属性");

    QAction* action = menu.exec(file_table_->mapToGlobal(pos));

    if (action == download_action) {
        download_file();
    } else if (action == rename_action) {
        // TODO: 实现重命名功能
        QMessageBox::information(this, "提示", "重命名功能待实现");
    } else if (action == delete_action) {
        delete_selected();
    } else if (action == properties_action) {
        // TODO: 显示文件属性
        QMessageBox::information(this, "提示", "属性查看功能待实现");
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

QString CloudPage::get_file_icon(const QString& filename) const
{
    // 根据文件扩展名返回图标
    QString ext = QFileInfo(filename).suffix().toLower();

    if (ext == "txt" || ext == "md" || ext == "json" || ext == "xml") {
        return "📄";
    } else if (ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif") {
        return "🖼️";
    } else if (ext == "mp4" || ext == "avi" || ext == "mkv") {
        return "🎬";
    } else if (ext == "mp3" || ext == "flac" || ext == "wav") {
        return "🎵";
    } else if (ext == "zip" || ext == "rar" || ext == "7z") {
        return "📦";
    } else if (ext == "pdf") {
        return "📕";
    } else {
        return "📄";
    }
}

void CloudPage::create_empty_state()
{
    empty_state_widget_ = new QWidget(this);
    auto* layout = new QVBoxLayout(empty_state_widget_);
    layout->setAlignment(Qt::AlignCenter);
    layout->setSpacing(24);

    // 云盘图标
    auto* icon_label = new QLabel("☁️", empty_state_widget_);
    icon_label->setStyleSheet(R"(
        QLabel {
            font-size: 80px;
            color: #0078d4;
        }
    )");
    icon_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(icon_label);

    // 提示文本
    auto* title_label = new QLabel("还没有添加云存储配置", empty_state_widget_);
    title_label->setStyleSheet(R"(
        QLabel {
            font-size: 20px;
            font-weight: 600;
            color: #323130;
        }
    )");
    title_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(title_label);

    auto* desc_label = new QLabel("添加云存储配置后，即可浏览和管理您的云端文件", empty_state_widget_);
    desc_label->setStyleSheet(R"(
        QLabel {
            font-size: 14px;
            color: #605e5c;
        }
    )");
    desc_label->setAlignment(Qt::AlignCenter);
    layout->addWidget(desc_label);

    layout->addSpacing(16);

    // 添加配置按钮
    auto* add_button = new QPushButton("➕ 添加云存储配置", empty_state_widget_);
    add_button->setStyleSheet(get_button_stylesheet(true));
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
