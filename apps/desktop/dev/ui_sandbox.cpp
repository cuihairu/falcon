/**
 * @file ui_sandbox.cpp
 * @brief 离屏 UI 截图沙盒(设计验收/视觉回归工具)
 *
 * QT_QPA_PLATFORM=offscreen 下按主窗布局组装真实组件(TopBar/SideBar/
 * 各页面/StatusBar),注入演示任务数据,亮/暗两主题各截一轮 png。
 * 供无 Qt6/无显示环境的机器验收设计效果,也可挂 CI 出视觉快照。
 *
 * 与真实 MainWindow 的差异:不构造 DownloadService(引擎/网络零副作用),
 * 窗口不是 frameless(截图不需要拖动/缩放);组件、布局参数、样式表
 * 与生产完全一致。
 *
 * 用法:falcon-ui-sandbox [输出目录](默认 ./ui_shots)
 *
 * @author Falcon Team
 * @date 2026-09-17
 */

#include <QApplication>
#include <QDir>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVector>
#include <cstdio>

#include "navigation/sidebar.hpp"
#include "pages/cloud_page.hpp"
#include "pages/discovery_page.hpp"
#include "pages/download_page.hpp"
#include "pages/settings_page.hpp"
#include "utils/theme_manager.hpp"
#include "widgets/status_bar.hpp"
#include "widgets/top_bar.hpp"

#include <rpc/aria2_snapshots.hpp>

#include <string>
#include <vector>

// Falcon 组件类均在 falcon::desktop 命名空间
using namespace falcon::desktop;

namespace {

using falcon::daemon::rpc::TaskSnapshot;

std::vector<TaskSnapshot> demo_tasks()
{
    auto make = [](falcon::TaskId id, const char* url, falcon::TaskStatus status,
                   double progress, std::uint64_t total, std::uint64_t speed,
                   const char* error = "") {
        TaskSnapshot snapshot;
        snapshot.id = id;
        snapshot.url = url;
        const std::string path(url);
        snapshot.output_path = "/home/user/Downloads/" +
                               path.substr(path.find_last_of('/') + 1);
        snapshot.status = status;
        snapshot.progress = progress;
        snapshot.total_bytes = total;
        snapshot.downloaded_bytes = static_cast<std::uint64_t>(total * progress);
        snapshot.speed = speed;
        snapshot.error_message = error;
        return snapshot;
    };

    return {
        make(1, "https://cdn.example.com/media/ubuntu-24.04.3-desktop-amd64.iso",
             falcon::TaskStatus::Downloading, 0.683, 6120000000ULL, 11250000),
        make(2, "https://mirror.example.org/pub/fedora-workstation-live.x86_64.iso",
             falcon::TaskStatus::Downloading, 0.241, 2940000000ULL, 4120000),
        make(3, "https://files.example.net/archives/source-code.tar.gz",
             falcon::TaskStatus::Paused, 0.512, 486000000ULL, 0),
        make(4, "https://dl.example.com/tools/asset-pack-final.zip",
             falcon::TaskStatus::Completed, 1.0, 1240000000ULL, 0),
        make(5, "https://cdn.example.com/videos/season-01.m3u8",
             falcon::TaskStatus::Failed, 0.08, 780000000ULL, 0,
             "HTTP 404 Not Found"),
    };
}

struct Shell {
    QWidget* root = nullptr;
    QStackedWidget* stack = nullptr;
    DownloadPage* download = nullptr;
};

Shell build_shell()
{
    auto* central = new QWidget;
    central->setObjectName("centralWidget");
    auto* root_layout = new QVBoxLayout(central);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    // 布局与 MainWindow::setup_ui 保持一致:顶栏 + (侧栏|内容) + 状态栏
    auto* top_bar = new TopBar;
    root_layout->addWidget(top_bar);

    auto* body = new QHBoxLayout;
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    auto* side_bar = new SideBar;
    body->addWidget(side_bar);

    auto* stack = new QStackedWidget;
    auto* download = new DownloadPage;
    auto* cloud = new CloudPage;
    auto* discovery = new DiscoveryPage;
    auto* settings = new SettingsPage;
    stack->addWidget(download);
    stack->addWidget(cloud);
    stack->addWidget(discovery);
    stack->addWidget(settings);
    body->addWidget(stack, 1);
    root_layout->addLayout(body, 1);

    auto* status_bar = new StatusBar;
    root_layout->addWidget(status_bar);

    // 侧栏接线(照 main_window 的真实语义):下载 tab 各自带视图模式
    QObject::connect(side_bar, &SideBar::downloadingTabClicked, stack, [stack, download] {
        stack->setCurrentIndex(0);
        download->set_view_mode(DownloadViewMode::Downloading);
    });
    QObject::connect(side_bar, &SideBar::completedTabClicked, stack, [stack, download] {
        stack->setCurrentIndex(0);
        download->set_view_mode(DownloadViewMode::Completed);
    });
    QObject::connect(side_bar, &SideBar::cloudClicked,
                     stack, [stack] { stack->setCurrentIndex(1); });
    QObject::connect(side_bar, &SideBar::discoveryClicked,
                     stack, [stack] { stack->setCurrentIndex(2); });
    QObject::connect(side_bar, &SideBar::settingsClicked,
                     stack, [stack] { stack->setCurrentIndex(3); });

    Shell shell;
    shell.root = central;
    shell.stack = stack;
    shell.download = download;
    return shell;
}

} // namespace

