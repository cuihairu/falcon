/**
 * @file sidebar.cpp
 * @brief 侧边导航栏实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "sidebar.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>

namespace falcon::desktop {

//==============================================================================
// SideBarButton 实现
//==============================================================================

SideBarButton::SideBarButton(const QString& icon_text,
                               const QString& tooltip,
                               QWidget* parent)
    : QPushButton(parent)
{
    setText(icon_text);
    setToolTip(tooltip);
    setCheckable(false);
    setFixedHeight(50);

    // 样式设置
    setStyleSheet(R"(
        SideBarButton {
            border: none;
            background-color: transparent;
            color: #333;
            text-align: left;
            padding-left: 15px;
            font-size: 14px;
        }
        SideBarButton:hover {
            background-color: #e8e8e8;
        }
        SideBarButton:active {
            background-color: #d0d0d0;
        }
    )");
}

void SideBarButton::setActive(bool active)
{
    active_ = active;

    if (active) {
        setStyleSheet(R"(
            SideBarButton {
                border: none;
                background-color: #0078d4;
                color: white;
                text-align: left;
                padding-left: 15px;
                font-size: 14px;
            }
        )");
    } else {
        setStyleSheet(R"(
            SideBarButton {
                border: none;
                background-color: transparent;
                color: #333;
                text-align: left;
                padding-left: 15px;
                font-size: 14px;
            }
            SideBarButton:hover {
                background-color: #e8e8e8;
            }
        )");
    }
}

//==============================================================================
// SideBar 实现
//==============================================================================

SideBar::SideBar(QWidget* parent)
    : QWidget(parent)
    , layout_(nullptr)
    , download_button_(nullptr)
    , cloud_button_(nullptr)
    , discovery_button_(nullptr)
    , toggle_button_(nullptr)
    , width_animation_(nullptr)
{
    setup_ui();
}

SideBar::~SideBar() = default;

void SideBar::setup_ui()
{
    setFixedWidth(expanded_width_);
    setStyleSheet("background-color: #f5f5f5;");

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 20, 0, 20);
    layout_->setSpacing(5);

    // 创建按钮
    create_buttons();

    // 添加弹性空间
    layout_->addStretch();

    // 收起按钮
    toggle_button_ = new QPushButton("◀", this);
    toggle_button_->setFixedSize(40, 40);
    toggle_button_->setStyleSheet(R"(
        QPushButton {
            border: none;
            background-color: #e8e8e8;
            border-radius: 20px;
            font-size: 16px;
        }
        QPushButton:hover {
            background-color: #d0d0d0;
        }
    )");
    layout_->addWidget(toggle_button_, 0, Qt::AlignCenter);

    connect(toggle_button_, &QPushButton::clicked, this, &SideBar::toggle);

    // 设置默认激活按钮
    download_button_->setActive(true);

    // 创建宽度动画
    width_animation_ = new QPropertyAnimation(this, "maximumWidth");
    width_animation_->setDuration(200);
    width_animation_->setEasingCurve(QEasingCurve::InOutQuad);
}

void SideBar::create_buttons()
{
    // 下载按钮
    download_button_ = new SideBarButton("⬇ 下载", "下载管理", this);
    download_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->addWidget(download_button_);

    connect(download_button_, &SideBarButton::clicked, this, [this]() {
        update_button_states();
        download_button_->setActive(true);
        emit downloadClicked();
    });

    // 云盘按钮
    cloud_button_ = new SideBarButton("☁ 云存储", "云存储资源", this);
    cloud_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->addWidget(cloud_button_);

    connect(cloud_button_, &SideBarButton::clicked, this, [this]() {
        update_button_states();
        cloud_button_->setActive(true);
        emit cloudClicked();
    });

    // 发现按钮
    discovery_button_ = new SideBarButton("🔍 发现", "搜索资源", this);
    discovery_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->addWidget(discovery_button_);

    connect(discovery_button_, &SideBarButton::clicked, this, [this]() {
        update_button_states();
        discovery_button_->setActive(true);
        emit discoveryClicked();
    });
}

void SideBar::update_button_states()
{
    download_button_->setActive(false);
    cloud_button_->setActive(false);
    discovery_button_->setActive(false);
}

void SideBar::expand()
{
    expanded_ = true;

    width_animation_->setStartValue(collapsed_width_);
    width_animation_->setEndValue(expanded_width_);
    width_animation_->start();

    setMaximumWidth(expanded_width_);
    setMinimumWidth(expanded_width_);

    toggle_button_->setText("◀");

    // 展开按钮文本
    download_button_->setText("⬇ 下载");
    cloud_button_->setText("☁ 云存储");
    discovery_button_->setText("🔍 发现");
}

void SideBar::collapse()
{
    expanded_ = false;

    width_animation_->setStartValue(expanded_width_);
    width_animation_->setEndValue(collapsed_width_);
    width_animation_->start();

    setMaximumWidth(collapsed_width_);
    setMinimumWidth(collapsed_width_);

    toggle_button_->setText("▶");

    // 收起时只显示图标
    download_button_->setText("⬇");
    cloud_button_->setText("☁");
    discovery_button_->setText("🔍");
}

void SideBar::toggle()
{
    if (expanded_) {
        collapse();
    } else {
        expand();
    }
}

} // namespace falcon::desktop
