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
#include "services/download_service.hpp"
#include "services/download_backend.hpp"

#include <QHBoxLayout>
#include <QWidget>
#include <QApplication>
#include <QEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QDir>
#include <QDesktopServices>
#include <QFileInfo>
#include <QMessageBox>
#include <QSettings>
#include <QAction>
#include <QMenu>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace falcon::desktop {

namespace {
constexpr quint16 kIpcPort = 51337;

/** TaskStatus → 扩展任务面板使用的 aria2 风格状态串 */
QByteArray ipc_status_text(falcon::TaskStatus status)
{
    switch (status) {
    case falcon::TaskStatus::Pending:
    case falcon::TaskStatus::Preparing:
        return "waiting";
    case falcon::TaskStatus::Downloading:
        return "active";
    case falcon::TaskStatus::Paused:
        return "paused";
    case falcon::TaskStatus::Completed:
        return "complete";
    case falcon::TaskStatus::Failed:
        return "error";
    case falcon::TaskStatus::Cancelled:
        return "removed";
    }
    return "unknown";
}

/** 序列化任务快照为 /v1/tasks 响应体（Qt JSON，含转义处理） */
QByteArray build_ipc_tasks_json(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks)
{
    QJsonArray arr;
    for (const auto& snap : tasks) {
        QJsonObject o;
        o.insert("id", static_cast<qint64>(snap.id));
        o.insert("url", QString::fromStdString(snap.url));
        o.insert("path", QString::fromStdString(snap.output_path));
        o.insert("status", QString::fromUtf8(ipc_status_text(snap.status)));
        o.insert("progress", snap.progress);
        o.insert("totalBytes", static_cast<qint64>(snap.total_bytes));
        o.insert("downloadedBytes", static_cast<qint64>(snap.downloaded_bytes));
        o.insert("speed", static_cast<qint64>(snap.speed));
        if (!snap.error_message.empty()) {
            o.insert("error", QString::fromStdString(snap.error_message));
        }
        arr.append(o);
    }
    QJsonObject root;
    root.insert("ok", true);
    root.insert("tasks", arr);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

/** 序列化全局统计为 /v1/stats 响应体 */
QByteArray build_ipc_stats_json(const falcon::daemon::rpc::GlobalStats& stats)
{
    QJsonObject root;
    root.insert("ok", true);
    root.insert("downloadSpeed", static_cast<qint64>(stats.download_speed));
    root.insert("activeTasks", static_cast<int>(stats.active_tasks));
    root.insert("waitingTasks", static_cast<int>(stats.waiting_tasks));
    root.insert("stoppedTasks", static_cast<int>(stats.stopped_tasks));
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

// 无边框窗口边缘缩放带宽度(像素)
constexpr int kResizeEdgeBand = 6;

/** 缩放边组合 → 对应的系统缩放光标形状 */
Qt::CursorShape cursor_shape_for_edges(Qt::Edges edges)
{
    if (edges == (Qt::TopEdge | Qt::LeftEdge)
            || edges == (Qt::BottomEdge | Qt::RightEdge)) {
        return Qt::SizeFDiagCursor;
    }
    if (edges == (Qt::TopEdge | Qt::RightEdge)
            || edges == (Qt::BottomEdge | Qt::LeftEdge)) {
        return Qt::SizeBDiagCursor;
    }
    if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
        return Qt::SizeHorCursor;
    }
    if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
        return Qt::SizeVerCursor;
    }
    return Qt::ArrowCursor;
}
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
    , download_service_(nullptr)
{
    // 初始化主题管理器（必须在 setup_ui 之前）
    theme_manager_ = new ThemeManager(this);
    // 从设置加载主题
    QSettings settings;
    settings.beginGroup("desktop");
    const QString theme_str = settings.value("theme", "light").toString();
    settings.endGroup();
    theme_manager_->set_theme(theme_str == "dark" ? ThemeType::Dark : ThemeType::Light);

    setup_ui();
    setup_clipboard_monitor();
    setup_system_tray();
    load_settings();
    apply_settings_to_runtime();
    setup_ipc_server();
    ensure_download_service();
}

MainWindow::~MainWindow()
{
    qApp->removeEventFilter(this);
    if (clipboard_monitor_) {
        clipboard_monitor_->stop();
    }
    if (ipc_server_) {
        ipc_server_->stop();
    }
    // DownloadService 是 QObject 子对象，但 worker 线程必须在窗口析构前停掉
    if (download_service_) {
        download_service_->stop();
    }
}

void MainWindow::setup_ui()
{
    // 启用自定义标题栏（无边框窗口）
    setWindowFlags(Qt::WindowType::FramelessWindowHint);

    resize(1200, 800);
    setMinimumSize(960, 640);

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

    // 无边框窗口边缘缩放必须监听 qApp 级事件（见 eventFilter 注释）
    qApp->installEventFilter(this);
}

void MainWindow::create_top_bar()
{
    top_bar_ = new TopBar(this);

    connect(top_bar_, &TopBar::minimizeClicked, this, &MainWindow::on_minimize_requested);
    connect(top_bar_, &TopBar::maximizeClicked, this, &MainWindow::on_maximize_requested);
    connect(top_bar_, &TopBar::closeClicked, this, &MainWindow::on_close_requested);

    // 搜索框回车 → 按文件名过滤下载页(表格/网格双视图共用)
    connect(top_bar_, &TopBar::searchRequested, this, [this](const QString& text) {
        if (download_page_) {
            download_page_->set_text_filter(text);
        }
    });

    // 视图切换 → 下载页表格/网格互切
    connect(top_bar_, &TopBar::viewToggleClicked, this, [this]() {
        if (download_page_) {
            download_page_->toggle_display_style();
        }
    });

    // 手动刷新:事件驱动下的兜底(用户感知数据陈旧时)
    connect(top_bar_, &TopBar::refreshClicked, this, [this]() {
        if (download_service_) {
            download_service_->request_refresh();
        }
    });

    // 视图切换钮仅对下载页有意义,其他页禁用(避免无效点击)
    // connect 移至 create_content_area:content_stack_ 在彼处才创建,
    // 此处传 nullptr 连接从未生效(Qt 报 invalid nullptr parameter)
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

void MainWindow::ensure_download_service()
{
    if (download_service_) {
        return;
    }

    // 按设置选择后端：daemon 模式经 JSON-RPC 访问 falcon-daemon，
    // 否则维持进程内引擎直连
    std::unique_ptr<IDownloadBackend> backend;
    if (settings_page_ && settings_page_->is_daemon_mode_enabled()) {
        falcon::daemon::rpc::JsonRpcClientConfig config;
        config.url = settings_page_->get_daemon_rpc_url().toStdString();
        config.secret = settings_page_->get_daemon_rpc_secret().toStdString();
        config.timeout_seconds = 5;
        backend = make_daemon_rpc_backend(std::move(config));
    } else {
        backend = make_inprocess_backend();
    }

    download_service_ = new DownloadService(std::move(backend), this);
    connect(download_service_, &DownloadService::tasks_refreshed,
            this, &MainWindow::on_tasks_refreshed);
    connect(download_service_, &DownloadService::stats_refreshed,
            this, &MainWindow::on_stats_refreshed);
    connect(download_service_, &DownloadService::task_add_failed,
            this, &MainWindow::on_task_add_failed);
    connect(download_service_, &DownloadService::task_completed,
            this, &MainWindow::on_task_completed);
    connect(download_service_, &DownloadService::task_failed,
            this, &MainWindow::on_task_failed);

    // 全局设置（并发数/限速）需在首次轮询前生效
    if (settings_page_) {
        download_service_->apply_global_settings(
            static_cast<std::size_t>(settings_page_->get_max_concurrent_downloads()),
            static_cast<std::size_t>(settings_page_->get_global_speed_limit()) * 1024);
    }

    // 500ms 仅作兜底轮询：daemon RPC 后端经 WebSocket 事件流收到通知
    // （任务状态变更/进度推送）会即时触发刷新，进程内后端保持周期驱动
    download_service_->start(500);
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

    ensure_download_service();

    falcon::DownloadOptions options;
    options.max_connections = static_cast<std::size_t>(dialog.get_connections());
    options.output_directory = dialog.get_save_path().toStdString();
    options.output_filename = dialog.get_file_name().toStdString();
    options.user_agent = dialog.get_user_agent().toStdString();
    options.referer = dialog.get_referrer().toStdString();

    // 应用任务速度限制（KB/s -> bytes/s）
    if (settings_page_) {
        const int task_limit_kb = settings_page_->get_task_speed_limit();
        options.speed_limit = static_cast<std::size_t>(task_limit_kb * 1024);
    }

    const QString cookies = dialog.get_cookies();
    if (!cookies.isEmpty()) {
        options.headers["Cookie"] = cookies.toStdString();
    }

    // 异步提交；被拒绝时经 task_add_failed 信号提示（成功不打扰——
    // 任务行即刻出现在列表里，旧的无样式"已添加"确认框只会盖住它）
    download_service_->add_task(dialog.get_url(), options, true);
}

bool MainWindow::add_download_task(const QString& url, bool start_immediately)
{
    ensure_download_service();

    falcon::DownloadOptions options;
    if (settings_page_) {
        options.max_connections = static_cast<std::size_t>(settings_page_->get_default_connections());
        options.output_directory = settings_page_->get_default_download_dir().toStdString();
        // 应用任务速度限制（KB/s -> bytes/s）
        const int task_limit_kb = settings_page_->get_task_speed_limit();
        options.speed_limit = static_cast<std::size_t>(task_limit_kb * 1024);
    }

    // 异步提交；被拒绝时经 task_add_failed 信号提示
    download_service_->add_task(url, options, start_immediately);
    return true;
}

void MainWindow::create_side_bar()
{
    side_bar_ = new SideBar(this);

    // 连接侧边栏信号到页面切换
    // 每个 tab 各自设置视图模式,不再经 downloadClicked 无条件重置
    // (旧实现双信号 + 连接顺序导致"已完成/云添加"被覆盖回"下载中")
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

    // 视图切换钮仅对下载页有意义,其他页禁用(避免无效点击);
    // 自 create_top_bar 移入——content_stack_ 在此才创建
    connect(content_stack_, &QStackedWidget::currentChanged, this, [this](int index) {
        if (top_bar_) {
            top_bar_->set_view_toggle_enabled(index == PAGE_DOWNLOAD);
        }
    });
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
    connect(download_page_, &DownloadPage::priority_changed,
            this, &MainWindow::on_priority_changed);
    connect(download_page_, &DownloadPage::pause_requested,
            this, [this](falcon::TaskId id) {
                if (download_service_) {
                    download_service_->pause_task(id);
                }
            });
    connect(download_page_, &DownloadPage::resume_requested,
            this, [this](falcon::TaskId id) {
                if (download_service_) {
                    download_service_->resume_task(id);
                }
            });

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
    if (theme_manager_) {
        settings_page_->set_theme_display(theme_manager_->current_theme() == ThemeType::Dark);
        connect(theme_manager_, &ThemeManager::theme_changed, settings_page_, [this](ThemeType theme) {
            if (settings_page_) {
                settings_page_->set_theme_display(theme == ThemeType::Dark);
            }
        });
    }
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
    settings_page_->set_task_speed_limit(
        settings.value("task_speed_limit_kb", 0).toInt());
    settings_page_->set_global_speed_limit(
        settings.value("global_speed_limit_kb", 0).toInt());
    settings_page_->set_action_when_completed(
        settings.value("action_when_completed", 0).toInt());
    settings_page_->set_notifications_enabled(
        settings.value("notifications_enabled", true).toBool());
    settings_page_->set_sound_notifications_enabled(
        settings.value("sound_notifications_enabled", false).toBool());
    settings_page_->set_daemon_mode_enabled(
        settings.value("daemon_mode_enabled", false).toBool());
    settings_page_->set_daemon_rpc_url(
        settings.value("daemon_rpc_url", "http://127.0.0.1:6800/jsonrpc").toString());
    settings_page_->set_daemon_rpc_secret(
        settings.value("daemon_rpc_secret", "").toString());

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
    settings.setValue("task_speed_limit_kb", settings_page_->get_task_speed_limit());
    settings.setValue("global_speed_limit_kb", settings_page_->get_global_speed_limit());
    settings.setValue("action_when_completed", settings_page_->get_action_when_completed());
    settings.setValue("notifications_enabled", settings_page_->is_notifications_enabled());
    settings.setValue("sound_notifications_enabled", settings_page_->is_sound_notifications_enabled());
    settings.setValue("daemon_mode_enabled", settings_page_->is_daemon_mode_enabled());
    settings.setValue("daemon_rpc_url", settings_page_->get_daemon_rpc_url());
    settings.setValue("daemon_rpc_secret", settings_page_->get_daemon_rpc_secret());
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

    if (download_service_) {
        download_service_->apply_global_settings(
            static_cast<std::size_t>(settings_page_->get_max_concurrent_downloads()),
            static_cast<std::size_t>(settings_page_->get_global_speed_limit()) * 1024);
    }
}

void MainWindow::setup_ipc_server()
{
    ipc_server_ = new HttpIpcServer(this);
    connect(ipc_server_, &HttpIpcServer::download_requested, this, &MainWindow::on_download_requested);
    // 只读查询端点的数据源（缓存由 on_tasks_refreshed/on_stats_refreshed 更新，
    // 两者都在 GUI 线程，与本服务器同线程，回调内直接读无并发问题）
    ipc_server_->set_tasks_provider([this]() {
        return build_ipc_tasks_json(latest_task_snapshots_);
    });
    ipc_server_->set_stats_provider([this]() {
        return build_ipc_stats_json(latest_stats_);
    });
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
    // 直接进 Fluent 添加对话框,URL 在对话框内输入(协议/文件名随输入
    // 联动);旧 QInputDialog 两段式路径是 UI 重做前的遗留,无主题样式
    show_add_download_dialog(UrlDetector::parse_url(QString()), nullptr);
}

void MainWindow::on_configured_download_requested(const QString& url)
{
    open_url(url);
}

void MainWindow::on_direct_download_requested(const QString& url, bool start_immediately)
{
    add_download_task(url, start_immediately);
}

void MainWindow::on_remove_task_requested(falcon::TaskId id)
{
    if (!download_service_) {
        return;
    }

    download_service_->remove_task(id);
}

void MainWindow::on_remove_finished_tasks_requested()
{
    if (!download_service_) {
        return;
    }

    download_service_->remove_finished_tasks();
}

void MainWindow::on_priority_changed(falcon::TaskId id, falcon::TaskPriority priority)
{
    if (!download_service_) {
        return;
    }

    download_service_->set_priority(id, priority);
}

//==============================================================================
// 无边框窗口:边缘缩放 + 最大化状态同步
//==============================================================================

void MainWindow::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::WindowStateChange && top_bar_) {
        // 最大化/还原切换顶栏按钮图标(Square ↔ Restore)
        top_bar_->set_maximized(isMaximized());
    }
    QMainWindow::changeEvent(event);
}

