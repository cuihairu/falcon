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
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <QVector>
#include <cstdio>

#include "dialogs/add_download_dialog.hpp"
#include "navigation/sidebar.hpp"
#include "pages/cloud_page.hpp"
#include "pages/discovery_page.hpp"
#include "pages/download_page.hpp"
#include "pages/settings_page.hpp"
#include "pages/trash_page.hpp"
#include "services/trash_store.hpp"
#include "utils/theme_manager.hpp"
#include "widgets/status_bar.hpp"
#include "widgets/top_bar.hpp"

#include <rpc/aria2_snapshots.hpp>

#include <ctime>
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

    auto make_seeding = [&make](falcon::TaskId id, const char* url,
                                std::uint64_t uploaded, double seconds,
                                bool active) {
        // 做种任务:BT 完成后进入做种阶段(Completed + seed 扩展字段);
        // uploaded/total 直接决定 ratio 展示
        auto snapshot = make(id, url, falcon::TaskStatus::Completed, 1.0,
                             4296000000ULL, 0);
        snapshot.seed_uploaded_bytes = uploaded;
        snapshot.seed_downloaded_bytes = 4296000000ULL;
        snapshot.seed_total_size = 4296000000ULL;
        snapshot.seeded_seconds = seconds;
        snapshot.seeding_active = active;
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
        // 做种中:ratio 1.20 / 已做种 2时15分
        make_seeding(6, "https://tracker.example.org/torrents/falcon-live-dvd.iso",
                     5155200000ULL, 8100.0, true),
        // 达标已停止:ratio 1.00 / 做种 60 分
        make_seeding(7, "https://tracker.example.org/torrents/podcast-pack.zip",
                     4296000000ULL, 3600.0, false),
    };
}

struct Shell {
    QWidget* root = nullptr;
    QStackedWidget* stack = nullptr;
    DownloadPage* download = nullptr;
    TrashPage* trash = nullptr;
};

/// 回收站演示条目（与 MainWindow 实际删除路径写入的记录同构）
std::vector<falcon::desktop::TrashEntry> demo_trash_entries()
{
    const std::int64_t now = std::time(nullptr);
    auto entry = [&now](std::uint64_t id, const char* name, std::uint64_t bytes,
                        const char* status, bool in_trash,
                        std::int64_t age_seconds) {
        falcon::desktop::TrashEntry e;
        e.id = id;
        e.url = std::string("https://cdn.example.com/media/") + name;
        e.output_path = std::string("/home/user/Downloads/") + name;
        e.file_name = name;
        e.total_bytes = bytes;
        e.status = status;
        e.file_in_trash = in_trash;
        if (in_trash) {
            e.trash_file_path = "/home/user/Downloads/.falcon-trash/"
                                + std::to_string(id) + "_" + name;
        }
        e.deleted_at = now - age_seconds;
        return e;
    };
    return {
        entry(12, "ubuntu-24.04.3-desktop-amd64.iso", 5700000000ULL,
              "completed", true, 3600 * 5),
        entry(9, "project-archive.zip", 4296000000ULL, "completed", true,
              3600 * 49),
        entry(5, "suspended-transfer.bin", 730000000ULL, "cancelled", false,
              3600 * 26),
    };
}

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
    auto* trash = new TrashPage;
    auto* settings = new SettingsPage;
    stack->addWidget(download);   // 0
    stack->addWidget(cloud);      // 1
    stack->addWidget(discovery);  // 2
    stack->addWidget(trash);      // 3
    stack->addWidget(settings);   // 4
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
    QObject::connect(side_bar, &SideBar::trashClicked,
                     stack, [stack] { stack->setCurrentIndex(3); });
    QObject::connect(side_bar, &SideBar::settingsClicked,
                     stack, [stack] { stack->setCurrentIndex(4); });

    Shell shell;
    shell.root = central;
    shell.stack = stack;
    shell.download = download;
    shell.trash = trash;
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
    // 尺寸矩阵:生产默认 1200×800(main_window resize)+ 最小窗口
    // 960×640(挤压场景)——重叠类布局缺陷只在窄窗下暴露
    struct SizeCase { int w; int h; const char* tag; };
    const SizeCase size_cases[] = {{1200, 800, "1200"}, {960, 640, "960"}};

    // 添加下载对话框:主窗之外的最高频交互面,此前从未进截图验收
    const auto snap_add_dialog = [&](const QString& name) {
        UrlInfo info;
        info.protocol = UrlProtocol::HTTPS;
        info.original_url = info.decoded_url =
            "https://cdn.example.com/media/ubuntu-24.04.3-desktop-amd64.iso";
        info.file_name = "ubuntu-24.04.3-desktop-amd64.iso";
        info.file_size = "5.7 GB";
        info.is_valid = true;
        AddDownloadDialog dialog(info);
        dialog.resize(600, 450);
        dialog.show();
        for (int i = 0; i < 8; ++i) {
            app.processEvents();
            app.sendPostedEvents(nullptr, QEvent::LayoutRequest);
        }
        const QString path = out_dir + "/" + name + ".png";
        if (!dialog.grab().save(path)) {
            std::fprintf(stderr, "failed to save %s\n", qPrintable(path));
            return;
        }
        std::printf("saved %s\n", qPrintable(path));
    };

    shell.download->update_tasks(demo_tasks());
    shell.trash->set_entries(demo_trash_entries());
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
    // navTab 创建序:0 下载中 / 1 已完成 / 2 资源发现 / 3 云盘空间 / 4 回收站 / 5 偏好设置
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
        go(1); // 已完成(做种任务在此视图,做种列/卡片做种摘要在位)
        snap("download_completed_" + suffix);
        shell.download->toggle_display_style();
        snap("download_completed_grid_" + suffix);
        shell.download->toggle_display_style();
        go(3); // 云盘空间
        snap("cloud_" + suffix);
        go(2); // 资源发现
        snap("discovery_" + suffix);
        go(4); // 回收站
        snap("trash_" + suffix);
        go(5); // 偏好设置
        snap("settings_" + suffix);
        // 设置组特写(做种/回收站/关于与更新组在滚动区折叠线以下,整窗截图看不到)
        for (auto* box : shell.root->findChildren<QGroupBox*>()) {
            const QString title = box->title();
            QString tag;
            if (title.contains(QString::fromUtf8("做种"))) {
                tag = "settings_seeding";
            } else if (title.contains(QString::fromUtf8("回收站"))) {
                tag = "settings_trash";
            } else if (title.contains(QString::fromUtf8("资源搜索"))) {
                tag = "settings_search";
            } else if (title.contains(QString::fromUtf8("关于与更新"))) {
                tag = "settings_about";
            } else {
                continue;
            }
            const QString path = out_dir + "/" + tag + "_" + suffix + ".png";
            if (!box->grab().save(path)) {
                std::fprintf(stderr, "failed to save %s\n", qPrintable(path));
            } else {
                std::printf("saved %s\n", qPrintable(path));
            }
        }
        snap_add_dialog("add_dialog_" + suffix);
    };

    for (const auto& sz : size_cases) {
        shell.root->resize(sz.w, sz.h);
        theme.set_theme(ThemeType::Light);
        shoot_all(QString("light_") + sz.tag);
        theme.set_theme(ThemeType::Dark);
        shoot_all(QString("dark_") + sz.tag);
    }

    delete shell.root;
    return 0;
}