int main(int argc, char** argv)
{
    // 必须先于 QApplication;无显示环境下纯软件渲染
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);

    const QString out_dir = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                     : QStringLiteral("ui_shots");
    if (!QDir().mkpath(out_dir)) {
        std::fprintf(stderr, "cannot create output dir: %s\n",
                     qPrintable(out_dir));
        return 1;
    }

    ThemeManager theme;

    Shell shell = build_shell();
    shell.root->resize(1280, 832);

    shell.download->update_tasks(demo_tasks());
    if (auto* status = shell.root->findChild<StatusBar*>()) {
        status->set_download_speed(15370000);
        status->set_task_counts(2, 1);
    }
    if (auto* side = shell.root->findChild<SideBar*>()) {
        side->set_queue_count(2);
    }

    const auto snap = [&](const QString& name) {
        shell.root->show();
        // 布局/样式 polish 与延迟渲染需要若干轮事件循环
        for (int i = 0; i < 8; ++i) {
            app.processEvents();
            app.sendPostedEvents(nullptr, QEvent::LayoutRequest);
        }
        const QString path = out_dir + "/" + name + ".png";
        if (!shell.root->grab().save(path)) {
            std::fprintf(stderr, "failed to save %s\n", qPrintable(path));
            return;
        }
        std::printf("saved %s\n", qPrintable(path));
    };

    // 经侧栏按钮真实点击切页(信号 + QButtonGroup 选中态同步),
    // navTab 创建序:0 下载中 / 1 已完成 / 2 资源发现 / 3 云盘空间 / 4 偏好设置
    const auto nav_tabs = shell.root->findChildren<QPushButton*>("navTab");
    const auto go = [&nav_tabs](int idx) {
        if (idx < nav_tabs.size()) {
            nav_tabs[idx]->click();
        }
    };

    const auto shoot_all = [&](const QString& suffix) {
        go(0); // 下载中·表格
        snap("download_table_" + suffix);
        shell.download->toggle_display_style();
        snap("download_grid_" + suffix);
        shell.download->toggle_display_style();
        go(1); // 已完成
        snap("download_completed_" + suffix);
        go(3); // 云盘空间
        snap("cloud_" + suffix);
        go(2); // 资源发现
        snap("discovery_" + suffix);
        go(4); // 偏好设置
        snap("settings_" + suffix);
    };

    theme.set_theme(ThemeType::Light);
    shoot_all("light");
    theme.set_theme(ThemeType::Dark);
    shoot_all("dark");

    delete shell.root;
    return 0;
}
