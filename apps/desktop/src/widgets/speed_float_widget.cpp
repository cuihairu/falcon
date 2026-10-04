/**
 * @file speed_float_widget.cpp
 * @brief 悬浮速度窗实现(紧凑横条,布局约束替代画布手算)
 *
 * B22 根因修复:旧实现全部文本由 QPainter 按基线手算定位(速度基线 +
 * descent + 4px gap 排副行,漏算副行字体 ascent → 副行文字上浮 ~13px
 * 与速度行叠字),字体度量/翻译文案变长时无布局约束必然挤压。新实现
 * 速度与任务数放进 QHBoxLayout 两格、进度条独立一行——QLayout 的
 * 单元格互斥占据,结构性消灭叠字;任何档位宽度下都不重叠。
 *
 * @author Falcon Team
 * @date 2026-10-04
 */

#include "widgets/speed_float_widget.hpp"

#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QProgressBar>
#include <QShowEvent>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include "utils/theme_tokens.hpp"

namespace falcon::desktop {

namespace {

// 三档宽度(高度由布局按内容决定,紧凑横条)
constexpr int kPresetWidths[] = {260, 320, 380};
constexpr int kLabelSpacing = 12;

QString elide_for_width(const QString& text, const QFontMetrics& fm, int width)
{
    if (text.isEmpty()) {
        return text;
    }
    return fm.elidedText(text, Qt::ElideMiddle, qMax(16, width));
}

} // namespace

SpeedFloatWidget::SpeedFloatWidget(QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint
                          | Qt::WindowStaysOnTopHint)
{
    // 不设 WA_TranslucentBackground:实测(Xvfb + 直取窗口像素)局部 QSS
    // 背景在该属性下不绘制——窗口除子控件外全透明,悬浮窗变成无卡片
    // 底板的浮字;本窗卡片底色本就不透明(theme_tokens card),取消该
    // 属性后背景恒绘制、圆角由 QSS border 描出,任何合成器有无的桌面
    // 都可读。窗内容深浅随主题用 restyle() 局部样式表,不进全局 QSS
    setObjectName("speedFloat");

    setWindowOpacity(opacity_percent_ / 100.0);
    setCursor(Qt::SizeAllCursor);
    setToolTip(tr("拖动移动位置 · 右键更多操作"));

    setup_ui();
    set_size_preset(size_preset_);
    restyle();
}

SpeedFloatWidget::~SpeedFloatWidget() = default;

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

void SpeedFloatWidget::setup_ui()
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 8, 12, 9);
    root->setSpacing(5);

    // 行 1:文件名(省略) + 暂停/继续钮
    auto* top_row = new QHBoxLayout();
    top_row->setSpacing(8);
    name_label_ = new QLabel(this);
    name_label_->setObjectName("floatFileName");
    name_label_->setTextInteractionFlags(Qt::NoTextInteraction);
    top_row->addWidget(name_label_, /*stretch=*/1);

    pause_button_ = new QToolButton(this);
    pause_button_->setObjectName("floatPauseButton");
    pause_button_->setAutoRaise(true);
    pause_button_->setEnabled(false);
    connect(pause_button_, &QToolButton::clicked, this, [this]() {
        if (preview_.has_task) {
            emit pause_resume_requested(preview_.task_id, preview_.running);
        }
    });
    top_row->addWidget(pause_button_);
    root->addLayout(top_row);

    // 行 2:进度条 + 百分比
    auto* progress_row = new QHBoxLayout();
    progress_row->setSpacing(8);
    progress_bar_ = new QProgressBar(this);
    progress_bar_->setObjectName("floatProgressBar");
    progress_bar_->setRange(0, 100);
    progress_bar_->setTextVisible(false);
    progress_row->addWidget(progress_bar_, /*stretch=*/1);

    percent_label_ = new QLabel(this);
    percent_label_->setObjectName("floatPercentLabel");
    percent_label_->setFixedWidth(36);
    percent_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    progress_row->addWidget(percent_label_);
    root->addLayout(progress_row);

    // 行 3:速度 | 任务数 —— 两个独立单元格(B22 分格,永不叠字)
    auto* meta_row = new QHBoxLayout();
    meta_row->setSpacing(kLabelSpacing);
    speed_label_ = new QLabel(this);
    speed_label_->setObjectName("floatMetaLabel");
    meta_row->addWidget(speed_label_);

    meta_row->addStretch();

    task_count_label_ = new QLabel(this);
    task_count_label_->setObjectName("floatMetaLabel");
    meta_row->addWidget(task_count_label_);
    root->addLayout(meta_row);
}

void SpeedFloatWidget::apply_theme(ThemeType theme)
{
    theme_ = theme;
    restyle();
}

void SpeedFloatWidget::update_stats(const Stats& stats)
{
    stats_ = stats;
    refresh_display();
}

void SpeedFloatWidget::update_task_preview(const TaskPreview& preview)
{
    preview_ = preview;
    refresh_display();
}

void SpeedFloatWidget::set_show_active_tasks(bool show)
{
    show_active_tasks_ = show;
    refresh_display();
}

void SpeedFloatWidget::set_show_total_progress(bool show)
{
    show_total_progress_ = show;
    refresh_display();
}

QSize SpeedFloatWidget::size_for_preset(SizePreset preset) const
{
    // 只定宽;高度由布局按内容决定(紧凑横条,构造期 height() 尚无意义)
    const int index = qBound(0, static_cast<int>(preset), 2);
    return {kPresetWidths[index], qMax(height(), 74)};
}

