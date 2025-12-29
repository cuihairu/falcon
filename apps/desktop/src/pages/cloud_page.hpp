/**
 * @file cloud_page.hpp
 * @brief 云盘资源浏览页面
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QSplitter>
#include <QStackedWidget>

namespace falcon::desktop {

/**
 * @brief 云存储配置项
 */
struct CloudStorageConfig {
    QString name;           // 显示名称
    QString protocol;       // 协议 (s3, oss, cos, kodo, upyun)
    QString endpoint;       // 服务端点
    QString access_key;     // 访问密钥ID
    QString secret_key;     // 密钥
    QString region;         // 区域
    QString bucket;         // 存储桶
};

/**
 * @brief 云盘资源浏览页面
 *
 * 支持多种对象存储（S3、OSS、COS、Kodo、Upyun）
 */
class CloudPage : public QWidget
{
    Q_OBJECT

public:
    explicit CloudPage(QWidget* parent = nullptr);
    ~CloudPage() override;

private slots:
    // 连接到云存储
    void connect_to_storage();

    // 断开连接
    void disconnect_storage();

    // 刷新当前目录
    void refresh_directory();

    // 进入目录
    void enter_directory(int row);

    // 返回上级目录
    void go_up();

    // 返回根目录
    void go_home();

    // 下载文件
    void download_file();

    // 上传文件
    void upload_file();

    // 删除选中项
    void delete_selected();

    // 新建文件夹
    void create_folder();

    // 显示上下文菜单
    void show_context_menu(const QPoint& pos);

private:
    void setup_ui();
    QWidget* create_toolbar();
    void create_storage_selector();
    void create_file_browser();
    void create_empty_state();
    void create_status_bar();

    // 视图切换
    void show_empty_state();
    void show_config_panel();
    void show_browser_panel();

    // 更新文件列表
    void update_file_list(const QString& path);

    // 格式化文件大小
    QString format_size(uint64_t bytes) const;

    // 控件
    QSplitter* splitter_;
    QWidget* empty_state_widget_;  // 空状态视图
    QStackedWidget* stacked_widget_;  // 视图切换容器

    // 左侧面板 - 存储选择器
    QWidget* left_panel_;
    QComboBox* storage_type_combo_;
    QLineEdit* endpoint_edit_;
    QLineEdit* access_key_edit_;
    QLineEdit* secret_key_edit_;
    QLineEdit* region_edit_;
    QLineEdit* bucket_edit_;
    QPushButton* connect_button_;
    QPushButton* disconnect_button_;
    QPushButton* save_config_button_;

    // 右侧面板 - 文件浏览器
    QWidget* right_panel_;
    QLineEdit* current_path_edit_;
    QTableWidget* file_table_;

    // 工具栏按钮
    QPushButton* refresh_button_;
    QPushButton* up_button_;
    QPushButton* home_button_;
    QPushButton* download_button_;
    QPushButton* upload_button_;
    QPushButton* delete_button_;
    QPushButton* new_folder_button_;

    // 状态栏
    QLabel* status_label_;
    QLabel* connection_status_label_;

    // 当前状态
    CloudStorageConfig current_config_;
    QString current_path_;
    bool is_connected_ = false;

    // 已保存的配置列表
    QList<CloudStorageConfig> saved_configs_;
};

} // namespace falcon::desktop
