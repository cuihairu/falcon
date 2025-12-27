/**
 * @file discovery_page.hpp
 * @brief 资源发现与搜索页面
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QComboBox>
#include <QLabel>
#include <QTabWidget>

namespace falcon::desktop {

/**
 * @brief 搜索结果项
 */
struct SearchResultItem {
    QString title;          // 标题
    QString url;            // 下载链接
    QString size;           // 文件大小
    QString source;         // 来源
    QString type;           // 文件类型
    QString date;           // 发布日期
    int seeders;            // 种子数（BT专用）
    int leechers;           // 下载数（BT专用）
};

/**
 * @brief 发现页面 - 资源搜索与发现
 *
 * 支持多种资源搜索：
 * - 磁力链接搜索
 * - HTTP/HTTPS 资源搜索
 * - 网盘资源搜索
 * - FTP 资源搜索
 */
class DiscoveryPage : public QWidget
{
    Q_OBJECT

public:
    explicit DiscoveryPage(QWidget* parent = nullptr);
    ~DiscoveryPage() override;

private slots:
    // 执行搜索
    void perform_search();

    // 清空搜索
    void clear_search();

    // 下载选中项
    void download_selected();

    // 复制链接
    void copy_link();

    // 打开链接
    void open_link();

    // 搜索类型改变
    void on_search_type_changed(int index);

    // 显示结果详情
    void show_item_details(int row);

    // 显示上下文菜单
    void show_context_menu(const QPoint& pos);

    // 添加到下载队列
    void add_to_download_queue();

private:
    void setup_ui();
    QWidget* create_search_bar();
    QWidget* create_filter_bar();
    void create_results_table();
    QWidget* create_status_bar();

    // 执行不同类型的搜索
    void search_magnet_links(const QString& keyword);
    void search_http_resources(const QString& keyword);
    void search_cloud_resources(const QString& keyword);
    void search_ftp_resources(const QString& keyword);

    // 解析并显示搜索结果
    void display_results(const QList<SearchResultItem>& results);

    // 格式化数字显示
    QString format_number(int num) const;

    // 控件
    QWidget* search_bar_;
    QWidget* filter_bar_;

    // 搜索栏
    QLineEdit* search_input_;
    QPushButton* search_button_;
    QPushButton* clear_button_;
    QComboBox* search_type_combo_;
    QComboBox* sort_combo_;

    // 过滤栏
    QComboBox* category_filter_;
    QComboBox* size_filter_;
    QLineEdit* min_size_edit_;
    QLineEdit* max_size_edit_;

    // 结果表格
    QTableWidget* results_table_;

    // 状态栏
    QLabel* status_label_;
    QLabel* result_count_label_;

    // 当前搜索结果
    QList<SearchResultItem> current_results_;

    // 搜索设置
    struct SearchSettings {
        QString search_type = "magnet";  // magnet, http, cloud, ftp
        QString category = "all";        // all, video, audio, document, software
        QString sort_by = "relevance";   // relevance, size, date, seeders
        int max_results = 50;
    } settings_;
};

} // namespace falcon::desktop
