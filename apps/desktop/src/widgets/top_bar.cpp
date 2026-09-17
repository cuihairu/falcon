/**
 * @file top_bar.cpp
 * @brief 顶部工具栏实现
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "top_bar.hpp"

#include "icon_utils.hpp"

#include <QHBoxLayout>
#include <QMouseEvent>

namespace falcon::desktop {

namespace {
using icons::ColorRole;
using icons::Id;
constexpr int kTopBarHeight = 56;
constexpr int kToolButtonSize = 36;
constexpr int kToolIconSize = 18;
constexpr int kWindowButtonWidth = 46;
constexpr int kWindowButtonHeight = 32;
constexpr int kWindowIconSize = 14;
} // namespace

TopBar::TopBar(QWidget* parent)
    : QWidget(parent)
{
    setup_ui();
}

TopBar::~TopBar() = default;

void TopBar::setup_ui()
{
    setFixedHeight(kTopBarHeight);
    setObjectName("topBar");

    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(20, 10, 0, 10);
    main_layout->setSpacing(14);

    auto* brand_layout = new QHBoxLayout();
    brand_layout->setSpacing(10);

    brand_mark_ = new QLabel(tr("F"), this);
    brand_mark_->setObjectName("brandMark");
    brand_mark_->setAlignment(Qt::AlignCenter);
    brand_mark_->setFixedSize(30, 30);
    brand_layout->addWidget(brand_mark_);

    auto* title_stack = new QVBoxLayout();
    title_stack->setSpacing(0);
    title_stack->setContentsMargins(0, 0, 0, 0);

    brand_title_ = new QLabel(tr("Falcon"), this);
    brand_title_->setObjectName("brandTitle");
    title_stack->addWidget(brand_title_);

    brand_subtitle_ = new QLabel(tr("高速下载工作台"), this);
    brand_subtitle_->setObjectName("brandSubtitle");
    title_stack->addWidget(brand_subtitle_);

    brand_layout->addLayout(title_stack);
    main_layout->addLayout(brand_layout);

    // 搜索框(回车 → 过滤下载任务)
    search_edit_ = new QLineEdit(this);
    search_edit_->setPlaceholderText(tr("过滤下载任务"));
    search_edit_->setObjectName("searchEdit");
    search_edit_->setClearButtonEnabled(true);
    search_edit_->setFixedHeight(34);
    connect(search_edit_, &QLineEdit::returnPressed, this, [this]() {
        emit searchRequested(search_edit_->text());
    });
    main_layout->addWidget(search_edit_, 5);

    main_layout->addStretch(1);

    // 功能按钮(纯图标,Fluent 工具钮)
    refresh_button_ = new QPushButton(this);
    refresh_button_->setObjectName("toolButton");
    refresh_button_->setIcon(icons::themed(Id::Refresh, ColorRole::TextSecondary));
    refresh_button_->setIconSize(QSize(kToolIconSize, kToolIconSize));
    refresh_button_->setFixedSize(kToolButtonSize, kToolButtonSize);
    refresh_button_->setToolTip(tr("立即刷新"));
    connect(refresh_button_, &QPushButton::clicked, this, &TopBar::refreshClicked);
    main_layout->addWidget(refresh_button_);

    view_toggle_button_ = new QPushButton(this);
    view_toggle_button_->setObjectName("toolButton");
    view_toggle_button_->setIcon(icons::themed(Id::LayoutGrid, ColorRole::TextSecondary));
    view_toggle_button_->setIconSize(QSize(kToolIconSize, kToolIconSize));
    view_toggle_button_->setFixedSize(kToolButtonSize, kToolButtonSize);
    view_toggle_button_->setToolTip(tr("切换列表/网格视图"));
    connect(view_toggle_button_, &QPushButton::clicked, this, &TopBar::viewToggleClicked);
    main_layout->addWidget(view_toggle_button_);

    main_layout->addSpacing(8);

    // 窗口控制按钮(SVG 图标,无边框贴边)
    minimize_button_ = new QPushButton(this);
    minimize_button_->setObjectName("windowButton");
    minimize_button_->setFixedSize(kWindowButtonWidth, kWindowButtonHeight);
    minimize_button_->setIconSize(QSize(kWindowIconSize, kWindowIconSize));
    minimize_button_->setToolTip(tr("最小化"));
    connect(minimize_button_, &QPushButton::clicked, this, &TopBar::minimizeClicked);
    main_layout->addWidget(minimize_button_);

    maximize_button_ = new QPushButton(this);
    maximize_button_->setObjectName("windowButton");
    maximize_button_->setFixedSize(kWindowButtonWidth, kWindowButtonHeight);
    maximize_button_->setIconSize(QSize(kWindowIconSize, kWindowIconSize));
    maximize_button_->setToolTip(tr("最大化"));
    connect(maximize_button_, &QPushButton::clicked, this, &TopBar::maximizeClicked);
    main_layout->addWidget(maximize_button_);

    close_button_ = new QPushButton(this);
    close_button_->setObjectName("closeButton");
    close_button_->setFixedSize(kWindowButtonWidth, kWindowButtonHeight);
    close_button_->setIconSize(QSize(kWindowIconSize, kWindowIconSize));
    close_button_->setToolTip(tr("关闭"));
    connect(close_button_, &QPushButton::clicked, this, &TopBar::closeClicked);
    main_layout->addWidget(close_button_);

    update_window_button_icons();
}

void TopBar::set_maximized(bool maximized)
{
    if (maximized_ == maximized) {
        return;
    }
    maximized_ = maximized;
    update_window_button_icons();
}

void TopBar::set_view_toggle_enabled(bool enabled)
{
    view_toggle_button_->setEnabled(enabled);
}

void TopBar::mousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && window()->windowHandle()) {
        // 空白区/品牌区拖动;输入框与按钮自消费事件,不会到达这里
        window()->windowHandle()->startSystemMove();
        return;
    }
    QWidget::mousePressEvent(event);
}

void TopBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton) {
        emit maximizeClicked();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void TopBar::update_window_button_icons()
{
    minimize_button_->setIcon(icons::themed(Id::Minus, ColorRole::TextSecondary));
    maximize_button_->setIcon(icons::themed(maximized_ ? Id::Restore : Id::Square,
                                            ColorRole::TextSecondary));
    close_button_->setIcon(icons::themed(Id::X, ColorRole::TextSecondary));
}

} // namespace falcon::desktop
