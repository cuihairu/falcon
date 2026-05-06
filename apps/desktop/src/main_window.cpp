/**
 * @file main_window.cpp
 * @brief 主窗口实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "main_window.hpp"
#include "widgets/top_bar.hpp"
#include "widgets/status_bar.hpp"
#include "navigation/sidebar.hpp"
#include "pages/download_page.hpp"
#include "pages/cloud_page.hpp"
#include "pages/discovery_page.hpp"
#include "pages/settings_page.hpp"
#include "dialogs/add_download_dialog.hpp"
#include "utils/clipboard_monitor.hpp"
#include "utils/url_detector.hpp"
#include "utils/theme_manager.hpp"
#include "ipc/http_server.hpp"

#include <QHBoxLayout>
#include <QWidget>
#include <QApplication>
#include <QDir>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QAction>
#include <QMenu>

namespace falcon::desktop {

namespace {
constexpr quint16 kIpcPort = 51337;
} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , side_bar_(nullptr)
    , status_bar_(nullptr)
    , content_stack_(nullptr)
    , download_page_(nullptr)
    , settings_page_(nullptr)
    , clipboard_monitor_(nullptr)
    , ipc_server_(nullptr)
    , system_tray_(nullptr)
    , tray_menu_(nullptr)
    , theme_manager_(nullptr)
    , download_engine_(nullptr)
{
    setup_ui();
    setup_clipboard_monitor();
    setup_system_tray();
    load_settings();
    apply_settings_to_runtime();
    setup_ipc_server();
    ensure_download_engine();

    // 初始化主题管理器
    theme_manager_ = new ThemeManager(this);
    // 从设置加载主题
    QSettings settings;
    settings.beginGroup("desktop");
    const QString theme_str = settings.value("theme", "light").toString();
    settings.endGroup();
    theme_manager_->set_theme(theme_str == "dark" ? ThemeType::Dark : ThemeType::Light);
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
    // 启用自定义标题栏
    setWindowFlags(Qt::WindowType::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);

    resize(1200, 800);

    // 创建中心部件（设置不透明背景，否则窗口会透明）
    auto* central_widget = new QWidget(this);
    central_widget->setObjectName("centralWidget");  // 用于 QSS 样式
    setCentralWidget(central_widget);

    // 主布局（垂直：顶部栏 + 内容区域 + 状态栏）
    auto* main_layout = new QVBoxLayout(central_widget);
    main_layout->setContentsMargins(0, 0, 0, 0);
    main_layout->setSpacing(0);

    // 创建顶部工具栏
    create_top_bar();
    main_layout->addWidget(top_bar_);

    // 创建水平布局容器（侧边栏 + 内容区域）
    auto* content_layout = new QHBoxLayout();
    content_layout->setContentsMargins(0, 0, 0, 0);
    content_layout->setSpacing(0);
    main_layout->addLayout(content_layout, 1); // 内容区域占据剩余空间

    // 创建侧边栏
    create_side_bar();
    content_layout->addWidget(side_bar_);

    // 创建内容区域
    create_content_area();
    content_layout->addWidget(content_stack_, 1); // 内容区域占据剩余空间

    // 创建底部状态栏
    status_bar_ = new StatusBar(this);
    main_layout->addWidget(status_bar_);
}

void MainWindow::create_top_bar()
{
    top_bar_ = new TopBar(this);

    connect(top_bar_, &TopBar::minimizeClicked, this, &MainWindow::on_minimize_requested);
    connect(top_bar_, &TopBar::maximizeClicked, this, &MainWindow::on_maximize_requested);
    connect(top_bar_, &TopBar::closeClicked, this, &MainWindow::on_close_requested);
    connect(top_bar_, &TopBar::refreshClicked, this, [this]() {
        // 刷新当前页面
        if (auto* page = qobject_cast<DownloadPage*>(content_stack_->currentWidget())) {
            // 触发刷新
        }
    });
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
    if (settings_page_) {
        dialog.set_default_save_path(settings_page_->get_default_download_dir());
        dialog.set_default_connections(settings_page_->get_default_connections());
    }

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

bool MainWindow::add_download_task(const QString& url, bool start_immediately)
{
    ensure_download_engine();

    falcon::DownloadOptions options;
    if (settings_page_) {
        options.max_connections = static_cast<std::size_t>(settings_page_->get_default_connections());
        options.output_directory = settings_page_->get_default_download_dir().toStdString();
    }

    auto task = download_engine_->add_task(url.toStdString(), options);
    if (!task) {
        return false;
    }

    if (start_immediately) {
        (void)download_engine_->start_task(task->id());
    }

    if (download_page_) {
        download_page_->add_engine_task(task);
    }

    return true;
}

void MainWindow::create_side_bar()
{
    side_bar_ = new SideBar(this);

    // 连接侧边栏信号到页面切换
    connect(side_bar_, &SideBar::downloadClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_DOWNLOAD);
        if (download_page_) {
            download_page_->set_view_mode(DownloadViewMode::Downloading);
        }
    });

    connect(side_bar_, &SideBar::downloadingTabClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_DOWNLOAD);
        if (download_page_) {
            download_page_->set_view_mode(DownloadViewMode::Downloading);
        }
    });

    connect(side_bar_, &SideBar::completedTabClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_DOWNLOAD);
        if (download_page_) {
            download_page_->set_view_mode(DownloadViewMode::Completed);
        }
    });

    connect(side_bar_, &SideBar::cloudAddTabClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_DOWNLOAD);
        if (download_page_) {
            download_page_->set_view_mode(DownloadViewMode::CloudAdd);
        }
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
    connect(download_page_, &DownloadPage::new_task_requested,
            this, &MainWindow::on_new_task_requested);
    connect(download_page_, &DownloadPage::remove_task_requested,
            this, &MainWindow::on_remove_task_requested);
    connect(download_page_, &DownloadPage::remove_finished_tasks_requested,
            this, &MainWindow::on_remove_finished_tasks_requested);

    // 云盘页面
    auto* cloud_page = new CloudPage(this);
    content_stack_->addWidget(cloud_page);

    // 发现页面
    auto* discovery_page = new DiscoveryPage(this);
    content_stack_->addWidget(discovery_page);
    connect(discovery_page, &DiscoveryPage::configured_download_requested,
            this, &MainWindow::on_configured_download_requested);
    connect(discovery_page, &DiscoveryPage::direct_download_requested,
            this, &MainWindow::on_direct_download_requested);

    // 设置页面
    settings_page_ = new SettingsPage(this);
    content_stack_->addWidget(settings_page_);

    // 连接设置页面信号
    connect(settings_page_, &SettingsPage::clipboard_monitoring_toggled, this, [this](bool enabled) {
        if (clipboard_monitor_) {
            clipboard_monitor_->set_enabled(enabled);
        }
    });
    connect(settings_page_, &SettingsPage::settings_changed, this, [this]() {
        save_settings();
        apply_settings_to_runtime();
    });
    connect(settings_page_, &SettingsPage::theme_toggle_requested, this, &MainWindow::on_theme_toggle_requested);
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

void MainWindow::load_settings()
{
    if (!settings_page_) {
        return;
    }

    QSettings settings;
    settings.beginGroup("desktop");

    settings_page_->set_clipboard_monitoring_enabled(
        settings.value("clipboard_monitoring_enabled", false).toBool());
    settings_page_->set_clipboard_detection_delay(
        settings.value("clipboard_detection_delay_ms", 1000).toInt());
    settings_page_->set_default_download_dir(
        settings.value("default_download_dir", QDir::homePath() + "/Downloads").toString());
    settings_page_->set_max_concurrent_downloads(
        settings.value("max_concurrent_downloads", 3).toInt());
    settings_page_->set_default_connections(
        settings.value("default_connections", 4).toInt());
    settings_page_->set_notifications_enabled(
        settings.value("notifications_enabled", true).toBool());
    settings_page_->set_sound_notifications_enabled(
        settings.value("sound_notifications_enabled", false).toBool());

    settings.endGroup();
}

void MainWindow::save_settings() const
{
    if (!settings_page_) {
        return;
    }

    QSettings settings;
    settings.beginGroup("desktop");
    settings.setValue("clipboard_monitoring_enabled", settings_page_->is_clipboard_monitoring_enabled());
    settings.setValue("clipboard_detection_delay_ms", settings_page_->get_clipboard_detection_delay());
    settings.setValue("default_download_dir", settings_page_->get_default_download_dir());
    settings.setValue("max_concurrent_downloads", settings_page_->get_max_concurrent_downloads());
    settings.setValue("default_connections", settings_page_->get_default_connections());
    settings.setValue("connection_timeout_seconds", settings_page_->get_connection_timeout());
    settings.setValue("retry_count", settings_page_->get_retry_count());
    settings.setValue("notifications_enabled", settings_page_->is_notifications_enabled());
    settings.setValue("sound_notifications_enabled", settings_page_->is_sound_notifications_enabled());
    settings.endGroup();
    settings.sync();
}

void MainWindow::apply_settings_to_runtime()
{
    if (!settings_page_) {
        return;
    }

    if (clipboard_monitor_) {
        clipboard_monitor_->set_detection_delay(settings_page_->get_clipboard_detection_delay());
        clipboard_monitor_->set_enabled(settings_page_->is_clipboard_monitoring_enabled());
    }

    if (download_engine_) {
        download_engine_->set_max_concurrent_tasks(
            static_cast<std::size_t>(settings_page_->get_max_concurrent_downloads()));
    }
}

void MainWindow::setup_ipc_server()
{
    ipc_server_ = new HttpIpcServer(this);
    connect(ipc_server_, &HttpIpcServer::download_requested, this, &MainWindow::on_download_requested);
    ipc_server_->start(kIpcPort);
}

void MainWindow::setup_system_tray()
{
    // 检查系统是否支持托盘
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    // 创建托盘图标
    system_tray_ = new QSystemTrayIcon(this);

    // 设置托盘图标（使用内置图标或自定义图标）
    // 这里使用标准图标作为示例，实际应该使用应用图标
    QIcon tray_icon = style()->standardIcon(QStyle::SP_ComputerIcon);
    system_tray_->setIcon(tray_icon);

    // 创建托盘菜单
    tray_menu_ = new QMenu(this);

    auto* show_action = tray_menu_->addAction(tr("显示主窗口"));
    connect(show_action, &QAction::triggered, this, &MainWindow::on_tray_show_clicked);

    tray_menu_->addSeparator();

    auto* quit_action = tray_menu_->addAction(tr("退出"));
    connect(quit_action, &QAction::triggered, this, &MainWindow::on_tray_quit_clicked);

    system_tray_->setContextMenu(tray_menu_);

    // 连接托盘激活信号
    connect(system_tray_, &QSystemTrayIcon::activated, this, &MainWindow::on_tray_activated);

    // 显示托盘图标
    system_tray_->show();

    // 设置托盘提示
    system_tray_->setToolTip(tr("Falcon 下载器"));
}

void MainWindow::on_tray_activated(QSystemTrayIcon::ActivationReason reason)
{
    switch (reason) {
        case QSystemTrayIcon::Trigger:
        case QSystemTrayIcon::DoubleClick:
            // 单击或双击托盘图标，切换窗口可见性
            if (isVisible()) {
                hide();
            } else {
                showNormal();
                activateWindow();
                raise();
            }
            break;
        case QSystemTrayIcon::MiddleClick:
            // 中键点击显示主窗口
            showNormal();
            activateWindow();
            raise();
            break;
        default:
            break;
    }
}

void MainWindow::on_tray_show_clicked()
{
    showNormal();
    activateWindow();
    raise();
}

void MainWindow::on_tray_quit_clicked()
{
    // 保存设置后退出
    save_settings();
    QApplication::quit();
}

void MainWindow::on_theme_toggle_requested()
{
    if (!theme_manager_) {
        return;
    }

    // 切换主题
    theme_manager_->toggle_theme();

    // 保存主题设置
    QSettings settings;
    settings.beginGroup("desktop");
    settings.setValue("theme", theme_manager_->current_theme() == ThemeType::Dark ? "dark" : "light");
    settings.endGroup();
    settings.sync();
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

void MainWindow::on_new_task_requested()
{
    const QString url = QInputDialog::getText(
        this,
        tr("New Download Task"),
        tr("Enter download URL (HTTP/HTTPS/Magnet):"),
        QLineEdit::Normal
    ).trimmed();

    if (url.isEmpty()) {
        return;
    }

    open_url(url);
}

void MainWindow::on_configured_download_requested(const QString& url)
{
    open_url(url);
}

void MainWindow::on_direct_download_requested(const QString& url, bool start_immediately)
{
    if (!add_download_task(url, start_immediately)) {
        QMessageBox::warning(this, tr("Download"), tr("URL is not supported.\n%1").arg(url));
    }
}

void MainWindow::on_remove_task_requested(falcon::TaskId id)
{
    if (!download_engine_) {
        return;
    }

    (void)download_engine_->remove_task(id);
}

void MainWindow::on_remove_finished_tasks_requested()
{
    if (!download_engine_) {
        return;
    }

    (void)download_engine_->remove_finished_tasks();
}

void MainWindow::on_minimize_requested()
{
    showMinimized();
}

void MainWindow::on_maximize_requested()
{
    if (isMaximized()) {
        showNormal();
    } else {
        showMaximized();
    }
}

void MainWindow::on_close_requested()
{
    // 如果系统托盘可用，最小化到托盘而不是关闭
    if (system_tray_ && system_tray_->isVisible()) {
        hide();
    } else {
        close();
    }
}

} // namespace falcon::desktop
