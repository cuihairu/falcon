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
#include "pages/settings_page.hpp"
#include "dialogs/add_download_dialog.hpp"
#include "utils/clipboard_monitor.hpp"
#include "utils/url_detector.hpp"
#include "ipc/http_server.hpp"

#include <QHBoxLayout>
#include <QWidget>
#include <QApplication>
#include <QMessageBox>

namespace falcon::desktop {

namespace {
constexpr quint16 kIpcPort = 51337;
} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , side_bar_(nullptr)
    , content_stack_(nullptr)
    , download_page_(nullptr)
    , clipboard_monitor_(nullptr)
    , ipc_server_(nullptr)
    , download_engine_(nullptr)
{
    setup_ui();
    setup_clipboard_monitor();
    setup_ipc_server();
    ensure_download_engine();
}

MainWindow::~MainWindow()
{
    if (clipboard_monitor_) {
        clipboard_monitor_->stop();
    }
    if (ipc_server_) {
        ipc_server_->stop();
    }
}

void MainWindow::setup_ui()
{
    setWindowTitle(tr("Falcon Downloader"));
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

void MainWindow::open_url(const QString& url)
{
    const UrlInfo url_info = UrlDetector::parse_url(url);
    if (!url_info.is_valid) {
        QMessageBox::warning(this, tr("Invalid URL"), tr("Unrecognized download URL:\n%1").arg(url));
        return;
    }
    on_url_detected(url_info);
}

void MainWindow::ensure_download_engine()
{
    if (download_engine_) {
        return;
    }
    download_engine_ = std::make_unique<falcon::DownloadEngine>();
}

void MainWindow::show_add_download_dialog(UrlInfo url_info, const IncomingDownloadRequest* request_context)
{
    if (request_context) {
        if (!request_context->filename.trimmed().isEmpty()) {
            url_info.file_name = request_context->filename.trimmed();
        }
    }

    AddDownloadDialog dialog(url_info, this);
    if (request_context) {
        dialog.set_request_referrer(request_context->referrer);
        dialog.set_request_user_agent(request_context->user_agent);
        dialog.set_request_cookies(request_context->cookies);
    }

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    ensure_download_engine();

    falcon::DownloadOptions options;
    options.max_connections = static_cast<std::size_t>(dialog.get_connections());
    options.output_directory = dialog.get_save_path().toStdString();
    options.output_filename = dialog.get_file_name().toStdString();
    options.user_agent = dialog.get_user_agent().toStdString();
    options.referer = dialog.get_referrer().toStdString();

    const QString cookies = dialog.get_cookies();
    if (!cookies.isEmpty()) {
        options.headers["Cookie"] = cookies.toStdString();
    }

    const std::string url = dialog.get_url().toStdString();
    auto task = download_engine_->add_task(url, options);
    if (!task) {
        QMessageBox::warning(this, tr("Download"), tr("URL is not supported."));
        return;
    }

    (void)download_engine_->start_task(task->id());

    if (download_page_) {
        download_page_->add_engine_task(task);
    }

    QMessageBox::information(
        this,
        tr("Download Added"),
        tr("A download task was added:\n\nURL: %1\nSave path: %2\nFile name: %3\nConnections: %4")
            .arg(dialog.get_url(), dialog.get_save_path(), dialog.get_file_name())
            .arg(dialog.get_connections())
    );
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

    connect(side_bar_, &SideBar::settingsClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_SETTINGS);
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
    download_page_ = new DownloadPage(this);
    content_stack_->addWidget(download_page_);

    // 云盘页面
    auto* cloud_page = new CloudPage(this);
    content_stack_->addWidget(cloud_page);

    // 发现页面
    auto* discovery_page = new DiscoveryPage(this);
    content_stack_->addWidget(discovery_page);

    // 设置页面
    auto* settings_page = new SettingsPage(this);
    content_stack_->addWidget(settings_page);

    // 连接设置页面信号
    connect(settings_page, &SettingsPage::clipboard_monitoring_toggled, this, [this](bool enabled) {
        if (clipboard_monitor_) {
            clipboard_monitor_->set_enabled(enabled);
        }
    });
}

void MainWindow::setup_clipboard_monitor()
{
    QClipboard* clipboard = QApplication::clipboard();
    clipboard_monitor_ = new ClipboardMonitor(clipboard, this);

    // 连接URL检测信号
    connect(clipboard_monitor_, &ClipboardMonitor::url_detected, this, &MainWindow::on_url_detected);

    // 默认不启动，由设置页面控制
    // clipboard_monitor_->start();
}

void MainWindow::setup_ipc_server()
{
    ipc_server_ = new HttpIpcServer(this);
    connect(ipc_server_, &HttpIpcServer::download_requested, this, &MainWindow::on_download_requested);
    ipc_server_->start(kIpcPort);
}

void MainWindow::on_url_detected(const UrlInfo& url_info)
{
    show_add_download_dialog(url_info, nullptr);
}

void MainWindow::on_download_requested(const IncomingDownloadRequest& request)
{
    const UrlInfo url_info = UrlDetector::parse_url(request.url);
    if (!url_info.is_valid) {
        QMessageBox::warning(this, tr("Invalid URL"), tr("Unrecognized download URL:\n%1").arg(request.url));
        return;
    }

    show_add_download_dialog(url_info, &request);
}

} // namespace falcon::desktop
