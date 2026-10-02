/**
 * @file speed_float_widget.cpp
 * @brief 悬浮速度窗实现(水波纹水位计)
 *
 * 绘制三层:① 圆角卡片底(theme card 色);② 双层正弦波水体(theme accent
 * 色,半透明,副波相位错开做涌动层次);③ 文本(速度 + 可选活跃任务数/总进度)
 * ——速度文本按水面裁剪双重绘制:水下部分用 accent_text 色,水上部分用
 * text 色,任何水位下都可读。
 *
 * @author Falcon Team
 * @date 2026-10-02
 */

#include "widgets/speed_float_widget.hpp"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QShowEvent>
#include <QTimer>
#include <QWindow>

#include <cmath>

#include "utils/theme_tokens.hpp"

namespace falcon::desktop {

namespace {

// 满水位对应的速度(10MB/s 达满;sqrt 标度让低速段也有可感知的水位差)
constexpr std::uint64_t kFullScaleSpeed = 10ULL * 1024 * 1024;
// 速度为 0 时的静息水位(不贴底,保持"水位计"形态可辨)
constexpr double kIdleLevel = 0.06;
// 水位/文本区域留白(占短边比例)
constexpr double kWaterInsetRatio = 0.06;

} // namespace

SpeedFloatWidget::SpeedFloatWidget(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint)
{
    setAttribute(Qt::WA_TranslucentBackground);
    // 悬浮窗不是业务页面,不吃通用 QSS 背景
    setObjectName("speedFloat");

    setFixedSize(size_for_preset(size_preset_));
    setWindowOpacity(opacity_percent_ / 100.0);
    setCursor(Qt::SizeAllCursor);
    setToolTip(tr("拖动移动位置"));

    timer_ = new QTimer(this);
    timer_->setInterval(33); // ~30fps
    connect(timer_, &QTimer::timeout, this, [this]() {
        const double target = water_level_for_speed(stats_.download_speed);
        // 指数逼近目标水位,速度抖动被平滑成水面涨落
        display_level_ += (target - display_level_) * 0.12;
        // 相位推进速率 ∝ 速度:速度快时波涌更急
        phase_ += 0.05 + 0.22 * target;
        update();
    });
}

SpeedFloatWidget::~SpeedFloatWidget() = default;

double SpeedFloatWidget::water_level_for_speed(std::uint64_t bytes_per_second)
{
    if (bytes_per_second == 0) {
        return kIdleLevel;
    }
    const double t = std::sqrt(static_cast<double>(bytes_per_second)
                               / static_cast<double>(kFullScaleSpeed));
    return kIdleLevel + (1.0 - kIdleLevel) * qBound(0.0, t, 1.0);
}

QString SpeedFloatWidget::format_speed(std::uint64_t bytes_per_second)
{
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    int unit = 0;
    double speed = static_cast<double>(bytes_per_second);
    while (speed >= 1024.0 && unit < 3) {
        speed /= 1024.0;
        ++unit;
    }
    return QString("%1 %2").arg(speed, 0, 'f', 1).arg(QLatin1String(units[unit]));
}

void SpeedFloatWidget::apply_theme(ThemeType theme)
{
    theme_ = theme;
    update();
}

void SpeedFloatWidget::update_stats(const Stats& stats)
{
    stats_ = stats;
    // 水位/相位在定时器里平滑推进;文本即时刷新
}

void SpeedFloatWidget::set_show_active_tasks(bool show)
{
    show_active_tasks_ = show;
    update();
}

void SpeedFloatWidget::set_show_total_progress(bool show)
{
    show_total_progress_ = show;
    update();
}

QSize SpeedFloatWidget::size_for_preset(SizePreset preset) const
{
    switch (preset) {
    case SizePreset::Small:
        return {132, 132};
    case SizePreset::Large:
        return {208, 208};
    case SizePreset::Medium:
    default:
        return {168, 168};
    }
}

void SpeedFloatWidget::set_size_preset(SizePreset preset)
{
    if (preset == size_preset_) {
        return;
    }
    size_preset_ = preset;
    setFixedSize(size_for_preset(preset));
    update();
}

void SpeedFloatWidget::set_opacity_percent(int percent)
{
    opacity_percent_ = qBound(30, percent, 100);
    setWindowOpacity(opacity_percent_ / 100.0);
}

void SpeedFloatWidget::set_click_through(bool enable)
{
    click_through_ = enable;
    // windowHandle 在 show 之后才有;showEvent 兜底 reapplies
    if (windowHandle()) {
        windowHandle()->setFlag(Qt::WindowTransparentForInput, enable);
    }
    setCursor(enable ? Qt::ArrowCursor : Qt::SizeAllCursor);
}

void SpeedFloatWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (windowHandle()) {
        windowHandle()->setFlag(Qt::WindowTransparentForInput, click_through_);
    }
    if (!timer_->isActive()) {
        timer_->start();
    }
}

void SpeedFloatWidget::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        dragging_ = true;
        drag_offset_ = event->globalPosition().toPoint() - pos();
    }
    QWidget::mousePressEvent(event);
}

void SpeedFloatWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (dragging_) {
        move(event->globalPosition().toPoint() - drag_offset_);
    }
    QWidget::mouseMoveEvent(event);
}

void SpeedFloatWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (dragging_ && event->button() == Qt::LeftButton) {
        dragging_ = false;
        emit position_changed(pos());
    }
    QWidget::mouseReleaseEvent(event);
}

void SpeedFloatWidget::paintEvent(QPaintEvent* /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const ThemeTokens tokens = tokens_for(theme_);
    const int w = width();
    const int h = height();
    const double radius = w / 5.0;

    // ① 卡片底(半透明圆角矩形)
    QRectF card_rect(0.5, 0.5, w - 1.0, h - 1.0);
    QPainterPath card_path;
    card_path.addRoundedRect(card_rect, radius, radius);

    QColor card = tokens.card;
    card.setAlpha(232);
    painter.fillPath(card_path, card);

    // 描边(divider 色勾边,弱化贴边感)
    QColor edge = tokens.divider;
    edge.setAlpha(160);
    painter.setPen(QPen(edge, 1.0));
    painter.drawPath(card_path);

    // ② 水体:水位 = 静息..满 之间;副波相位错开、振幅略低,叠出涌动层次
    const double inset = w * kWaterInsetRatio;
    const double water_top_max = inset;                 // 满水位时波峰可达处
    const double water_bottom = h - inset;              // 静息水位线
    const double amplitude = 2.0 + 9.0 * display_level_;
    const double level_y = water_bottom
        - display_level_ * (water_bottom - water_top_max);

    auto wave_y = [&](double x, double freq, double ph, double amp) {
        return level_y + amp * std::sin(x * freq + ph);
    };

    painter.save();
    painter.setClipPath(card_path);

    QColor water_main = tokens.accent;
    water_main.setAlpha(150);
    QColor water_sub = tokens.accent;
    water_sub.setAlpha(80);

    // 副波(水位略高、相位滞后,先画,做层次)
    QPainterPath sub_path;
    sub_path.moveTo(0, h);
    for (double x = 0; x <= w; x += 2.0) {
        sub_path.lineTo(x, wave_y(x, 0.045, -phase_ * 0.7 + 1.7, amplitude * 0.6));
    }
    sub_path.lineTo(w, h);
    sub_path.closeSubpath();
    painter.fillPath(sub_path, water_sub);

    // 主波
    QPainterPath water_path;
    water_path.moveTo(0, h);
    for (double x = 0; x <= w; x += 2.0) {
        water_path.lineTo(x, wave_y(x, 0.06, phase_, amplitude));
    }
    water_path.lineTo(w, h);
    water_path.closeSubpath();
    painter.fillPath(water_path, water_main);

    painter.restore();

    // ③ 文本:速度为主行,活跃任务数/总进度为副行
    const QString speed_text = format_speed(stats_.download_speed);
    QString sub_text;
    if (show_active_tasks_ && show_total_progress_) {
        sub_text = stats_.overall_progress >= 0.0
            ? tr("%1 个任务 · %2%")
                  .arg(stats_.active_tasks)
                  .arg(qRound(stats_.overall_progress * 100.0))
            : tr("%1 个任务").arg(stats_.active_tasks);
    } else if (show_active_tasks_) {
        sub_text = tr("%1 个任务").arg(stats_.active_tasks);
    } else if (show_total_progress_ && stats_.overall_progress >= 0.0) {
        sub_text = tr("%1%").arg(qRound(stats_.overall_progress * 100.0));
    }

    QFont speed_font = font();
    speed_font.setBold(true);
    speed_font.setPixelSize(qMax(14, static_cast<int>(w * 0.155)));
    QFont sub_font = font();
    sub_font.setPixelSize(qMax(10, static_cast<int>(w * 0.095)));

    QFontMetrics speed_fm(speed_font);
    const int line_gap = 4;
    const int sub_h = sub_text.isEmpty() ? 0 : QFontMetrics(sub_font).height();
    const int text_block_h = speed_fm.height() + line_gap + sub_h;
    double y = (h - text_block_h) / 2.0 + speed_fm.ascent();

    painter.setFont(speed_font);
    const QPointF speed_pos((w - speed_fm.horizontalAdvance(speed_text)) / 2.0, y);

    // 双通道绘制:水上用干色(text/text_secondary),水下用 accent_text
    // ——水面淹没文字的任何位置都保持可读
    const auto draw_duotone = [&](const QString& text, const QPointF& pos,
                                   const QColor& dry_color) {
        painter.save();
        painter.setClipPath(card_path);
        painter.setPen(dry_color);
        painter.drawText(pos, text);
        painter.restore();
        painter.save();
        painter.setClipPath(water_path, Qt::IntersectClip);
        painter.setPen(tokens.accent_text);
        painter.drawText(pos, text);
        painter.restore();
    };

    draw_duotone(speed_text, speed_pos, tokens.text);

    if (!sub_text.isEmpty()) {
        y += speed_fm.descent() + line_gap;
        painter.setFont(sub_font);
        const QFontMetrics sub_fm(sub_font);
        draw_duotone(sub_text,
                     QPointF((w - sub_fm.horizontalAdvance(sub_text)) / 2.0, y),
                     tokens.text_secondary);
    }
}

} // namespace falcon::desktop
