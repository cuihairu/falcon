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
#include <QTimer>
#include <QDateTime>

#include <falcon/download_task.hpp>

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

    void add_engine_task(const falcon::DownloadTask::Ptr& task);

signals:
    void new_task_requested();
    void remove_task_requested(falcon::TaskId id);
    void remove_finished_tasks_requested();

private:
    void setup_ui();
    void create_tab_widget();
    void create_downloading_tab();
    void create_completed_tab();
    void create_trash_tab();
    void create_history_tab();
    QWidget* create_toolbar();

    void pause_selected_task();
    void resume_selected_task();
    void cancel_selected_task();
    void delete_selected_task();
    void clear_finished_tasks();
    void update_action_buttons();
    falcon::DownloadTask::Ptr selected_task() const;
    falcon::TaskId selected_archived_task_id(QTableWidget* table) const;
    void remove_downloading_row(falcon::TaskId id);
    void append_trash_entry(falcon::TaskId id, const QString& reason);
    void upsert_completed_row(falcon::TaskId id);
    void upsert_history_row(falcon::TaskId id);
    void remove_completed_row(falcon::TaskId id);
    void remove_tracked_task(falcon::TaskId id);
    void sync_task_tables(const falcon::DownloadTask::Ptr& task);

    void refresh_engine_tasks();
    static QString format_bytes(uint64_t bytes);
    static QString format_speed(uint64_t bytes_per_second);
    static QString format_timestamp(const QDateTime& timestamp);

    struct TaskRecord {
        falcon::DownloadTask::Ptr task;
        QString filename;
        QString save_path;
        QString size_text;
        QString status_text;
        QString error_text;
        QDateTime added_at;
        QDateTime finished_at;
        bool in_trash = false;
    };

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

    QTimer* refresh_timer_;
    QHash<qulonglong, int> row_by_task_id_;
    QHash<qulonglong, int> completed_row_by_task_id_;
    QHash<qulonglong, int> history_row_by_task_id_;
    QHash<qulonglong, int> trash_row_by_task_id_;
    QHash<qulonglong, TaskRecord> task_records_;
};

} // namespace falcon::desktop
