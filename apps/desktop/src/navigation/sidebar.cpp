/**
 * @file sidebar.cpp
 * @brief Sidebar implementation (Fluent style)
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "sidebar.hpp"

#include "../utils/icon_utils.hpp"

#include <QVBoxLayout>
#include <QButtonGroup>
#include <QFrame>

namespace falcon::desktop {

namespace {
constexpr int kSideBarWidth = 232;
constexpr int kSectionSpacing = 18;
constexpr int kItemSpacing = 6;
} // namespace

SideBar::SideBar(QWidget* parent)
    : QWidget(parent)
    , main_layout_(nullptr)
    , nav_group_(new QButtonGroup(this))
    , downloading_tab_(nullptr)
    , completed_tab_(nullptr)
    , library_button_(nullptr)
    , third_party_button_(nullptr)
    , trash_button_(nullptr)
    , settings_button_(nullptr)
{
    nav_group_->setExclusive(true);
    setup_ui();
}

SideBar::~SideBar() = default;

void SideBar::setup_ui()
{
    setFixedWidth(kSideBarWidth);
    setObjectName("sideBar");

    main_layout_ = new QVBoxLayout(this);
    main_layout_->setContentsMargins(16, 18, 16, 18);
    main_layout_->setSpacing(kSectionSpacing);

    create_download_section();
    main_layout_->addWidget(create_separator());

    transfer_label_ = new QLabel(tr("发现与空间"), this);
    transfer_label_->setObjectName("sectionLabel");
    main_layout_->addWidget(transfer_label_);

    create_space_section();

    main_layout_->addStretch();

    footer_card_ = new QWidget(this);
    footer_card_->setObjectName("sideBarFooter");
    // 裸 QWidget 默认不绘制 QSS 边框/背景，必须显式开启
    footer_card_->setAttribute(Qt::WA_StyledBackground, true);
    auto* footer_layout = new QVBoxLayout(footer_card_);
    footer_layout->setContentsMargins(14, 14, 14, 14);
    footer_layout->setSpacing(4);

    footer_value_ = new QLabel(tr("0"), footer_card_);
    footer_value_->setObjectName("sidebarStatValue");
    footer_layout->addWidget(footer_value_);

    footer_label_ = new QLabel(tr("当前活跃任务"), footer_card_);
    footer_label_->setObjectName("sidebarStatLabel");
    footer_layout->addWidget(footer_label_);

    main_layout_->addWidget(footer_card_);
}

void SideBar::set_queue_count(int count)
{
    if (footer_value_) {
        footer_value_->setText(QString::number(count));
    }
}

QPushButton* SideBar::create_nav_button(const QString& text, icons::Id icon)
{
    auto* button = new QPushButton(text, this);
    button->setObjectName("navTab");
    button->setCheckable(true);
    // 主题感知图标（TokenIconEngine 绘制时取当前主题 token，换肤自动换色）
    button->setIcon(icons::themed(icon, icons::ColorRole::Text));
    nav_group_->addButton(button);
    return button;
}

QWidget* SideBar::create_separator()
{
    auto* separator = new QWidget(this);
    separator->setObjectName("separatorLine");
    separator->setFixedHeight(1);
    return separator;
}

void SideBar::create_download_section()
{
    my_download_label_ = new QLabel(tr("我的下载"), this);
    my_download_label_->setObjectName("sectionLabel");
    main_layout_->addWidget(my_download_label_);

    auto* tabs_layout = new QVBoxLayout();
    tabs_layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(tabs_layout);

    downloading_tab_ = create_nav_button(tr("下载中"), icons::Id::Download);
    downloading_tab_->setChecked(true);
    tabs_layout->addWidget(downloading_tab_);
    connect(downloading_tab_, &QPushButton::clicked, this, &SideBar::downloadingTabClicked);

    completed_tab_ = create_nav_button(tr("已完成"), icons::Id::CheckCircle);
    tabs_layout->addWidget(completed_tab_);
    connect(completed_tab_, &QPushButton::clicked, this, &SideBar::completedTabClicked);
}

void SideBar::create_space_section()
{
    auto* tools_layout = new QVBoxLayout();
    tools_layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(tools_layout);

    library_button_ = create_nav_button(tr("资源发现"), icons::Id::Search);
    tools_layout->addWidget(library_button_);
    connect(library_button_, &QPushButton::clicked, this, &SideBar::discoveryClicked);

    third_party_button_ = create_nav_button(tr("云盘空间"), icons::Id::Cloud);
    tools_layout->addWidget(third_party_button_);
    connect(third_party_button_, &QPushButton::clicked, this, &SideBar::cloudClicked);

    trash_button_ = create_nav_button(tr("回收站"), icons::Id::Trash);
    tools_layout->addWidget(trash_button_);
    connect(trash_button_, &QPushButton::clicked, this, &SideBar::trashClicked);

    settings_button_ = create_nav_button(tr("偏好设置"), icons::Id::Settings);
    tools_layout->addWidget(settings_button_);
    connect(settings_button_, &QPushButton::clicked, this, &SideBar::settingsClicked);
}

} // namespace falcon::desktop