void SpeedFloatWidget::set_size_preset(SizePreset preset)
{
    size_preset_ = preset;
    const QSize size = size_for_preset(preset);
    setFixedWidth(size.width());
    adjustSize(); // 高度交还布局(内容驱动,改档位只变宽度)
    refresh_name_elide();
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

void SpeedFloatWidget::refresh_display()
{
    // 标题行:预览任务文件名(省略),无任务显示占位
    if (preview_.has_task) {
        name_label_->setToolTip(preview_.file_name);
        pause_button_->setEnabled(true);
        pause_button_->setText(preview_.running ? tr("暂停") : tr("继续"));
        pause_button_->setToolTip(preview_.running ? tr("暂停该任务")
                                                   : tr("继续该任务"));
    } else {
        name_label_->setToolTip(QString());
        pause_button_->setEnabled(false);
        pause_button_->setText(tr("暂停"));
        pause_button_->setToolTip(tr("暂无活动任务"));
    }
    refresh_name_elide();

    // 进度行:优先预览任务进度,无任务回落全局总进度
    double progress = preview_.has_task
        ? preview_.progress
        : stats_.overall_progress;
    const bool has_progress = preview_.has_task || progress >= 0.0;
    progress_bar_->setVisible(show_total_progress_ && has_progress);
    percent_label_->setVisible(show_total_progress_ && has_progress);
    if (has_progress) {
        const int pct = qRound(qBound(0.0, progress, 1.0) * 100.0);
        progress_bar_->setValue(pct);
        percent_label_->setText(QString("%1%").arg(pct));
    }

    // 元信息行:速度 | 任务数(两格,各自独立布局约束)
    speed_label_->setText(tr("⬇ %1").arg(format_speed(stats_.download_speed)));
    task_count_label_->setText(tr("%1 个任务").arg(stats_.active_tasks));
    task_count_label_->setVisible(show_active_tasks_);
}

void SpeedFloatWidget::refresh_name_elide()
{
    const QString text = preview_.has_task
        ? preview_.file_name
        : tr("暂无下载任务");
    // 预留右侧按钮宽度(按钮文本 暂停/继续 同字号)
    const int reserved = pause_button_->sizeHint().width() + 8;
    const QFontMetrics fm(name_label_->font());
    name_label_->setText(elide_for_width(
        text, fm, qMax(24, name_label_->width() - reserved)));
}

void SpeedFloatWidget::restyle()
{
    const ThemeTokens t = tokens_for(theme_);
    // 局部样式表(悬浮窗不进全局 QSS);色值全部取自双主题 token 表
    setStyleSheet(QStringLiteral(
        "#speedFloat { background: %1; border: 1px solid %2;"
        "  border-radius: 10px; }"
        "#floatFileName { color: %3; font-size: 12px; font-weight: 600;"
        "  background: transparent; border: none; }"
        "#floatMetaLabel { color: %4; font-size: 11px;"
        "  background: transparent; border: none; }"
        "#floatPercentLabel { color: %3; font-size: 11px;"
        "  background: transparent; border: none; }"
        "#floatPauseButton { color: %3; background: transparent;"
        "  border: 1px solid %2; border-radius: 6px;"
        "  padding: 1px 10px; font-size: 11px; }"
        "#floatPauseButton:hover:enabled { color: %5; border-color: %5; }"
        "#floatPauseButton:disabled { color: %6; border-color: %2; }"
        "#floatProgressBar { background: %7; border: none;"
        "  border-radius: 3px; min-height: 6px; max-height: 6px; }"
        "#floatProgressBar::chunk { background: %5; border-radius: 3px; }")
        .arg(t.card.name(), t.divider.name(), t.text.name(),
             t.text_secondary.name(), t.accent.name(),
             t.text_disabled.name(),
             t.divider.name()));
}

void SpeedFloatWidget::contextMenuEvent(QContextMenuEvent* event)
{
    // B21②: 常用项右键菜单(穿透开启时窗口对输入透明,本事件不会到达)
    QMenu menu(this);
    menu.setAttribute(Qt::WA_DeleteOnClose, false);

    QAction* toggle = menu.addAction(
        preview_.running ? tr("暂停任务") : tr("继续任务"));
    toggle->setEnabled(preview_.has_task);
    connect(toggle, &QAction::triggered, this, [this]() {
        if (preview_.has_task) {
            emit pause_resume_requested(preview_.task_id, preview_.running);
        }
    });

    QAction* copy_link = menu.addAction(tr("复制链接"));
    copy_link->setEnabled(!preview_.url.isEmpty());
    connect(copy_link, &QAction::triggered, this, [this]() {
        QGuiApplication::clipboard()->setText(preview_.url);
    });

    QAction* open_dir = menu.addAction(tr("打开所在目录"));
    open_dir->setEnabled(!preview_.directory.isEmpty());
    connect(open_dir, &QAction::triggered, this, [this]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(preview_.directory));
    });

    menu.addSeparator();
    QAction* close_action = menu.addAction(tr("关闭浮窗"));
    connect(close_action, &QAction::triggered, this,
            &SpeedFloatWidget::close_requested);

    menu.exec(event->globalPos());
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

void SpeedFloatWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    refresh_name_elide();
}

void SpeedFloatWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (windowHandle()) {
        windowHandle()->setFlag(Qt::WindowTransparentForInput, click_through_);
    }
    refresh_name_elide();
}

} // namespace falcon::desktop
