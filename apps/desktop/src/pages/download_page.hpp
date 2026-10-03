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
#include <QStackedWidget>
#include <QFrame>

#include <rpc/aria2_snapshots.hpp>

#include <vector>

namespace falcon::desktop {

class TaskTableWidget;

/**
 * @brief 当前视图模式
 */
enum class DownloadViewMode {
    Downloading,   // 下载中
    Completed      // 已完成
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
 * 只消费 TaskSnapshot（DownloadService 每 500ms 推送），不直接持有引擎
 * 或任务对象；暂停/继续等操作经信号交由 MainWindow 转发到下载服务。
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

    /// 全量替换任务快照并刷新显示（DownloadService 每个轮询周期调用）
    void update_tasks(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks);
    void set_view_mode(DownloadViewMode mode);
    void set_display_style(TaskDisplayStyle style);
    /// 顶栏搜索框回车 → 按文件名过滤当前视图（空串清除过滤）
    void set_text_filter(const QString& text);
    /// 顶栏视图切换 → 表格/网格互切（与页内按钮共用同一状态）
    void toggle_display_style();
    /// 当前显示样式（表格/卡片）；默认卡片（首次/无记录），供持久化与设置页同步
    TaskDisplayStyle display_style() const { return display_style_; }
    /// 恢复持久化的手动排序（启动时从 QSettings 装载；表格/网格共用，
    /// 空序 = 从未拖拽，保持 id 升序默认行为）
    void set_task_order(const std::vector<falcon::TaskId>& order);

signals:
    void new_task_requested();
    void remove_task_requested(falcon::TaskId id);
    void remove_finished_tasks_requested();
    void priority_changed(falcon::TaskId id, falcon::TaskPriority priority);
    void pause_requested(falcon::TaskId id);
    void resume_requested(falcon::TaskId id);
    /// 停止做种（BitTorrent；仅对 seeding_active 任务出现菜单项）
    void stop_seeding_requested(falcon::TaskId id);
    /// 拖拽排序变化 → 序列化串（"3,1,2"），MainWindow 落 QSettings
    void task_order_changed(const QString& serialized);
    /// 显示样式实际变化 → MainWindow 落 QSettings（手动切换记忆）并同步设置页
    /// @param grid_view true=卡片视图，false=列表视图
    void display_style_changed(bool grid_view);

private slots:
    void on_new_task_clicked();
    void on_refresh_clicked();
    void on_style_toggle_clicked();  // 切换表格/网格视图
    void on_more_options_clicked();
    void on_pause_selected();
    void on_resume_selected();
    void on_delete_selected();

private:
    // 任务显示记录：快照 + 预格式化的文本列
    struct TaskRecord {
        falcon::daemon::rpc::TaskSnapshot snapshot;
        QString filename;
        QString save_path;
        QString size_text;
        QString status_text;
        QString error_text;
    };

    void setup_ui();
    void create_header_bar();
    void create_hero_section();
    void create_summary_cards();
    void create_empty_state();
    void create_task_table();
    void create_task_grid();  // 创建网格视图
    void update_header_for_mode();
    void update_summary_cards();
    void update_empty_state();
    void show_context_menu(const QPoint& pos);
    void show_grid_context_menu(const QPoint& pos);  // 网格视图右键菜单
    void rerender();             // 从当前 task_records_ 全量刷新显示
    void sync_task_grid();       // 同步网格内容
    void sync_task_row(const TaskRecord& record);   // 按视图过滤增/删行
    void update_row_texts(int row, const TaskRecord& record);
    QWidget* create_task_card(const TaskRecord& record);  // 创建任务卡片
    void rebuild_display_positions();  // 由 task_order_+task_records_ 重建排序位置表
    void rebuild_table_rows();         // 清空表格行并按当前排序全量重插
    // 表格拖拽落定：可见子集换位 → 全局 order 重建 → 重排 → 持久化信号
    void on_table_reorder(int from_row, int to_row);
    // 拖拽会话开始/结束（结束时补刷会话期间挂起的快照）
    void on_drag_session(bool active);

