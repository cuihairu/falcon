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
#include "utils/clipboard_monitor.hpp"
#include "utils/theme_manager.hpp"
#include "ipc/http_server.hpp"
#include "services/download_service.hpp"

#include <QHBoxLayout>
#include <QWidget>
#include <QApplication>
#include <QSettings>

// 本 TU 保留窗口装配与页面接线（构造/析构/setup_ui/顶栏/侧栏/页面创建）；
// 其余成员函数拆分于：
//   main_window_ipc.cpp       — IPC 服务器装配与 JSON 快照序列化
//   main_window_settings.cpp  — 设置持久化与启动检查更新
//   main_window_tray.cpp      — 托盘/剪贴板监听/悬浮窗/主题槽
//   main_window_frameless.cpp — 无边框边缘缩放与窗口控制槽
//   main_window_service.cpp   — 下载服务接线/请求处理/任务事件

namespace falcon::desktop {

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
}

} // namespace falcon::desktop
