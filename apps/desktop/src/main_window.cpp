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
#include "pages/device_pages.hpp"
#include "pages/social_pages.hpp"
#include "dialogs/add_download_dialog.hpp"
#include "dialogs/flow_dialogs.hpp"
#include "utils/clipboard_monitor.hpp"
#include "utils/url_detector.hpp"
#include "utils/theme_manager.hpp"
#include "ipc/http_server.hpp"
#include "services/download_service.hpp"
#include "services/download_backend.hpp"
#include "services/task_order.hpp"
#include "services/local_store.hpp"
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
    // 从设置加载主题家族与模式（模式缺省回落旧 theme 键，兼容既有安装；
    // 先家族后模式——家族切换清缓存重渲，模式再解析亮暗）
    QSettings settings;
    settings.beginGroup("desktop");
    const QString family_str = settings.value("theme_family", "warm").toString();
    const QString mode_str = settings.value("theme_mode",
                                            settings.value("theme", "light")).toString();
    settings.endGroup();
    theme_manager_->set_family(family_str == "cold" ? ThemeFamily::Cold
                                : (family_str == "graphite" ? ThemeFamily::Graphite
                                                            : ThemeFamily::Warm));
    theme_manager_->set_theme_mode(
        mode_str == "system" ? ThemeMode::System
        : (mode_str == "dark" ? ThemeMode::Dark : ThemeMode::Light));

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
            this, [this](falcon::TaskId id, const QString& output_path,
                        const QString& stop_reason) {
                Q_UNUSED(id);
                if (!settings_page_ || !settings_page_->is_notifications_enabled()) {
                    return;
                }
                if (system_tray_ && system_tray_->isVisible()) {
                    QString reason_text;
                    if (stop_reason == QLatin1String("manual")) {
                        reason_text = tr("做种已手动停止。");
                    } else if (stop_reason == QLatin1String("limit_reached")) {
                        reason_text = tr("做种已达份额/时长上限，自动停止。");
                    } else {
                        reason_text = tr("做种已结束（达标或手动停止）。");
                    }
                    system_tray_->showMessage(
                        tr("做种已停止"),
                        tr("%1\n%2").arg(output_path, reason_text),
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

// ============================================================
// ============================================================
// ui-next 弹层入口（本地闭环：数据落 LocalStore）
// ============================================================
void MainWindow::refresh_proto_pages()
{
    // 通过 findChild 按类型刷新，避免在头文件新增 8 个成员指针
    if (auto* p = findChild<proto::CrossDevicePage*>()) p->refresh();
    if (auto* p = findChild<proto::NasNodesPage*>()) p->refresh();
    if (auto* p = findChild<proto::AirdropPage*>()) p->refresh();
    if (auto* p = findChild<proto::NasWebPage*>()) p->refresh();
    if (auto* p = findChild<proto::AccountPage*>()) p->refresh();
    if (auto* p = findChild<proto::FriendsPage*>()) p->refresh();
    if (auto* p = findChild<proto::ShareGroupsPage*>()) p->refresh();
    if (auto* p = findChild<proto::ShareRecordsPage*>()) p->refresh();
}

void MainWindow::open_show_code_dialog()
{
    proto::ShowCodeDialog dialog(this);
    dialog.exec();
}

void MainWindow::open_pair_code_dialog()
{
    proto::PairCodeDialog dialog(this);
    connect(&dialog, &proto::PairCodeDialog::connect_requested, this,
            [this](const QString& code) {
                // 码已由弹层校验通过；此处落库为配对设备，并作废该码
                LocalStore& store = LocalStore::instance();
                if (!store.consume_code(code)) {
                    QMessageBox::warning(this, tr("匹配码配对"), tr("码已过期，请重新生成。"));
                    return;
                }
                PairedDevice device;
                device.name = tr("配对设备 %1").arg(code);
                device.platform = tr("本机演示 · 待后端识别");
                device.code = code;
                store.add_paired_device(device);

                // 同时生成一条好友请求，好友页的「接受 / 忽略」即可真实操作
                FriendRequest request;
                request.name = device.name;
                request.message = tr("通过匹配码请求建立可信关系");
                store.add_friend_request(request);

                refresh_proto_pages();
                QMessageBox::information(this, tr("匹配码配对"),
                                         tr("配对成功：设备已加入「跨设备 → 我的设备」，"
                                            "并生成一条好友请求待处理。"
                                            "跨设备的实际数据同步需后端接入。"));
            });
    dialog.exec();
}

void MainWindow::open_add_node_dialog()
{
    proto::AddNodeDialog dialog(this);
    connect(&dialog, &proto::AddNodeDialog::test_connection_requested, this,
            [this](const proto::NodeDraft& draft) {
                // 本地参数校验：协议白名单 + endpoint 形制；真实网络探测需后端
                const QStringList allowed = {"WebDAV", "SMB", "SMB3", "SFTP", "FTPS", "S3", "Samba", "Aria2"};
                const bool protocol_ok = allowed.contains(draft.protocol, Qt::CaseInsensitive);
                const bool endpoint_ok = draft.endpoint.contains(QStringLiteral("://"));
                if (protocol_ok && endpoint_ok) {
                    QMessageBox::information(this, tr("测试连接"),
                                             tr("参数校验通过：%1 / %2。实际连通性探测需后端接入。")
                                                 .arg(draft.protocol, draft.endpoint));
                } else {
                    QMessageBox::warning(this, tr("测试连接"),
                                         tr("参数不合法：协议需在白名单内，Endpoint 需含协议前缀（如 https://）。"));
                }
            });
    connect(&dialog, &proto::AddNodeDialog::save_requested, this,
            [this](const proto::NodeDraft& draft) {
                NasNode node;
                node.name = draft.name;
                node.protocol = draft.protocol;
                node.endpoint = draft.endpoint;
                node.access_key = draft.access_key;
                node.secret_key = draft.secret_key;
                node.region = draft.region;
                node.capabilities = QStringList() << draft.protocol;
                LocalStore::instance().add_nas_node(node);
                refresh_proto_pages();
                QMessageBox::information(this, tr("保存配置"),
                                         tr("节点「%1」已保存到本机。").arg(draft.name));
            });
    dialog.exec();
}

void MainWindow::open_login_dialog()
{
    proto::LoginDialog dialog(this);
    connect(&dialog, &proto::LoginDialog::login_succeeded, this, [this](const QString&) {
        refresh_proto_pages();
    });
    dialog.exec();
}

void MainWindow::open_edit_profile_dialog()
{
    proto::EditProfileDialog dialog(this);
    dialog.exec();
    refresh_proto_pages();
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

    // ui-next 新增页面导航
    connect(side_bar_, &SideBar::crossDeviceClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_CROSS_DEVICE);
    });
    connect(side_bar_, &SideBar::nasNodesClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_NAS_NODES);
    });
    connect(side_bar_, &SideBar::airdropClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_AIRDROP);
    });
    connect(side_bar_, &SideBar::nasWebClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_NAS_WEB);
    });
    connect(side_bar_, &SideBar::accountClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_ACCOUNT);
    });
    connect(side_bar_, &SideBar::friendsClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_FRIENDS);
    });
    connect(side_bar_, &SideBar::shareGroupsClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_SHARE_GROUPS);
    });
    connect(side_bar_, &SideBar::shareRecordsClicked, this, [this]() {
        content_stack_->setCurrentIndex(PAGE_SHARE_RECORDS);
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
    connect(download_page_, &DownloadPage::redownload_requested,
            this, &MainWindow::on_redownload_requested);
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
    connect(discovery_page, &DiscoveryPage::manage_engines_requested,
            this, [this]() { content_stack_->setCurrentIndex(PAGE_SETTINGS); });

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
    // 设置页三级切换：模式（浅色/深色/跟随系统）与家族（三家族）分别
    // 落盘热生效（沿浮窗/双击行为范式，不等「应用」按钮）
    connect(settings_page_, &SettingsPage::theme_mode_changed, this, &MainWindow::on_theme_mode_changed);
    connect(settings_page_, &SettingsPage::theme_family_changed, this, &MainWindow::on_theme_family_changed);
    // 设置页下拉框 → 下载页即时生效（经 set_display_style 回发
    // display_style_changed 完成落盘，程序化回写有 QSignalBlocker 不回环）
    connect(settings_page_, &SettingsPage::task_view_grid_changed, this,
            [this](bool grid_view) {
                if (download_page_) {
                    download_page_->set_display_style(
                        grid_view ? TaskDisplayStyle::Grid : TaskDisplayStyle::Table);
                }
            });
    // B18: 「已完成双击行为」即时落盘（沿 clipboard/浮窗范式）；下载页在
    // 双击事件时即时读取 QSettings，无需向下载页回写
    connect(settings_page_, &SettingsPage::completed_double_click_changed, this,
            [this](int action) {
                QSettings settings;
                settings.beginGroup("desktop");
                settings.setValue("completed_double_click_action", action);
                settings.endGroup();
            });
    if (theme_manager_) {
        settings_page_->set_theme_display(theme_manager_->current_theme() == ThemeType::Dark);
        settings_page_->set_theme_mode_display(static_cast<int>(theme_manager_->mode()));
        settings_page_->set_theme_family_display(static_cast<int>(theme_manager_->family()));
        connect(theme_manager_, &ThemeManager::theme_changed, settings_page_, [this](ThemeType theme) {
            if (settings_page_) {
                settings_page_->set_theme_display(theme == ThemeType::Dark);
            }
        });
        // 家族切换回写设置页下拉（模式下拉不随 theme_changed 移动——
        // 模式反映用户选择，跟随系统下解析出的亮暗不动下拉位置）
        connect(theme_manager_, &ThemeManager::family_changed, settings_page_, [this](ThemeFamily family) {
            if (settings_page_) {
                settings_page_->set_theme_family_display(static_cast<int>(family));
            }
        });
    }

    // ============================================================
    // ui-next 新增页面（addWidget 顺序须与 PageIndex 枚举一致）
    // ============================================================
    auto* cross_device_page = new proto::CrossDevicePage(this);
    content_stack_->addWidget(cross_device_page);
    connect(cross_device_page, &proto::CrossDevicePage::show_code_requested,
            this, [this]() { open_show_code_dialog(); });
    connect(cross_device_page, &proto::CrossDevicePage::pair_code_requested,
            this, [this]() { open_pair_code_dialog(); });
    connect(cross_device_page, &proto::CrossDevicePage::airdrop_requested,
            this, [this]() { content_stack_->setCurrentIndex(PAGE_AIRDROP); });

    auto* nas_nodes_page = new proto::NasNodesPage(this);
    content_stack_->addWidget(nas_nodes_page);
    connect(nas_nodes_page, &proto::NasNodesPage::add_node_requested,
            this, [this]() { open_add_node_dialog(); });

    auto* airdrop_page = new proto::AirdropPage(this);
    content_stack_->addWidget(airdrop_page);
    connect(airdrop_page, &proto::AirdropPage::show_code_requested,
            this, [this]() { open_show_code_dialog(); });
    connect(airdrop_page, &proto::AirdropPage::pair_code_requested,
            this, [this]() { open_pair_code_dialog(); });
    connect(airdrop_page, &proto::AirdropPage::share_records_requested,
            this, [this]() { content_stack_->setCurrentIndex(PAGE_SHARE_RECORDS); });

    auto* nas_web_page = new proto::NasWebPage(this);
    content_stack_->addWidget(nas_web_page);

    auto* account_page = new proto::AccountPage(this);
    content_stack_->addWidget(account_page);
    connect(account_page, &proto::AccountPage::edit_profile_requested,
            this, [this]() { open_edit_profile_dialog(); });
    connect(account_page, &proto::AccountPage::friends_requested,
            this, [this]() { content_stack_->setCurrentIndex(PAGE_FRIENDS); });
    connect(account_page, &proto::AccountPage::share_groups_requested,
            this, [this]() { content_stack_->setCurrentIndex(PAGE_SHARE_GROUPS); });
    connect(account_page, &proto::AccountPage::share_records_requested,
            this, [this]() { content_stack_->setCurrentIndex(PAGE_SHARE_RECORDS); });
    connect(account_page, &proto::AccountPage::login_requested,
            this, [this]() { open_login_dialog(); });

    auto* friends_page = new proto::FriendsPage(this);
    content_stack_->addWidget(friends_page);
    connect(friends_page, &proto::FriendsPage::pair_code_requested,
            this, [this]() { open_pair_code_dialog(); });

    auto* share_groups_page = new proto::ShareGroupsPage(this);
    content_stack_->addWidget(share_groups_page);

    auto* share_records_page = new proto::ShareRecordsPage(this);
    content_stack_->addWidget(share_records_page);

    // DownloadPage 右键新动作：文件详情弹层 / 添加到云盘切页
    connect(download_page_, &DownloadPage::file_details_requested, this,
            [this](const falcon::daemon::rpc::TaskSnapshot& snapshot) {
                proto::TaskDetailDialog dialog(snapshot, this);
                connect(&dialog, &proto::TaskDetailDialog::add_to_cloud_requested,
                        this, [this, &dialog]() {
                            dialog.accept();
                            content_stack_->setCurrentIndex(PAGE_CLOUD);
                        });
                dialog.exec();
            });
    connect(download_page_, &DownloadPage::add_to_cloud_requested, this,
            [this]() { content_stack_->setCurrentIndex(PAGE_CLOUD); });
}

} // namespace falcon::desktop
