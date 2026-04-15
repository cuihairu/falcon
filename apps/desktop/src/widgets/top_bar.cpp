/**
 * @file top_bar.cpp
 * @brief 顶部工具栏实现
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "top_bar.hpp"

#include <QHBoxLayout>
#include <QIcon>
#include <QPainter>
#include <QStyleOption>

namespace falcon::desktop {

namespace {
constexpr int kTopBarHeight = 56;
constexpr int kButtonSize = 32;
} // namespace

TopBar::TopBar(QWidget* parent)
    : QWidget(parent)
{
    setup_ui();
    setup_styles();
}

TopBar::~TopBar() = default;

void TopBar::setup_ui()
{
    setFixedHeight(kTopBarHeight);
    setObjectName("topBar");

    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(16, 8, 8, 8);
    main_layout->setSpacing(12);

    // 搜索框
    search_edit_ = new QLineEdit(this);
    search_edit_->setPlaceholderText(tr("搜索全网、下载、云盘或收藏地址"));
    search_edit_->setObjectName("searchEdit");
    search_edit_->setMinimumHeight(36);
    connect(search_edit_, &QLineEdit::returnPressed, this, [this]() {
        emit searchRequested(search_edit_->text());
    });
    main_layout->addWidget(search_edit_, 6); // stretch factor 6

    // 中间弹性空间
    main_layout->addStretch(2);

    // 功能按钮
    refresh_button_ = new QPushButton(this);
    refresh_button_->setObjectName("toolButton");
    refresh_button_->setFixedSize(kButtonSize, kButtonSize);
    refresh_button_->setText(tr("🔄"));
    refresh_button_->setToolTip(tr("刷新"));
    connect(refresh_button_, &QPushButton::clicked, this, &TopBar::refreshClicked);
    main_layout->addWidget(refresh_button_);

    view_toggle_button_ = new QPushButton(this);
    view_toggle_button_->setObjectName("toolButton");
    view_toggle_button_->setFixedSize(kButtonSize, kButtonSize);
    view_toggle_button_->setText(tr("▦"));
    view_toggle_button_->setToolTip(tr("切换视图"));
    connect(view_toggle_button_, &QPushButton::clicked, this, &TopBar::viewToggleClicked);
    main_layout->addWidget(view_toggle_button_);

    more_button_ = new QPushButton(this);
    more_button_->setObjectName("toolButton");
    more_button_->setFixedSize(kButtonSize, kButtonSize);
    more_button_->setText(tr("⋯"));
    more_button_->setToolTip(tr("更多选项"));
    connect(more_button_, &QPushButton::clicked, this, &TopBar::moreOptionsClicked);
    main_layout->addWidget(more_button_);

    main_layout->addSpacing(16);

    // 窗口控制按钮
    minimize_button_ = new QPushButton(this);
    minimize_button_->setObjectName("windowButton");
    minimize_button_->setFixedSize(46, 32);
    minimize_button_->setText(tr("−"));
    minimize_button_->setToolTip(tr("最小化"));
    connect(minimize_button_, &QPushButton::clicked, this, &TopBar::minimizeClicked);
    main_layout->addWidget(minimize_button_);

    maximize_button_ = new QPushButton(this);
    maximize_button_->setObjectName("windowButton");
    maximize_button_->setFixedSize(46, 32);
    maximize_button_->setText(tr("□"));
    maximize_button_->setToolTip(tr("最大化"));
    connect(maximize_button_, &QPushButton::clicked, this, &TopBar::maximizeClicked);
    main_layout->addWidget(maximize_button_);

    close_button_ = new QPushButton(this);
    close_button_->setObjectName("closeButton");
    close_button_->setFixedSize(46, 32);
    close_button_->setText(tr("✕"));
    close_button_->setToolTip(tr("关闭"));
    connect(close_button_, &QPushButton::clicked, this, &TopBar::closeClicked);
    main_layout->addWidget(close_button_);
}

void TopBar::setup_styles()
{
    // 搜索框样式
    search_edit_->setStyleSheet(R"(
        QLineEdit#searchEdit {
            border: none;
            background: #F5F5F5;
            border-radius: 18px;
            padding: 8px 16px 8px 40px;
            font-size: 13px;
            color: #666666;
        }
        QLineEdit#searchEdit:focus {
            background: #EEEEEE;
        }
    )");

    // 工具按钮样式
    QString tool_button_style = R"(
        QPushButton#toolButton {
            border: none;
            background: transparent;
            border-radius: 16px;
        }
        QPushButton#toolButton:hover {
            background: #E3F2FD;
        }
        QPushButton#toolButton:pressed {
            background: #BBDEFB;
        }
    )";

    refresh_button_->setStyleSheet(tool_button_style);
    view_toggle_button_->setStyleSheet(tool_button_style);
    more_button_->setStyleSheet(tool_button_style);

    // 窗口控制按钮样式
    QString window_button_style = R"(
        QPushButton#windowButton {
            border: none;
            background: transparent;
            border-radius: 4px;
        }
        QPushButton#windowButton:hover {
            background: #E0E0E0;
        }
        QPushButton#closeButton:hover {
            background: #F44336;
            color: white;
        }
        QPushButton#closeButton:pressed {
            background: #D32F2F;
        }
    )";

    minimize_button_->setStyleSheet(window_button_style);
    maximize_button_->setStyleSheet(window_button_style);
    close_button_->setStyleSheet(window_button_style);

    // 绘制窗口控制按钮图标
    auto paint_icon = [](QPushButton* btn, const QString& icon_name) {
        btn->setAttribute(Qt::WA_TransparentForMouseEvents);
        btn->setText("");
    };

    paint_icon(minimize_button_, "minimize");
    paint_icon(maximize_button_, "maximize");
    paint_icon(close_button_, "close");
}

} // namespace falcon::desktop
