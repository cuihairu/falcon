/**
 * @file cloud_page_aux.cpp
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
    if (!config_name.isEmpty()) {
        current_config_name_ = config_name;
    }
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

} // namespace falcon::desktop
