/**
 * @file speed_float_widget.hpp
 * @brief 悬浮速度窗(水波纹水位计风格,始终置顶可拖动)
 *
 * 常显当前全局下载速度;框内以双层正弦波做"水位计"填充动画——
 * 水位高度与波纹涌动幅度/相位速度均随当前速度变化(速度越快水位越高、
 * 波浪越大越急)。颜色全部取自 theme_tokens 双主题表,随主题切换刷新。
 *
 * 外观与行为由设置页"悬浮速度窗"节控制(开关/显示内容/大小/透明度/
 * 鼠标穿透);窗口位置由宿主(MainWindow)经 position_changed 信号持久化。
 *
 * @author Falcon Team
 * @date 2026-10-02
 */

#pragma once

#include <QPoint>
#include <QWidget>

#include "utils/theme_manager.hpp"

class QTimer;

namespace falcon::desktop {

class SpeedFloatWidget : public QWidget
{
    Q_OBJECT

public:
    /// 尺寸档位(设置页"大小"下拉,序号即枚举值)
    enum class SizePreset { Small = 0, Medium = 1, Large = 2 };

    /// 一次统计快照(MainWindow 从 GlobalStats + 任务快照装配)
    struct Stats {
        std::uint64_t download_speed = 0;   // 字节/秒
        int active_tasks = 0;               // 活跃任务数
        double overall_progress = -1.0;     // 0..1;<0 = 无活跃任务/未知
    };

    explicit SpeedFloatWidget(QWidget* parent = nullptr);
    ~SpeedFloatWidget() override;

    /// 主题切换时刷新配色(tokens_for 全量取自当前主题)
    void apply_theme(ThemeType theme);

    /// 喂入新统计(数据由 MainWindow 在 stats/tasks 快照回调装配)
    void update_stats(const Stats& stats);

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

    /// 速度 → 目标水位(0..1)映射:10MB/s 满水位,sqrt 标度,底部留 0.06 静息水位
    static double water_level_for_speed(std::uint64_t bytes_per_second);
    /// 速度文本(与 StatusBar::format_speed 同风格 "1.5 MB/s")
    static QString format_speed(std::uint64_t bytes_per_second);

signals:
    /// 一次拖动结束(位置持久化由宿主负责)
    void position_changed(const QPoint& pos);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    QSize size_for_preset(SizePreset preset) const;

    ThemeType theme_ = ThemeType::Dark;
    Stats stats_;
    double display_level_ = 0.0;   // 平滑逼近目标水位(防速度抖动跳变)
    double phase_ = 0.0;           // 波纹相位,推进速率 ∝ 速度
    QTimer* timer_ = nullptr;

    bool show_active_tasks_ = true;
    bool show_total_progress_ = true;
    SizePreset size_preset_ = SizePreset::Medium;
    int opacity_percent_ = 90;
    bool click_through_ = false;
    bool dragging_ = false;
    QPoint drag_offset_;
};

} // namespace falcon::desktop
