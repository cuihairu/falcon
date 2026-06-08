/**
 * @file top_bar.cpp
 * @brief 顶部工具栏实现
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "top_bar.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>

namespace falcon::desktop {

namespace {
constexpr int kTopBarHeight = 72;
constexpr int kToolButtonSize = 36;
constexpr int kWindowButtonWidth = 42;
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
    main_layout->setContentsMargins(20, 12, 10, 12);
    main_layout->setSpacing(14);

    auto* brand_layout = new QHBoxLayout();
    brand_layout->setSpacing(10);

    brand_mark_ = new QLabel(tr("F"), this);
    brand_mark_->setObjectName("brandMark");
    brand_mark_->setAlignment(Qt::AlignCenter);
    brand_mark_->setFixedSize(34, 34);
    brand_layout->addWidget(brand_mark_);

    auto* title_stack = new QVBoxLayout();
    title_stack->setSpacing(1);
    title_stack->setContentsMargins(0, 0, 0, 0);

    brand_title_ = new QLabel(tr("Falcon"), this);
    brand_title_->setObjectName("brandTitle");
    title_stack->addWidget(brand_title_);

    brand_subtitle_ = new QLabel(tr("高速下载工作台"), this);
    brand_subtitle_->setObjectName("brandSubtitle");
    title_stack->addWidget(brand_subtitle_);

    brand_layout->addLayout(title_stack);
    main_layout->addLayout(brand_layout);

    // 搜索框
    search_edit_ = new QLineEdit(this);
    search_edit_->setPlaceholderText(tr("粘贴链接、磁力或搜索资源"));
    search_edit_->setObjectName("searchEdit");
    search_edit_->setMinimumHeight(36);
    connect(search_edit_, &QLineEdit::returnPressed, this, [this]() {
        emit searchRequested(search_edit_->text());
    });
    main_layout->addWidget(search_edit_, 5);

    meta_badge_ = new QLabel(tr("Preview UI"), this);
    meta_badge_->setObjectName("titleMetaBadge");
    meta_badge_->setAlignment(Qt::AlignCenter);
    meta_badge_->setMinimumWidth(82);
    meta_badge_->setFixedHeight(24);
    main_layout->addWidget(meta_badge_);

    main_layout->addStretch(1);

    // 功能按钮
    refresh_button_ = new QPushButton(tr("刷新"), this);
    refresh_button_->setObjectName("toolButton");
    refresh_button_->setFixedHeight(kToolButtonSize);
    connect(refresh_button_, &QPushButton::clicked, this, &TopBar::refreshClicked);
    main_layout->addWidget(refresh_button_);

    view_toggle_button_ = new QPushButton(tr("视图"), this);
    view_toggle_button_->setObjectName("toolButton");
    view_toggle_button_->setFixedHeight(kToolButtonSize);
    connect(view_toggle_button_, &QPushButton::clicked, this, &TopBar::viewToggleClicked);
    main_layout->addWidget(view_toggle_button_);

    more_button_ = new QPushButton(tr("更多"), this);
    more_button_->setObjectName("toolButton");
    more_button_->setFixedHeight(kToolButtonSize);
    connect(more_button_, &QPushButton::clicked, this, &TopBar::moreOptionsClicked);
    main_layout->addWidget(more_button_);

    main_layout->addSpacing(8);

    // 窗口控制按钮
    minimize_button_ = new QPushButton(this);
    minimize_button_->setObjectName("windowButton");
    minimize_button_->setFixedSize(kWindowButtonWidth, 32);
    minimize_button_->setText(tr("_"));
    minimize_button_->setToolTip(tr("最小化"));
    connect(minimize_button_, &QPushButton::clicked, this, &TopBar::minimizeClicked);
    main_layout->addWidget(minimize_button_);

    maximize_button_ = new QPushButton(this);
    maximize_button_->setObjectName("windowButton");
    maximize_button_->setFixedSize(kWindowButtonWidth, 32);
    maximize_button_->setText(tr("[ ]"));
    maximize_button_->setToolTip(tr("最大化"));
    connect(maximize_button_, &QPushButton::clicked, this, &TopBar::maximizeClicked);
    main_layout->addWidget(maximize_button_);

    close_button_ = new QPushButton(this);
    close_button_->setObjectName("closeButton");
    close_button_->setFixedSize(kWindowButtonWidth, 32);
    close_button_->setText(tr("X"));
    close_button_->setToolTip(tr("关闭"));
    connect(close_button_, &QPushButton::clicked, this, &TopBar::closeClicked);
    main_layout->addWidget(close_button_);
}

} // namespace falcon::desktop