    const TaskRecord* record_by_id(qulonglong key) const;
    const TaskRecord* record_from_sender() const;
    const TaskRecord* selected_record() const;
    const TaskRecord* record_at_row(int row) const;
    void remove_task_row(qulonglong key);
    bool should_show(const falcon::daemon::rpc::TaskSnapshot& snapshot) const;

    static QString filename_for(const falcon::daemon::rpc::TaskSnapshot& snapshot);
    static QString format_bytes(uint64_t bytes);
    static QString format_speed(uint64_t bytes_per_second);
    static QString seeding_text(const falcon::daemon::rpc::TaskSnapshot& snapshot);
    static QString seed_ratio_text(const falcon::daemon::rpc::TaskSnapshot& snapshot);
    static QString seed_duration_text(const falcon::daemon::rpc::TaskSnapshot& snapshot);
    // 做种列汇总("做种中 · 1.20 · 2时15分";无做种信息为空串)与明细 tooltip
    static QString seed_column_text(const falcon::daemon::rpc::TaskSnapshot& snapshot);
    static QString seed_column_tooltip(const falcon::daemon::rpc::TaskSnapshot& snapshot);
    // 活动任务才显示速度,暂停/终态显示 "—"(0 B/s 是无信息量噪音)
    static QString speed_display_text(const falcon::daemon::rpc::TaskSnapshot& snapshot);

    // 当前视图模式
    DownloadViewMode view_mode_;

    // 头部区域
    QHBoxLayout* header_layout_;
    QLabel* status_label_;
    QLabel* hero_title_label_ = nullptr;
    QLabel* hero_description_label_ = nullptr;
    QPushButton* new_task_button_;
    QPushButton* refresh_button_;
    QPushButton* style_toggle_button_;  // 切换表格/网格视图
    QPushButton* more_button_;
    // 页头分段切换器(下载中|已完成),与侧栏 tab 双向同步
    QWidget* view_segmented_ = nullptr;
    QPushButton* view_segment_downloading_ = nullptr;
    QPushButton* view_segment_completed_ = nullptr;
    QLabel* active_summary_value_ = nullptr;
    QLabel* completed_summary_value_ = nullptr;
    QLabel* speed_summary_value_ = nullptr;
    QWidget* empty_state_widget_ = nullptr;
    QLabel* empty_state_title_ = nullptr;
    QLabel* empty_state_body_ = nullptr;

    // 内容区三页栈（空态/表格/网格互斥页）：三态恒占同一 stretch 区域，
    // 有无任务页面占用体积一致——空态直挂布局时自然高度 vs 表格
    // stretch=1 会随任务出现/清空跳动
    QStackedWidget* content_stack_ = nullptr;
    QWidget* empty_page_ = nullptr;  // 空态页（卡片垂直居中）

    // 任务表格（支持行拖拽排序）
    TaskTableWidget* task_table_;

    // 任务数据（DownloadService 推送的快照副本）
    QHash<qulonglong, int> row_by_task_id_;
    QHash<qulonglong, TaskRecord> task_records_;

    // 手动排序（拖拽持久化）：任务 id 序列；显示排序键 = (order 位置, id)，
    // 空序退化为 id 升序（算法见 services/task_order.hpp）
    std::vector<falcon::TaskId> task_order_;
    // id → 显示排序位置（rerender 前由 rebuild_display_positions 重建）
    QHash<qulonglong, int> display_pos_;

    // 拖拽会话期间挂起 500ms 快照刷新（行被重建会使落点行号失真）
    bool drag_active_ = false;
    bool has_pending_tasks_ = false;
    std::vector<falcon::daemon::rpc::TaskSnapshot> pending_tasks_;

    // 显示样式
    TaskDisplayStyle display_style_;

    // 文件名过滤词（顶栏搜索框，空串 = 不过滤）
    QString text_filter_;

    // 网格视图组件
    QWidget* grid_container_;      // 网格视图容器
    QScrollArea* grid_scroll_area_;  // 网格滚动区域
    QWidget* grid_widget_;         // 网格内容 widget
    QGridLayout* grid_layout_;     // 网格布局
};

} // namespace falcon::desktop
