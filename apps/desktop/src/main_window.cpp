/**
 * @file main_window.cpp
 * @brief 主窗口实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "main_window.hpp"
#include "navigation/sidebar.hpp"
#include "pages/download_page.hpp"
#include "pages/cloud_page.hpp"
#include "pages/discovery_page.hpp"

#include <QHBoxLayout>
#include <QWidget>

namespace falcon::desktop {

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , side_bar_(nullptr)
    , content_stack_(nullptr)
{
    setup_ui();
}

MainWindow::~MainWindow() = default;

void MainWindow::setup_ui()
{
    setWindowTitle("Falcon 下载器");
    resize(1200, 800);

    // 创建中心部件
    auto* central_widget = new QWidget(this);
    setCentralWidget(central_widget);

    // 主布局
    auto* main_layout = new QHBoxLayout(central_widget);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // 创建侧边栏
    create_side_bar();
    main_layout->addWidget(side_bar_);

    // 创建内容区域
    create_content_area();
    main_layout->addWidget(content_stack_, 1); // 内容区域占据剩余空间
}

void MainWindow::create_side_bar()
{
    side_bar_ = new SideBar(this);

    // 连接侧边栏信号到页面切换
    connect(side_bar_, &SideBar::downloadClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_DOWNLOAD);
    });

    connect(side_bar_, &SideBar::cloudClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_CLOUD);
    });

    connect(side_bar_, &SideBar::discoveryClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_DISCOVERY);
    });
}

void MainWindow::create_content_area()
{
    content_stack_ = new QStackedWidget(this);
    create_pages();
}

void MainWindow::create_pages()
{
    // 下载页面
    auto* download_page = new DownloadPage(this);
    content_stack_->addWidget(download_page);

    // 云盘页面
    auto* cloud_page = new CloudPage(this);
    content_stack_->addWidget(cloud_page);

    // 发现页面
    auto* discovery_page = new DiscoveryPage(this);
    content_stack_->addWidget(discovery_page);
}

} // namespace falcon::desktop
