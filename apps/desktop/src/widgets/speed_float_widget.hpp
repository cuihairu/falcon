/**
 * @file speed_float_widget.hpp
 * @brief 悬浮速度窗(紧凑横条,始终置顶可拖动)
 *
 * B21/B22 重构:放弃 168×168 方形水位计画布自绘,改为 QWidget 布局的
 * 紧凑横条——文件名(省略) + 暂停/继续钮 / 任务进度条 + 百分比 /
 * 速度与任务数**分格**两格(布局约束下任何档位/翻译长度都不会叠字)。
 *
 * 右键菜单给常用项:暂停/继续、复制链接、打开所在目录、关闭浮窗
 * (关闭项发 close_requested,由宿主隐藏并持久化关闭状态)。
 *
 * 外观与行为由设置页"悬浮速度窗"节控制(开关/显示内容/大小/透明度/
 * 鼠标穿透);窗口位置由宿主(MainWindow)经 position_changed 信号持久化。
 * 颜色取自 theme_tokens 双主题表,随主题切换刷新。
 *
 * @author Falcon Team
 * @date 2026-10-04
 */

#pragma once

#include <QPoint>
#include <QWidget>

#include "utils/theme_manager.hpp"

class QLabel;
class QProgressBar;
class QToolButton;

namespace falcon::desktop {

class SpeedFloatWidget : public QWidget
{
    Q_OBJECT

public:
    /// 尺寸档位(设置页"大小"下拉,序号即枚举值;Small = 默认紧凑档)
    enum class SizePreset { Small = 0, Medium = 1, Large = 2 };

    /// 一次统计快照(MainWindow 从 GlobalStats + 任务快照装配)
    struct Stats {
        std::uint64_t download_speed = 0;   // 字节/秒
        int active_tasks = 0;               // 活跃任务数
        double overall_progress = -1.0;     // 0..1;<0 = 无活跃任务/未知
    };

    /// 当前预览任务(B21:第一条 Downloading,否则第一条 Paused;
    /// 宿主在任务快照回调里装配,无活跃/暂停任务时 has_task=false)
    struct TaskPreview {
        bool has_task = false;
        std::uint64_t task_id = 0;
        bool running = false;               // true=下载中(钮/菜单=暂停)
        QString url;                        // 复制链接用
        QString file_name;                  // 标题行(自动省略)
        QString directory;                  // 输出目录(打开所在目录用)
        double progress = 0.0;              // 0..1
    };

    explicit SpeedFloatWidget(QWidget* parent = nullptr);
    ~SpeedFloatWidget() override;

    /// 主题切换时刷新配色(tokens_for 全量取自当前主题)
    void apply_theme(ThemeType theme);

    /// 喂入新统计(数据由 MainWindow 在 stats 快照回调装配)
    void update_stats(const Stats& stats);

    /// 喂入当前预览任务(MainWindow 在任务快照回调装配)
    void update_task_preview(const TaskPreview& preview);

    void set_show_active_tasks(bool show);
    void set_show_total_progress(bool show);
    void set_size_preset(SizePreset preset);
    /// 30..100(%);经 setWindowOpacity 生效
    void set_opacity_percent(int percent);
    /// 鼠标穿透:开启后悬浮窗对点击完全透明(经设置页开关关闭)
    void set_click_through(bool enable);

    bool shows_active_tasks() const { return show_active_tasks_; }
    bool shows_total_progress() const { return show_total_progress_; }
    SizePreset size_preset() const { return size_preset_; }
    int opacity_percent() const { return opacity_percent_; }
    bool click_through() const { return click_through_; }

    /// 速度文本(与 StatusBar::format_speed 同风格 "1.5 MB/s")
    static QString format_speed(std::uint64_t bytes_per_second);

signals:
    /// 一次拖动结束(位置持久化由宿主负责)
    void position_changed(const QPoint& pos);

    /// 右键菜单/按钮请求切换任务运行态:pause=true 请求暂停,false 请求继续
    void pause_resume_requested(std::uint64_t task_id, bool pause);

    /// 右键菜单「关闭浮窗」(宿主负责隐藏 + 持久化关闭状态)
    void close_requested();

protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void setup_ui();
    void refresh_display();
    void refresh_name_elide();
    void restyle();
    QSize size_for_preset(SizePreset preset) const;

    ThemeType theme_ = ThemeType::Dark;
    Stats stats_;
    TaskPreview preview_;

    QLabel* name_label_ = nullptr;
    QToolButton* pause_button_ = nullptr;
    QProgressBar* progress_bar_ = nullptr;
    QLabel* percent_label_ = nullptr;
    QLabel* speed_label_ = nullptr;
    QLabel* task_count_label_ = nullptr;

    bool show_active_tasks_ = true;
    bool show_total_progress_ = true;
    SizePreset size_preset_ = SizePreset::Small;   // B21①: 默认紧凑档
    int opacity_percent_ = 90;
    bool click_through_ = false;
    bool dragging_ = false;
    QPoint drag_offset_;
};

} // namespace falcon::desktop
