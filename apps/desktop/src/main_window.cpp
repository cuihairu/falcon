/**
 * @file main_window.cpp
 * @brief 主窗口实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "main_window.hpp"
#include "widgets/top_bar.hpp"
#include "widgets/status_bar.hpp"
#include "widgets/speed_float_widget.hpp"
#include "navigation/sidebar.hpp"
#include "pages/download_page.hpp"
#include "pages/cloud_page.hpp"
#include "pages/discovery_page.hpp"
#include "pages/trash_page.hpp"
#include "pages/settings_page.hpp"
#include "dialogs/add_download_dialog.hpp"
#include "utils/clipboard_monitor.hpp"
#include "utils/url_detector.hpp"
#include "utils/theme_manager.hpp"
#include "ipc/http_server.hpp"
#include "services/download_service.hpp"
#include "services/download_backend.hpp"
#include "services/task_order.hpp"
#include "services/update_checker.hpp"
#include "utils/icon_utils.hpp"

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
#include <QScreen>
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
    , trash_page_(nullptr)
    , settings_page_(nullptr)
    , clipboard_monitor_(nullptr)
    , ipc_server_(nullptr)
    , system_tray_(nullptr)
    , tray_menu_(nullptr)
    , theme_manager_(nullptr)
    , speed_float_(nullptr)
    , download_service_(nullptr)
    , update_checker_(nullptr)
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
    setup_speed_float();
    setup_clipboard_monitor();
    setup_system_tray();
    load_settings();
    apply_settings_to_runtime();
    setup_ipc_server();
    ensure_download_service();
    check_for_updates_on_startup();
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
    // 否则维持进程内引擎直连。回收站仅进程内后端启用（目录为
    // <下载目录>/.falcon-trash）；daemon 路径保持旧行为（remove 即删）。
    std::unique_ptr<IDownloadBackend> backend;
    std::string trash_dir;
    if (settings_page_ && settings_page_->is_daemon_mode_enabled()) {
        falcon::daemon::rpc::JsonRpcClientConfig config;
        config.url = settings_page_->get_daemon_rpc_url().toStdString();
        config.secret = settings_page_->get_daemon_rpc_secret().toStdString();
        config.timeout_seconds = 5;
        backend = make_daemon_rpc_backend(std::move(config));
    } else {
        backend = make_inprocess_backend();
        if (settings_page_) {
            trash_dir = settings_page_->get_default_download_dir().toStdString()
                        + "/.falcon-trash";
        }
    }

    download_service_ = new DownloadService(std::move(backend), std::move(trash_dir), this);
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
    connect(download_service_, &DownloadService::seeding_stopped,
            this, [this](falcon::TaskId id, const QString& output_path) {
                Q_UNUSED(id);
                if (!settings_page_ || !settings_page_->is_notifications_enabled()) {
                    return;
                }
                if (system_tray_ && system_tray_->isVisible()) {
                    system_tray_->showMessage(
                        tr("做种已停止"),
                        tr("%1\n做种已结束（达标或手动停止）。").arg(output_path),
                        QSystemTrayIcon::Information,
                        3000);
                }
            });
    // 回收站内容变化（入站/恢复/清理）→ 全量重拉并刷新回收站页
    connect(download_service_, &DownloadService::trash_changed,
            this, [this]() {
                if (trash_page_) {
                    trash_page_->set_entries(download_service_->trash_list());
                }
            });
    // 首次填充：清单在 TrashStore 构造时已加载，等变更才有数据会空一屏
    if (trash_page_) {
        trash_page_->set_entries(download_service_->trash_list());
    }

    // 全局设置（并发数/限速/做种默认值/回收站保留天数）需在首次轮询前生效
    if (settings_page_) {
        download_service_->apply_global_settings(
            static_cast<std::size_t>(settings_page_->get_max_concurrent_downloads()),
            static_cast<std::size_t>(settings_page_->get_global_speed_limit()) * 1024);
        download_service_->apply_seed_defaults(
            settings_page_->get_seed_ratio(),
            static_cast<std::size_t>(settings_page_->get_seed_time_minutes()));
        download_service_->set_trash_retention_days(
            settings_page_->get_trash_retention_days());
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
        dialog.set_seed_defaults(settings_page_->get_seed_ratio(),
                                 static_cast<std::size_t>(settings_page_->get_seed_time_minutes()));
    }

    if (request_context) {
        dialog.set_request_referrer(request_context->referrer);
        dialog.set_request_user_agent(request_context->user_agent);
        dialog.set_request_cookies(request_context->cookies);
    }

    // 主窗可能隐藏在托盘（剪贴板检测弹窗场景）：模态 exec 前显式
    // show/raise/activateWindow 保证对话框落到前台并获得焦点——否则
    // 在部分窗口管理器下对话框藏在其他窗口后面，用户看不到"没反应"
    dialog.show();
    dialog.raise();
    dialog.activateWindow();

    const int exec_result = dialog.exec();
    if (exec_result != QDialog::Accepted) {
        return;
    }

    ensure_download_service();

    falcon::DownloadOptions options;
    options.max_connections = static_cast<std::size_t>(dialog.get_connections());
    options.output_directory = dialog.get_save_path().toStdString();
    options.output_filename = dialog.get_file_name().toStdString();
    options.user_agent = dialog.get_user_agent().toStdString();
    options.referer = dialog.get_referrer().toStdString();
    // 对话框路径显式携带做种策略（用户看到并确认的值，不被全局默认覆写）
    options.seed_ratio = dialog.get_seed_ratio();
    options.seed_time_minutes = dialog.get_seed_time_minutes();

    // 应用任务速度限制（KB/s -> bytes/s）+ 连接超时（停滞超时，aria2
    // --timeout 同语义）与重试次数——设置项此前保存却从不消费
    if (settings_page_) {
        const int task_limit_kb = settings_page_->get_task_speed_limit();
        options.speed_limit = static_cast<std::size_t>(task_limit_kb * 1024);
        options.timeout_seconds =
            static_cast<std::size_t>(settings_page_->get_connection_timeout());
        options.max_retries =
            static_cast<std::size_t>(settings_page_->get_retry_count());
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
    // 无对话框路径（剪贴板/扩展/发现页直达）：seed_ratio < 0 是桌面层
    // 「未显式设置」哨兵，后端按全局做种默认填充
    options.seed_ratio = -1.0;
    if (settings_page_) {
        options.max_connections = static_cast<std::size_t>(settings_page_->get_default_connections());
        options.output_directory = settings_page_->get_default_download_dir().toStdString();
        // 应用任务速度限制（KB/s -> bytes/s）+ 连接超时与重试次数
        const int task_limit_kb = settings_page_->get_task_speed_limit();
        options.speed_limit = static_cast<std::size_t>(task_limit_kb * 1024);
        options.timeout_seconds =
            static_cast<std::size_t>(settings_page_->get_connection_timeout());
        options.max_retries =
            static_cast<std::size_t>(settings_page_->get_retry_count());
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

    connect(side_bar_, &SideBar::trashClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_TRASH);
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
    connect(download_page_, &DownloadPage::stop_seeding_requested,
            this, [this](falcon::TaskId id) {
                if (download_service_) {
                    download_service_->stop_seeding(id);
                }
            });
    // 拖拽排序 → 立即落 QSettings（不等待 save_settings：排序是即时
    // 动作，退出时机不可控，且 save_settings 是 const 只回写设置页字段）
    connect(download_page_, &DownloadPage::task_order_changed,
            this, [this](const QString& serialized) {
                QSettings settings;
                settings.beginGroup("desktop");
                settings.setValue("task_order", serialized);
                settings.endGroup();
                settings.sync();
            });

    // 显示样式手动切换 → 立即落 QSettings（B15：记住选择，首次/无记录默认卡片）
    // 并同步设置页下拉框
    connect(download_page_, &DownloadPage::display_style_changed, this,
            [this](bool grid_view) {
                QSettings settings;
                settings.beginGroup("desktop");
                settings.setValue("task_display_style",
                                  grid_view ? QStringLiteral("grid")
                                            : QStringLiteral("table"));
                settings.endGroup();
                settings.sync();
                if (settings_page_) {
                    settings_page_->set_task_view_grid(grid_view);
                }
            });

    // 视图模式（下载中/已完成）变化 → 侧栏高亮跟随（B20 双向联动的
    // 页签 → 侧栏方向；反向走 downloadingTabClicked/completedTabClicked
    // 既有接线。set_active_download_tab 内 QSignalBlocker 防回环）
    connect(download_page_, &DownloadPage::view_mode_changed, this,
            [this](DownloadViewMode mode) {
                if (side_bar_) {
                    side_bar_->set_active_download_tab(mode == DownloadViewMode::Completed);
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

    // 回收站页面（操作转发 DownloadService，trash_changed 回来后刷新）
    trash_page_ = new TrashPage(this);
    content_stack_->addWidget(trash_page_);
    connect(trash_page_, &TrashPage::restore_requested,
            this, [this](std::uint64_t id) {
                if (download_service_) {
                    download_service_->trash_restore(id);
                }
            });
    connect(trash_page_, &TrashPage::purge_requested,
            this, [this](std::uint64_t id) {
                if (download_service_) {
                    download_service_->trash_purge(id);
                }
            });
    connect(trash_page_, &TrashPage::clear_requested,
            this, [this]() {
                if (download_service_) {
                    download_service_->trash_clear();
                }
            });

    // 设置页面
    settings_page_ = new SettingsPage(this);
    content_stack_->addWidget(settings_page_);

    // 连接设置页面信号
    connect(settings_page_, &SettingsPage::clipboard_monitoring_toggled, this, [this](bool enabled) {
        if (clipboard_monitor_) {
            clipboard_monitor_->set_enabled(enabled);
        }
        // 即时落盘（沿悬浮窗位置范式）：开关是即时动作，不等「应用」按钮
        // 与退出兜底——用户勾选后直接关机也不丢
        QSettings settings;
        settings.beginGroup("desktop");
        settings.setValue("clipboard_monitoring_enabled", enabled);
        settings.endGroup();
    });
    // B21③: 设置页「显示悬浮速度窗」开关即时生效 + 即时落盘（沿
    // clipboard 范式——勾选后直接关机也不丢，右键「关闭浮窗」也走此
    // 处理器统一收口：回写复选框 → toggled → 显隐 + 落盘）
    connect(settings_page_, &SettingsPage::float_widget_toggled, this,
            [this](bool enabled) {
                if (speed_float_) {
                    speed_float_->setVisible(enabled);
                }
                QSettings settings;
                settings.beginGroup("desktop");
                settings.setValue("float_widget_enabled", enabled);
                settings.endGroup();
            });
    connect(settings_page_, &SettingsPage::settings_changed, this, [this]() {
        save_settings();
        apply_settings_to_runtime();
    });
    connect(settings_page_, &SettingsPage::theme_toggle_requested, this, &MainWindow::on_theme_toggle_requested);
    // 设置页下拉框 → 下载页即时生效（经 set_display_style 回发
    // display_style_changed 完成落盘，程序化回写有 QSignalBlocker 不回环）
    connect(settings_page_, &SettingsPage::task_view_grid_changed, this,
            [this](bool grid_view) {
                if (download_page_) {
                    download_page_->set_display_style(
                        grid_view ? TaskDisplayStyle::Grid : TaskDisplayStyle::Table);
                }
            });
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

void MainWindow::setup_speed_float()
{
    speed_float_ = new SpeedFloatWidget(nullptr); // 独立顶层悬浮窗
    speed_float_->apply_theme(theme_manager_->current_theme());

    // 拖动结束即持久化位置（不等设置页"应用"——位置是即时的空间状态）
    connect(speed_float_, &SpeedFloatWidget::position_changed, this,
            [this](const QPoint& pos) {
                QSettings settings;
                settings.beginGroup("desktop");
                settings.setValue("float_pos_x", pos.x());
                settings.setValue("float_pos_y", pos.y());
                settings.endGroup();
            });

    connect(theme_manager_, &ThemeManager::theme_changed, speed_float_,
            &SpeedFloatWidget::apply_theme);

    // 浮窗按钮/右键菜单的暂停/继续请求 → 下载服务（与下载页行内按钮同路径）
    connect(speed_float_, &SpeedFloatWidget::pause_resume_requested, this,
            [this](std::uint64_t task_id, bool pause) {
                if (!download_service_) {
                    return;
                }
                const falcon::TaskId id = static_cast<falcon::TaskId>(task_id);
                if (pause) {
                    download_service_->pause_task(id);
                } else {
                    download_service_->resume_task(id);
                }
            });

    // B21③: 右键「关闭浮窗」= 关闭并持久化隐藏。回写设置页复选框
    // （setChecked 触发 toggled → float_widget_toggled 处理器统一收口
    // 显隐 + 落盘，无第二路径）
    connect(speed_float_, &SpeedFloatWidget::close_requested, this, [this]() {
        if (settings_page_) {
            settings_page_->set_float_widget_enabled(false);
        } else if (speed_float_) {
            speed_float_->setVisible(false);
        }
    });

    // 恢复上次位置；无记录默认停靠主屏右下角
    QSettings settings;
    settings.beginGroup("desktop");
    const int x = settings.value("float_pos_x", -1).toInt();
    const int y = settings.value("float_pos_y", -1).toInt();
    settings.endGroup();
    if (x >= 0 && y >= 0) {
        speed_float_->move(x, y);
    } else if (const QScreen* screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        speed_float_->move(available.right() - speed_float_->width() - 24,
                           available.bottom() - speed_float_->height() - 48);
    }
}

void MainWindow::load_settings()
{
    if (!settings_page_) {
        return;
    }

    QSettings settings;
    settings.beginGroup("desktop");

    // 剪贴板监听默认开启（主流下载器语义）：首次启动（无持久化记录）
    // 即生效；用户显式关闭后按持久化值保持关闭
    settings_page_->set_clipboard_monitoring_enabled(
        settings.value("clipboard_monitoring_enabled", true).toBool());
    settings_page_->set_clipboard_detection_delay(
        settings.value("clipboard_detection_delay_ms", 1000).toInt());
    settings_page_->set_default_download_dir(
        settings.value("default_download_dir", QDir::homePath() + "/Downloads").toString());
    settings_page_->set_max_concurrent_downloads(
        settings.value("max_concurrent_downloads", 3).toInt());
    settings_page_->set_default_connections(
        settings.value("default_connections", 4).toInt());
    settings_page_->set_connection_timeout(
        settings.value("connection_timeout_seconds", 30).toInt());
    settings_page_->set_retry_count(
        settings.value("retry_count", 3).toInt());
    settings_page_->set_task_speed_limit(
        settings.value("task_speed_limit_kb", 0).toInt());
    settings_page_->set_global_speed_limit(
        settings.value("global_speed_limit_kb", 0).toInt());
    settings_page_->set_seed_ratio(
        settings.value("seed_ratio", 1.0).toDouble());
    settings_page_->set_seed_time_minutes(
        settings.value("seed_time_minutes", 0).toInt());
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
    settings_page_->set_check_updates_on_startup_enabled(
        settings.value("check_updates_on_startup", true).toBool());
    settings_page_->set_trash_retention_days(
        settings.value("trash_retention_days", 7).toInt());
    settings_page_->set_float_widget_enabled(
        settings.value("float_widget_enabled", true).toBool());
    settings_page_->set_float_show_active_tasks(
        settings.value("float_show_active_tasks", true).toBool());
    settings_page_->set_float_show_total_progress(
        settings.value("float_show_total_progress", true).toBool());
    settings_page_->set_float_size_preset(
        settings.value("float_size_preset", 0).toInt()); // B21①: 默认紧凑档
    settings_page_->set_float_opacity_percent(
        settings.value("float_opacity_percent", 90).toInt());
    settings_page_->set_float_click_through(
        settings.value("float_click_through", false).toBool());
    // 手动排序（"3,1,2" 逗号串；无记录/全垃圾 → 空序 = id 升序默认行为）
    const QString task_order_text = settings.value("task_order", QString()).toString();
    // 任务列表视图：首次/无记录默认卡片（grid）；"table" = 用户手动切过列表
    const bool task_view_grid =
        settings.value("task_display_style", QStringLiteral("grid")).toString() !=
        QStringLiteral("table");

    settings.endGroup();

    if (download_page_) {
        download_page_->set_task_order(
            task_order::deserialize(task_order_text.toStdString()));
        download_page_->set_display_style(
            task_view_grid ? TaskDisplayStyle::Grid : TaskDisplayStyle::Table);
    }
    if (settings_page_) {
        settings_page_->set_task_view_grid(task_view_grid);
    }
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
    settings.setValue("seed_ratio", settings_page_->get_seed_ratio());
    settings.setValue("seed_time_minutes", settings_page_->get_seed_time_minutes());
    settings.setValue("action_when_completed", settings_page_->get_action_when_completed());
    settings.setValue("notifications_enabled", settings_page_->is_notifications_enabled());
    settings.setValue("sound_notifications_enabled", settings_page_->is_sound_notifications_enabled());
    settings.setValue("daemon_mode_enabled", settings_page_->is_daemon_mode_enabled());
    settings.setValue("daemon_rpc_url", settings_page_->get_daemon_rpc_url());
    settings.setValue("daemon_rpc_secret", settings_page_->get_daemon_rpc_secret());
    settings.setValue("check_updates_on_startup",
                      settings_page_->is_check_updates_on_startup_enabled());
    settings.setValue("trash_retention_days",
                      settings_page_->get_trash_retention_days());
    settings.setValue("float_widget_enabled",
                      settings_page_->is_float_widget_enabled());
    settings.setValue("float_show_active_tasks",
                      settings_page_->is_float_show_active_tasks());
    settings.setValue("float_show_total_progress",
                      settings_page_->is_float_show_total_progress());
    settings.setValue("float_size_preset",
                      settings_page_->get_float_size_preset());
    settings.setValue("float_opacity_percent",
                      settings_page_->get_float_opacity_percent());
    settings.setValue("float_click_through",
                      settings_page_->is_float_click_through());
    settings.endGroup();
    settings.sync();
}

void MainWindow::check_for_updates_on_startup()
{
    if (!settings_page_ || !settings_page_->is_check_updates_on_startup_enabled()) {
        return;
    }

    update_checker_ = new UpdateChecker(this);
    connect(update_checker_, &UpdateChecker::update_available, this,
            [this](const QString& latest, const QString& url) {
        // 启动静默检查仅托盘通知（发现页/设置页不打扰）；url 需用户
        // 自行前往设置页查看或访问 GitHub 发布页，不接入 messageClicked
        Q_UNUSED(url);
        if (system_tray_ && system_tray_->isVisible()) {
            system_tray_->showMessage(
                tr("发现新版本"),
                tr("Falcon %1 已发布，可在设置页检查更新获取发布页链接。").arg(latest),
                QSystemTrayIcon::Information, 8000);
        }
    });
    // up_to_date / check_failed 在启动路径一律静默（网络不通等情况不打扰）

    update_checker_->check_for_updates();
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
        download_service_->apply_seed_defaults(
            settings_page_->get_seed_ratio(),
            static_cast<std::size_t>(settings_page_->get_seed_time_minutes()));
        download_service_->set_trash_retention_days(
            settings_page_->get_trash_retention_days());
    }

    if (speed_float_) {
        speed_float_->setVisible(settings_page_->is_float_widget_enabled());
        speed_float_->set_show_active_tasks(
            settings_page_->is_float_show_active_tasks());
        speed_float_->set_show_total_progress(
            settings_page_->is_float_show_total_progress());
        speed_float_->set_size_preset(
            static_cast<SpeedFloatWidget::SizePreset>(
                settings_page_->get_float_size_preset()));
        speed_float_->set_opacity_percent(
            settings_page_->get_float_opacity_percent());
        speed_float_->set_click_through(
            settings_page_->is_float_click_through());
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

    // 设置托盘图标：应用 logo（qrc 内联 SVG，主题引擎按需缩放）
    system_tray_->setIcon(QIcon(":/icons/falcon.svg"));

    // 创建托盘菜单
    tray_menu_ = new QMenu(this);

    auto* show_action = tray_menu_->addAction(tr("显示主窗口"));
    show_action->setIcon(QIcon(":/icons/falcon.svg"));
    connect(show_action, &QAction::triggered, this, &MainWindow::on_tray_show_clicked);

    tray_menu_->addSeparator();

    auto* quit_action = tray_menu_->addAction(tr("退出"));
    quit_action->setIcon(icons::themed(icons::Id::X));
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
    // B21③: 托盘退出显式收窗——悬浮窗是独立顶层窗口，显式 close 保证
    // 与主窗同步消失不残留（QApplication::quit 只是退出事件循环）
    if (speed_float_) {
        speed_float_->close();
    }
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

    // 进程内后端走回收站（文件暂存可恢复）；daemon 后端保持旧行为
    if (download_service_->trash_available()) {
        download_service_->remove_task_to_trash(id);
    } else {
        download_service_->remove_task(id);
    }
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

    // 悬浮速度窗预览任务:第一个 Downloading,否则第一个 Paused
    // (B21: 标题行显示任务名 + 暂停/继续钮;无活动/暂停任务时 has_task=false)
    if (speed_float_) {
        SpeedFloatWidget::TaskPreview preview;
        for (const auto& snap : tasks) {
            if (snap.status != falcon::TaskStatus::Downloading
                && snap.status != falcon::TaskStatus::Paused) {
                continue;
            }
            preview.has_task = true;
            preview.task_id = static_cast<std::uint64_t>(snap.id);
            preview.running = snap.status == falcon::TaskStatus::Downloading;
            preview.url = QString::fromStdString(snap.url);
            const QFileInfo file_info(
                QString::fromStdString(snap.output_path));
            preview.file_name = file_info.fileName();
            preview.directory = file_info.absolutePath();
            preview.progress = snap.progress;
            break;
        }
        speed_float_->update_task_preview(preview);
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

    // 悬浮速度窗:速度/活跃任务数取自统计快照;总进度 = 活跃任务平均进度
    // (来自最近一轮任务快照,同一刷新周期内最多滞后半拍)
    if (speed_float_) {
        SpeedFloatWidget::Stats fs;
        fs.download_speed = static_cast<std::uint64_t>(stats.download_speed);
        fs.active_tasks = static_cast<int>(stats.active_tasks);
        int downloading = 0;
        double progress_sum = 0.0;
        for (const auto& snap : latest_task_snapshots_) {
            if (snap.status == falcon::TaskStatus::Downloading) {
                ++downloading;
                progress_sum += snap.progress;
            }
        }
        if (downloading > 0) {
            fs.overall_progress = progress_sum / downloading;
        }
        speed_float_->update_stats(fs);
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
