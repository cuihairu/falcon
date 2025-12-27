/**
 * @file download_page.hpp
 * @brief 下载管理页面
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <QWidget>
#include <QTabWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QLabel>

namespace falcon::desktop {

/**
 * @brief 下载任务表格项
 */
struct DownloadTaskItem {
    QString filename;
    QString url;
    qint64 total_size;
    qint64 downloaded_size;
    double speed;          // bytes/s
    QString status;        // 下载中、暂停、完成、失败
    QString save_path;
    int progress;          // 0-100
};

/**
 * @brief 下载管理页面
 *
 * 包含：正在下载、已完成、回收站、下载记录四个标签页
 */
class DownloadPage : public QWidget
{
    Q_OBJECT

public:
    explicit DownloadPage(QWidget* parent = nullptr);
    ~DownloadPage() override;

private:
    void setup_ui();
    QWidget* create_tab_widget();
    void create_downloading_tab();
    void create_completed_tab();
    void create_trash_tab();
    void create_history_tab();
    QWidget* create_toolbar();

    // 添加新任务
    void add_new_task();

    // 更新任务状态
    void update_task_status();

    // 控件
    QTabWidget* tab_widget_;

    // 正在下载表格
    QTableWidget* downloading_table_;

    // 已完成表格
    QTableWidget* completed_table_;

    // 回收站表格
    QTableWidget* trash_table_;

    // 下载记录表格
    QTableWidget* history_table_;

    // 工具栏按钮
    QPushButton* new_task_button_;
    QPushButton* pause_button_;
    QPushButton* resume_button_;
    QPushButton* cancel_button_;
    QPushButton* delete_button_;
    QPushButton* clean_button_;
};

} // namespace falcon::desktop
