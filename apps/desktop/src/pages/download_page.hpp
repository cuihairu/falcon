/**
 * @file download_page.hpp
 * @brief 下载管理页面（迅雷风格）
 * @author Falcon Team
 * @date 2026-04-15
 */

#pragma once

#include <QWidget>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QScrollArea>

#include <falcon/download_task.hpp>

namespace falcon::desktop {

/**
 * @brief 当前视图模式
 */
enum class DownloadViewMode {
    Downloading,   // 下载中
    Completed,     // 已完成
    CloudAdd       // 云添加
};

/**
 * @brief 任务显示样式
 */
enum class TaskDisplayStyle {
    Table,         // 表格视图
    Grid           // 网格/卡片视图
};

/**
 * @brief 下载管理页面（迅雷风格）
 *
 * 布局：
 * - 顶部：状态标题 + 操作按钮 + 新建按钮
 * - 中间：任务列表表格
 * - 底部：推广区域（预留）
 */
class DownloadPage : public QWidget
{
    Q_OBJECT

public:
    explicit DownloadPage(QWidget* parent = nullptr);
    ~DownloadPage() override;

    void add_engine_task(const falcon::DownloadTask::Ptr& task);
    void set_view_mode(DownloadViewMode mode);
    void set_display_style(TaskDisplayStyle style);

signals:
    void new_task_requested();
    void remove_task_requested(falcon::TaskId id);
    void remove_finished_tasks_requested();

private slots:
    void on_new_task_clicked();
    void on_refresh_clicked();
    void on_view_toggle_clicked();
    void on_style_toggle_clicked();  // 切换表格/网格视图
    void on_more_options_clicked();
    void on_pause_selected();
    void on_resume_selected();
    void on_delete_selected();

private:
    // TaskRecord structure (must be defined before methods that use it)
    struct TaskRecord {
        falcon::DownloadTask::Ptr task;
        QString filename;
        QString save_path;
        QString size_text;
        QString status_text;
        QString error_text;
        bool in_trash = false;
    };

    void setup_ui();
    void create_header_bar();
    void create_task_table();
    void create_task_grid();  // 创建网格视图
    void update_header_for_mode();
    void update_action_buttons();
    void show_context_menu(const QPoint& pos);
    void show_grid_context_menu(const QPoint& pos);  // 网格视图右键菜单
    void refresh_display();  // 刷新当前显示
    void sync_task_grid();   // 同步网格内容
    QWidget* create_task_card(const TaskRecord& record);  // 创建任务卡片
    falcon::DownloadTask::Ptr selected_task() const;
    falcon::DownloadTask::Ptr task_at_row(int row) const;

    void refresh_engine_tasks();
    void sync_task_tables(const falcon::DownloadTask::Ptr& task);

    static QString format_bytes(uint64_t bytes);
    static QString format_speed(uint64_t bytes_per_second);

    // 当前视图模式
    DownloadViewMode view_mode_;

    // 头部区域
    QHBoxLayout* header_layout_;
    QLabel* status_label_;
    QPushButton* new_task_button_;
    QPushButton* refresh_button_;
    QPushButton* view_toggle_button_;
    QPushButton* style_toggle_button_;  // 切换表格/网格视图
    QPushButton* more_button_;

    // 任务表格
    QTableWidget* task_table_;

    // 操作按钮
    QPushButton* pause_button_;
    QPushButton* delete_button_;

    // 刷新定时器
    QTimer* refresh_timer_;

    // 任务数据
    QHash<qulonglong, int> row_by_task_id_;
    QHash<qulonglong, TaskRecord> task_records_;

    // 显示样式
    TaskDisplayStyle display_style_;

    // 网格视图组件
    QWidget* grid_container_;      // 网格视图容器
    QScrollArea* grid_scroll_area_;  // 网格滚动区域
    QWidget* grid_widget_;         // 网格内容 widget
    QGridLayout* grid_layout_;     // 网格布局
};

} // namespace falcon::desktop