bool MainWindow::eventFilter(QObject* obj, QEvent* event)
{
    const QEvent::Type type = event->type();
    if (type == QEvent::MouseMove || type == QEvent::MouseButtonPress) {
        auto* widget = qobject_cast<QWidget*>(obj);
        // 只关心本窗口内的控件;菜单/tooltip/combo 弹窗是独立顶层窗口,
        // window() 比较天然排除。缩放仅在非最大化时生效。
        if (widget && widget->window() == static_cast<QWidget*>(this) && !isMaximized()) {
            const auto* mouse = static_cast<QMouseEvent*>(event);
            const Qt::Edges edges = resize_edge_for(mouse->globalPosition().toPoint());
            if (edges != 0) {
                if (type == QEvent::MouseButtonPress
                        && mouse->button() == Qt::LeftButton && windowHandle()) {
                    windowHandle()->startSystemResize(edges);
                    return true;
                }
                if (type == QEvent::MouseMove) {
                    setCursor(cursor_shape_for_edges(edges));
                    resize_cursor_active_ = true;
                    return false;
                }
            } else if (type == QEvent::MouseMove && resize_cursor_active_) {
                // 仅在曾设过缩放光标时恢复,避免覆盖子控件自己的光标
                // (如 QLineEdit 的 IBeam)
                setCursor(Qt::ArrowCursor);
                resize_cursor_active_ = false;
            }
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

Qt::Edges MainWindow::resize_edge_for(const QPoint& global_pos) const
{
    const QPoint local = mapFromGlobal(global_pos);
    const QRect frame = rect();

    Qt::Edges edges;
    if (local.x() <= kResizeEdgeBand) {
        edges |= Qt::LeftEdge;
    }
    if (local.x() >= frame.width() - kResizeEdgeBand) {
        edges |= Qt::RightEdge;
    }
    if (local.y() <= kResizeEdgeBand) {
        edges |= Qt::TopEdge;
    }
    if (local.y() >= frame.height() - kResizeEdgeBand) {
        edges |= Qt::BottomEdge;
    }
    return edges;
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

//==============================================================================
// DownloadService Events（信号已从 worker 线程排队投递到 GUI 线程）
//==============================================================================

void MainWindow::on_tasks_refreshed(const std::vector<falcon::daemon::rpc::TaskSnapshot>& tasks)
{
    // 浏览器扩展 IPC /v1/tasks 的数据源
    latest_task_snapshots_ = tasks;

    // 维护 id → URL 映射，供错误通知显示文件名
    task_url_by_id_.clear();
    task_url_by_id_.reserve(static_cast<int>(tasks.size()) * 2);
    for (const auto& snap : tasks) {
        task_url_by_id_.insert(static_cast<qulonglong>(snap.id),
                               QString::fromStdString(snap.url));
    }

    if (download_page_) {
        download_page_->update_tasks(tasks);
    }
}

void MainWindow::on_stats_refreshed(falcon::daemon::rpc::GlobalStats stats)
{
    // 浏览器扩展 IPC /v1/stats 的数据源
    latest_stats_ = stats;

    if (status_bar_) {
        status_bar_->set_download_speed(static_cast<uint64_t>(stats.download_speed));
        status_bar_->set_task_counts(static_cast<int>(stats.active_tasks),
                                     static_cast<int>(stats.stopped_tasks));
    }

    // 侧栏底部统计卡:真实活跃任务数(替代此前的硬编码假数据)
    if (side_bar_) {
        side_bar_->set_queue_count(static_cast<int>(stats.active_tasks));
    }
}

void MainWindow::on_task_add_failed(const QString& url, const QString& reason)
{
    QMessageBox::warning(this, tr("Download"),
                         tr("Failed to add download task:\n%1\n\n%2").arg(url, reason));
}

void MainWindow::on_task_failed(falcon::TaskId id, const QString& error_message)
{
    // 如果通知未启用，直接返回
    if (!settings_page_ || !settings_page_->is_notifications_enabled()) {
        return;
    }

    // 从最近快照提取文件名用于显示
    QString task_name = tr("Task #%1").arg(static_cast<qulonglong>(id));
    const auto it = task_url_by_id_.constFind(static_cast<qulonglong>(id));
    if (it != task_url_by_id_.constEnd() && !it.value().isEmpty()) {
        const QString& url = it.value();
        const int last_slash = url.lastIndexOf('/');
        if (last_slash >= 0 && last_slash < url.length() - 1) {
            task_name = url.mid(last_slash + 1);
        } else {
            task_name = url;
        }
    }

    // 使用系统托盘显示错误通知
    if (system_tray_ && system_tray_->isVisible()) {
        system_tray_->showMessage(
            tr("Download Error"),
            tr("%1\nError: %2").arg(task_name, error_message),
            QSystemTrayIcon::Critical,
            5000  // 显示 5 秒
        );
    }
}

void MainWindow::on_task_completed(falcon::TaskId id, const QString& output_path)
{
    (void)id;

    if (!settings_page_) {
        return;
    }

    const int action = settings_page_->get_action_when_completed();

    switch (action) {
        case 0:  // Do nothing
            break;

        case 1:  // Open file
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(output_path));
            break;
        }

        case 2:  // Open folder
        {
            const QFileInfo file_info(output_path);
            QDesktopServices::openUrl(QUrl::fromLocalFile(file_info.absolutePath()));
            break;
        }

        case 3:  // Show notification only
        {
            if (system_tray_ && settings_page_->is_notifications_enabled()) {
                system_tray_->showMessage(
                    tr("Download Completed"),
                    tr("A download has finished successfully."),
                    QSystemTrayIcon::Information,
                    3000
                );
            }
            break;
        }
    }
}

} // namespace falcon::desktop
