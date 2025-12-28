/**
 * @file sidebar.cpp
 * @brief Sidebar implementation with modern styling
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "sidebar.hpp"
#include "../styles.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QEasingCurve>

namespace falcon::desktop {

//==============================================================================
// SideBarButton Implementation
//==============================================================================

SideBarButton::SideBarButton(const QString& icon_text,
                               const QString& tooltip,
                               QWidget* parent)
    : QPushButton(parent)
{
    setText(icon_text);
    setToolTip(tooltip);
    setCheckable(false);
    setFixedHeight(50); // Slightly more compact
    setCursor(Qt::PointingHandCursor);

    // Initial style
    update_style(false);
}

void SideBarButton::update_style(bool active)
{
    if (active) {
        setStyleSheet(R"(
            QPushButton {
                background-color: #EDF2F7; /* Light gray background for active */
                color: #5C6BC0; /* Brand color text */
                border: none;
                border-left: 4px solid #5C6BC0; /* Brand color indicator */
                text-align: left;
                padding-left: 16px;
                font-size: 14px;
                font-weight: 600;
                border-radius: 0 4px 4px 0; /* Rounded on the right */
                margin-right: 8px; /* Spacing from right edge */
            }
        )");
    } else {
        setStyleSheet(R"(
            QPushButton {
                background-color: transparent;
                color: #718096; /* Secondary text color */
                border: none;
                border-left: 4px solid transparent;
                text-align: left;
                padding-left: 16px;
                font-size: 14px;
                font-weight: 500;
                border-radius: 0 4px 4px 0;
                margin-right: 8px;
            }
            QPushButton:hover {
                background-color: #F7FAFC;
                color: #4A5568;
            }
        )");
    }
}

void SideBarButton::setActive(bool active)
{
    active_ = active;
    update_style(active);
}

//==============================================================================
// SideBar Implementation
//==============================================================================

SideBar::SideBar(QWidget* parent)
    : QWidget(parent)
    , layout_(nullptr)
    , download_button_(nullptr)
    , cloud_button_(nullptr)
    , discovery_button_(nullptr)
    , toggle_button_(nullptr)
    , width_animation_(new QPropertyAnimation(this, "maximumWidth"))
{
    setup_ui();

    // Animation config
    width_animation_->setDuration(250); // Slightly slower for smoothness
    width_animation_->setEasingCurve(QEasingCurve::OutCubic); // Smoother easing
}

SideBar::~SideBar() = default;

void SideBar::setup_ui()
{
    setFixedWidth(expanded_width_);
    setStyleSheet(R"(
        QWidget {
            background-color: #FFFFFF;
            border-right: 1px solid #E2E8F0;
        }
    )");

    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 24, 0, 24);
    layout_->setSpacing(4); // Tighter spacing

    // Buttons
    create_buttons();

    // Spacer
    layout_->addStretch();

    // Toggle button
    toggle_button_ = new QPushButton("◀", this);
    toggle_button_->setFixedSize(32, 32);
    toggle_button_->setCursor(Qt::PointingHandCursor);
    toggle_button_->setStyleSheet(R"(
        QPushButton {
            background-color: transparent;
            color: #A0AEC0;
            border: 1px solid #E2E8F0;
            border-radius: 16px;
            font-size: 12px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #F7FAFC;
            color: #718096;
            border-color: #CBD5E0;
        }
        QPushButton:pressed {
            background-color: #EDF2F7;
        }
    )");
    layout_->addWidget(toggle_button_, 0, Qt::AlignCenter);

    connect(toggle_button_, &QPushButton::clicked, this, &SideBar::toggle);

    // Default active
    download_button_->setActive(true);
}

void SideBar::create_buttons()
{
    // Downloads
    download_button_ = new SideBarButton("⬇  Downloads", "Manage Downloads", this);
    download_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->addWidget(download_button_);

    connect(download_button_, &SideBarButton::clicked, this, [this]() {
        update_button_states();
        download_button_->setActive(true);
        emit downloadClicked();
    });

    // Cloud
    cloud_button_ = new SideBarButton("☁  Cloud", "Cloud Storage", this);
    cloud_button_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    layout_->addWidget(cloud_button_);

    connect(cloud_button_, &SideBarButton::clicked, this, [this]() {
        update_button_states();
        cloud_button_->setActive(true);
        emit cloudClicked();
    });

    // Discovery
    discovery_button_ = new SideBarButton("🔍  Discovery", "Search & Find", this);
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

    // Expand text
    download_button_->setText("⬇  Downloads");
    cloud_button_->setText("☁  Cloud");
    discovery_button_->setText("🔍  Discovery");
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

    // Icons only
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