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

    main_layout_->addWidget(create_separator());
    auto* device_label = new QLabel(tr("跨设备与节点"), this);
    device_label->setObjectName("sectionLabel");
    main_layout_->addWidget(device_label);
    create_device_section();

    main_layout_->addWidget(create_separator());
    auto* social_label = new QLabel(tr("账号与分享"), this);
    social_label->setObjectName("sectionLabel");
    main_layout_->addWidget(social_label);
    create_social_section();

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

void SideBar::set_active_download_tab(bool completed)
{
    if (!downloading_tab_ || !completed_tab_) {
        return;
    }
    // QSignalBlocker + 双向显式 setChecked：程序化高亮不触发 clicked
    // （本类两个信号源），不回流 MainWindow；两个都显式设置，不依赖
    // QButtonGroup 排他行为在信号阻断下的具体实现
    const QSignalBlocker block_downloading(downloading_tab_);
    const QSignalBlocker block_completed(completed_tab_);
    downloading_tab_->setChecked(!completed);
    completed_tab_->setChecked(completed);
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

void SideBar::create_device_section()
{
    auto* layout = new QVBoxLayout();
    layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(layout);

    auto* cross = create_nav_button(tr("跨设备"), icons::Id::Upload);
    layout->addWidget(cross);
    extra_nav_buttons_ << cross;
    connect(cross, &QPushButton::clicked, this, &SideBar::crossDeviceClicked);

    auto* nas = create_nav_button(tr("NAS 与路由"), icons::Id::Cloud);
    layout->addWidget(nas);
    extra_nav_buttons_ << nas;
    connect(nas, &QPushButton::clicked, this, &SideBar::nasNodesClicked);

    auto* airdrop = create_nav_button(tr("隔空传送"), icons::Id::Link);
    layout->addWidget(airdrop);
    extra_nav_buttons_ << airdrop;
    connect(airdrop, &QPushButton::clicked, this, &SideBar::airdropClicked);

    auto* nas_web = create_nav_button(tr("NAS Web 管理"), icons::Id::Folder);
    layout->addWidget(nas_web);
    extra_nav_buttons_ << nas_web;
    connect(nas_web, &QPushButton::clicked, this, &SideBar::nasWebClicked);
}

void SideBar::create_social_section()
{
    auto* layout = new QVBoxLayout();
    layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(layout);

    auto* account = create_nav_button(tr("账号"), icons::Id::File);
    layout->addWidget(account);
    extra_nav_buttons_ << account;
    connect(account, &QPushButton::clicked, this, &SideBar::accountClicked);

    auto* friends = create_nav_button(tr("好友"), icons::Id::File);
    layout->addWidget(friends);
    extra_nav_buttons_ << friends;
    connect(friends, &QPushButton::clicked, this, &SideBar::friendsClicked);

    auto* groups = create_nav_button(tr("分享组"), icons::Id::Folder);
    layout->addWidget(groups);
    extra_nav_buttons_ << groups;
    connect(groups, &QPushButton::clicked, this, &SideBar::shareGroupsClicked);

    auto* records = create_nav_button(tr("分享记录"), icons::Id::List);
    layout->addWidget(records);
    extra_nav_buttons_ << records;
    connect(records, &QPushButton::clicked, this, &SideBar::shareRecordsClicked);
}

} // namespace falcon::desktop
