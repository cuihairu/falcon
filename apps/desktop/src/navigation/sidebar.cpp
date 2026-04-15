/**
 * @file sidebar.cpp
 * @brief Sidebar implementation (Xunlei-style)
 * @author Falcon Team
 * @date 2026-04-15
 */

#include "sidebar.hpp"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QButtonGroup>

namespace falcon::desktop {

namespace {
constexpr int kSideBarWidth = 200;
constexpr int kSectionSpacing = 16;
constexpr int kItemSpacing = 4;
} // namespace

SideBar::SideBar(QWidget* parent)
    : QWidget(parent)
    , main_layout_(nullptr)
    , my_download_label_(nullptr)
    , my_download_tabs_(new QButtonGroup(this))
    , downloading_tab_(nullptr)
    , completed_tab_(nullptr)
    , cloud_add_tab_(nullptr)
    , common_tools_(new QButtonGroup(this))
    , library_button_(nullptr)
    , third_party_button_(nullptr)
    , recycle_bin_button_(nullptr)
    , private_space_label_(nullptr)
    , private_space_tools_(new QButtonGroup(this))
    , private_downloading_button_(nullptr)
    , private_completed_button_(nullptr)
{
    setup_ui();
}

SideBar::~SideBar() = default;

void SideBar::setup_ui()
{
    setFixedWidth(kSideBarWidth);
    setObjectName("sideBar");

    main_layout_ = new QVBoxLayout(this);
    main_layout_->setContentsMargins(12, 16, 12, 16);
    main_layout_->setSpacing(kSectionSpacing);

    create_my_download_section();
    create_common_tools_section();
    create_private_space_section();

    main_layout_->addStretch();
}

void SideBar::create_my_download_section()
{
    // 区域标题
    my_download_label_ = new QLabel(tr("我的下载"), this);
    my_download_label_->setObjectName("navLabel");
    main_layout_->addWidget(my_download_label_);

    // 标签页容器
    auto* tabs_layout = new QVBoxLayout();
    tabs_layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(tabs_layout);

    // 下载中标签
    downloading_tab_ = new QPushButton(tr("下载中"), this);
    downloading_tab_->setObjectName("navTab");
    downloading_tab_->setCheckable(true);
    downloading_tab_->setChecked(true);  // 默认选中
    my_download_tabs_->addButton(downloading_tab_, 0);
    tabs_layout->addWidget(downloading_tab_);
    connect(downloading_tab_, &QPushButton::clicked, this, &SideBar::downloadingTabClicked);
    connect(downloading_tab_, &QPushButton::clicked, this, &SideBar::downloadClicked);

    // 已完成标签
    completed_tab_ = new QPushButton(tr("已完成"), this);
    completed_tab_->setObjectName("navTab");
    completed_tab_->setCheckable(true);
    my_download_tabs_->addButton(completed_tab_, 1);
    tabs_layout->addWidget(completed_tab_);
    connect(completed_tab_, &QPushButton::clicked, this, &SideBar::completedTabClicked);
    connect(completed_tab_, &QPushButton::clicked, this, &SideBar::downloadClicked);

    // 云添加标签
    cloud_add_tab_ = new QPushButton(tr("云添加"), this);
    cloud_add_tab_->setObjectName("navTab");
    cloud_add_tab_->setCheckable(true);
    my_download_tabs_->addButton(cloud_add_tab_, 2);
    tabs_layout->addWidget(cloud_add_tab_);
    connect(cloud_add_tab_, &QPushButton::clicked, this, &SideBar::cloudAddTabClicked);
    connect(cloud_add_tab_, &QPushButton::clicked, this, &SideBar::cloudClicked);
}

void SideBar::create_common_tools_section()
{
    // 分隔线
    auto* separator = new QWidget(this);
    separator->setFixedHeight(1);
    separator->setStyleSheet("background-color: #E0E0E0;");
    main_layout_->addWidget(separator);

    // 常用工具按钮
    auto* tools_layout = new QVBoxLayout();
    tools_layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(tools_layout);

    // 片库
    library_button_ = new QPushButton(tr("片库"), this);
    library_button_->setObjectName("navTab");
    library_button_->setCheckable(true);
    common_tools_->addButton(library_button_, 0);
    tools_layout->addWidget(library_button_);
    connect(library_button_, &QPushButton::clicked, this, &SideBar::discoveryClicked);

    // 三方网盘
    third_party_button_ = new QPushButton(tr("三方"), this);
    third_party_button_->setObjectName("navTab");
    third_party_button_->setCheckable(true);
    common_tools_->addButton(third_party_button_, 1);
    tools_layout->addWidget(third_party_button_);
    connect(third_party_button_, &QPushButton::clicked, this, &SideBar::cloudClicked);

    // 回收站
    recycle_bin_button_ = new QPushButton(tr("回收站"), this);
    recycle_bin_button_->setObjectName("navTab");
    recycle_bin_button_->setCheckable(true);
    common_tools_->addButton(recycle_bin_button_, 2);
    tools_layout->addWidget(recycle_bin_button_);
    connect(recycle_bin_button_, &QPushButton::clicked, this, &SideBar::settingsClicked);
}

void SideBar::create_private_space_section()
{
    // 分隔线
    auto* separator = new QWidget(this);
    separator->setFixedHeight(1);
    separator->setStyleSheet("background-color: #E0E0E0;");
    main_layout_->addWidget(separator);

    // 区域标题
    private_space_label_ = new QLabel(tr("私人空间"), this);
    private_space_label_->setObjectName("navLabel");
    main_layout_->addWidget(private_space_label_);

    // 私人空间按钮
    auto* space_layout = new QVBoxLayout();
    space_layout->setSpacing(kItemSpacing);
    main_layout_->addLayout(space_layout);

    // 下载中
    private_downloading_button_ = new QPushButton(tr("下载中"), this);
    private_downloading_button_->setObjectName("navTab");
    private_downloading_button_->setCheckable(true);
    private_space_tools_->addButton(private_downloading_button_, 0);
    space_layout->addWidget(private_downloading_button_);
    connect(private_downloading_button_, &QPushButton::clicked, this, &SideBar::downloadClicked);

    // 已完成
    private_completed_button_ = new QPushButton(tr("已完成"), this);
    private_completed_button_->setObjectName("navTab");
    private_completed_button_->setCheckable(true);
    private_space_tools_->addButton(private_completed_button_, 1);
    space_layout->addWidget(private_completed_button_);
    connect(private_completed_button_, &QPushButton::clicked, this, &SideBar::downloadClicked);
}

} // namespace falcon::desktop
