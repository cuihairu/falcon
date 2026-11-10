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
 * 用法:
 *   falcon-ui-sandbox [输出目录]                 全矩阵截图(默认 ./ui_shots)
 *   falcon-ui-sandbox --prototype [输出目录]     主视图设计原型两版(暗色 1200):
 *     warm-console-dark.png  = 生产 warm console 暗色主题现状
 *     cold-utility-dark.png  = 冷峻工具感变体(对标 Motrix:更深中性底/
 *                              高对比文字/克制 accent)——token 值仅在本次
 *                              沙盒会话内替换(QSS hex 替换 + QPalette 重建),
 *                              生产 theme_tokens/QSS 零改动
 *   falcon-ui-sandbox --redesign [输出目录]      重设计提案全集(todo UI-RD-001):
 *     三方向(warm 迭代 / cold 迭代 / graphite emerald 自由向)× 亮暗 ×
 *     14 功能面 = 84 张(1200 宽);token 仅在沙盒会话内替换,生产零改动
 *   falcon-ui-sandbox --speed-float [输出目录]   悬浮速度窗走查:
 *     亮暗双主题 × 速度三档(0 / 2MB/s / 15MB/s);mid/fast 档喂任务预览
 *     (标题省略 + 暂停/继续钮 + 进度行),idle 档验证无任务占位
 *
 * @author Falcon Team
 * @date 2026-09-17
 */

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QStackedWidget>
#include <QThread>
#include <QVBoxLayout>
#include <QVector>
#include <cstdio>
#include <cstring>
#include <utility>

#include "dialogs/add_download_dialog.hpp"
#include "navigation/sidebar.hpp"
#include "pages/cloud_page.hpp"
#include "pages/discovery_page.hpp"
#include "pages/download_page.hpp"
#include "pages/settings_page.hpp"
#include "pages/trash_page.hpp"
#include "services/trash_store.hpp"
#include "utils/theme_manager.hpp"
#include "widgets/speed_float_widget.hpp"
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

// ---------------------------------------------------------------------------
// 设计原型 B「cold utility」:冷峻工具感暗色变体(对标 Motrix)
//
// 机制:token 值仅在本次沙盒会话内替换——对 ThemeManager 的暗色 QSS 文本
// 做 hex 热冷映射 + 整串替换 checkbox 内嵌 base64 SVG,再按冷 token 重建
// QPalette。生产 theme_tokens.hpp / fluent_dark.qss 零改动。
//
// 色板(WCAG AA 实算):window #131519 / card #1b1e24 / text #f5f7fa(17.0:1,
// 高于 warm 的 15.3)/ secondary #98a0ab(6.9:1)/ accent #5b9df5(6.6:1,
// 上文字 #0d1a2b 6.3:1)/ divider #2b3038;danger 语义色两版不变。
//
// 已知边界:图标着色经 icon_utils 的 tokens_for() 解析(生产值),Text 角色
// 暖白 #f2ede8 与冷白 #f5f7fa 在 16px 单色图标上不可辨;Accent 角色图标
// 仅云盘连接钮一处(主视图不出现)——原型对比不受影响。
// ---------------------------------------------------------------------------

/// 暗色 QSS 全部可变 hex 的 热→冷 映射(danger 除外)
struct ColdHexMap {
    const char* warm;
    const char* cold;
    const char* note;
};
const ColdHexMap kColdHexMap[] = {
    {"#1b1714", "#131519", "window"},
    {"#262019", "#1b1e24", "card"},
    {"#37302a", "#2b3038", "divider"},
    {"#f2ede8", "#f5f7fa", "text"},
    {"#b5aca3", "#98a0ab", "text_secondary"},
    {"#6e665e", "#5a6169", "text_disabled"},
    {"#312a24", "#22262d", "控件底(按钮/输入)"},
    {"#4a423a", "#363c45", "控件描边"},
    {"#3a322b", "#262b33", "分段钮 checked 底"},
    {"#554b41", "#3d444e", "卡片 hover 描边"},
    {"#ffa07a", "#5b9df5", "accent"},
    {"#ffb28c", "#7ab1f8", "accent hover"},
    {"#e0875e", "#4585e0", "accent pressed"},
    {"#27140a", "#0d1a2b", "accent 上文字"},
};

/// QPalette 角色映射镜像 theme_manager.cpp 的 palette_for(token 冷值)
QPalette cold_palette()
{
    QPalette pal;
    const QColor window(0x13, 0x15, 0x19);
    const QColor card(0x1b, 0x1e, 0x24);
    const QColor text(0xf5, 0xf7, 0xfa);
    const QColor text_secondary(0x98, 0xa0, 0xab);
    const QColor text_disabled(0x5a, 0x61, 0x69);
    const QColor accent(0x5b, 0x9d, 0xf5);
    const QColor accent_text(0x0d, 0x1a, 0x2b);
    const QColor divider(0x2b, 0x30, 0x38);
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, card);
    pal.setColor(QPalette::AlternateBase, window);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::PlaceholderText, text_disabled);
    pal.setColor(QPalette::Button, card);
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::BrightText, text);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, accent_text);
    pal.setColor(QPalette::Link, accent);
    pal.setColor(QPalette::ToolTipBase, card);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Light, card);
    pal.setColor(QPalette::Midlight, divider);
    pal.setColor(QPalette::Mid, divider);
    pal.setColor(QPalette::Dark, divider);
    pal.setColor(QPalette::Disabled, QPalette::Window, window);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::Text, text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::Base, window);
    return pal;
}

/// 会话内应用冷色变体:暗色 QSS hex 替换 + checkbox base64 SVG 整串替换
void apply_cold_dark_theme(const QString& dark_qss)
{
    QString qss = dark_qss;
    for (const auto& m : kColdHexMap) {
        qss.replace(QString::fromLatin1(m.warm), QString::fromLatin1(m.cold));
    }
    // checkbox 内嵌 base64 SVG 的 hex 藏在 base64 内,hex 替换够不到——整串换
    // (warm/cold 串均由 tools 级 python 解码-替换-重编码生成,绝不手抄)
    qss.replace(QStringLiteral("PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNiIgaGVpZ2h0PSIxNiI+PHJlY3QgeD0iMC41IiB5PSIwLjUiIHdpZHRoPSIxNSIgaGVpZ2h0PSIxNSIgcng9IjMiIGZpbGw9IiMyNjIwMTkiIHN0cm9rZT0iIzZlNjY1ZSIvPjwvc3ZnPg=="),
                QStringLiteral("PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNiIgaGVpZ2h0PSIxNiI+PHJlY3QgeD0iMC41IiB5PSIwLjUiIHdpZHRoPSIxNSIgaGVpZ2h0PSIxNSIgcng9IjMiIGZpbGw9IiMxYjFlMjQiIHN0cm9rZT0iIzVhNjE2OSIvPjwvc3ZnPg=="));
    qss.replace(QStringLiteral("PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNiIgaGVpZ2h0PSIxNiI+PHJlY3Qgd2lkdGg9IjE2IiBoZWlnaHQ9IjE2IiByeD0iMyIgZmlsbD0iI2ZmYTA3YSIvPjxwYXRoIGQ9Ik00IDguNWwyLjggMi44TDEyIDUuNSIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjMjcxNDBhIiBzdHJva2Utd2lkdGg9IjEuOCIgc3Ryb2tlLWxpbmVjYXA9InJvdW5kIiBzdHJva2UtbGluZWpvaW49InJvdW5kIi8+PC9zdmc+"),
                QStringLiteral("PHN2ZyB4bWxucz0iaHR0cDovL3d3dy53My5vcmcvMjAwMC9zdmciIHdpZHRoPSIxNiIgaGVpZ2h0PSIxNiI+PHJlY3Qgd2lkdGg9IjE2IiBoZWlnaHQ9IjE2IiByeD0iMyIgZmlsbD0iIzViOWRmNSIvPjxwYXRoIGQ9Ik00IDguNWwyLjggMi44TDEyIDUuNSIgZmlsbD0ibm9uZSIgc3Ryb2tlPSIjMGQxYTJiIiBzdHJva2Utd2lkdGg9IjEuOCIgc3Ryb2tlLWxpbmVjYXA9InJvdW5kIiBzdHJva2UtbGluZWpvaW49InJvdW5kIi8+PC9zdmc+"));
    qApp->setStyleSheet(qss);
    qApp->setPalette(cold_palette());
}

// ---------------------------------------------------------------------------
// --redesign 模式(todo UI-RD-001,2026-11):重设计提案三方向全集
//
// 与上方 --prototype 单变体的差异:三方向 × 亮暗双主题 × 14 功能面,
// 且修正 --prototype 时代的两处过时假设:
//   ① checkbox 已随 B23 迁到 qrc 图标(QSS url() 引用)——apply_cold_dark_
//      theme 的 base64 整串替换成为死代码(替换目标串已不在 QSS 里),旧
//      cold-utility 原型渲染的是生产暖色 qrc checkbox;本模式改为 QSS
//      url() 路径替换 → 会话内生成的方向变体 SVG 文件(结构逐字节同生产
//      assets/icons/checkbox-*.svg,仅色值取方向 token)。
//   ② 亮色主题此前无变体——三方向均补齐 light 面(含各自 token 表)。
// 生产 theme_tokens.hpp / 双 QSS / qrc 零改动。
//
// 三方向(色值全部经 WCAG 实算,对比脚本 /tmp/wcag_check.py 口径):
//   A warm console II(迭代):暖中性精修——暗部加深一档、文字对比再抬升
//     (dark text 16.73:1 / accent 于 window 9.37:1),accent 鲑橙家族原样
//     保留(品牌锚点不动);light 暖白微调(text 14.49:1 / accent 4.76:1)。
//   B cold utility II(迭代):冷峻工具感——dark 沿既有原型 B 色板(对标
//     Motrix 的更深中性底 + 克制蓝 accent),补齐全新冷色 light 面
//     (text 14.29:1 / accent #2f6fd0 4.50:1,达标 AA)。
//   C graphite emerald(自由向):近黑石墨中性 + 祖母绿强调,自成一体
//     的 token 体系(非 Motrix/Fluent 复刻;dark accent 于 window 9.78:1)。
//
// 已知边界(如实披露,与 --prototype 同族):① falcon logo SVG 烧入品牌
// 鲑橙 #ffa07a,不随 token(品牌锚点,冷/绿方向保留);② 图标着色经
// icon_utils tokens_for() 取生产值,方向色差不进入单色图标(修复需改
// 生产代码,超出原型铁律);③ rgba 叠加色(alpha 混合)不映射;④ 生产
// unchecked-light 环色 #a29a92 与 disabled 文字 token #a8a29e 是两个值,
// 环色单独映射(方向变体取环色行的映射值)。
// ---------------------------------------------------------------------------

/// QSS 可变 hex 的 热→方向 映射(未命中回落热值——accent 不动的方向零条目)
struct RedesignHexMap {
    const char* warm;
    const char* variant;
    const char* note;
};

// A「warm console II」暗色(danger/accent 家族不变,不进表)
const RedesignHexMap kWarmIiDark[] = {
    {"#1b1714", "#16120f", "window"},
    {"#262019", "#221c16", "card"},
    {"#37302a", "#3a322a", "divider"},
    {"#f2ede8", "#f7f2ec", "text"},
    {"#b5aca3", "#bcb2a6", "text_secondary"},
    {"#6e665e", "#756c62", "text_disabled"},
    {"#312a24", "#2d2620", "控件底(按钮/输入)"},
    {"#4a423a", "#4e453c", "控件描边"},
    {"#3a322b", "#362e26", "分段钮 checked 底"},
    {"#554b41", "#5a4f44", "卡片 hover 描边"},
};
// A「warm console II」亮色(#ffffff 双角色 card/accent_text 均不动)
const RedesignHexMap kWarmIiLight[] = {
    {"#f6f4f1", "#f7f5f2", "window"},
    {"#292524", "#262220", "text"},
    {"#57534e", "#524d47", "text_secondary"},
    {"#a8a29e", "#aaa39c", "text_disabled"},
    {"#a29a92", "#aba49b", "checkbox 描边环"},
    {"#e7e5e4", "#e4e1dc", "divider"},
    {"#dcd7d1", "#d8d3cb", "边框/分隔强"},
    {"#f5f2ee", "#f4f1ec", "hover 底"},
    {"#edeae5", "#eae6df", "控件底"},
    {"#e8e3dd", "#e7e1d9", "primaryButton disabled 底"},
    {"#cac2b9", "#c5bdb2", "卡片 hover 描边"},
};
// B「cold utility II」暗色(与 --prototype 的 kColdHexMap 同值,含 accent 族)
const RedesignHexMap kColdIiDark[] = {
    {"#1b1714", "#131519", "window"},
    {"#262019", "#1b1e24", "card"},
    {"#37302a", "#2b3038", "divider"},
    {"#f2ede8", "#f5f7fa", "text"},
    {"#b5aca3", "#98a0ab", "text_secondary"},
    {"#6e665e", "#5a6169", "text_disabled"},
    {"#312a24", "#22262d", "控件底(按钮/输入)"},
    {"#4a423a", "#363c45", "控件描边"},
    {"#3a322b", "#262b33", "分段钮 checked 底"},
    {"#554b41", "#3d444e", "卡片 hover 描边"},
    {"#ffa07a", "#5b9df5", "accent"},
    {"#ffb28c", "#7ab1f8", "accent hover"},
    {"#e0875e", "#4585e0", "accent pressed"},
    {"#27140a", "#0d1a2b", "accent 上文字"},
};
// B「cold utility II」亮色(全新冷色面;#ffffff 不动)
const RedesignHexMap kColdIiLight[] = {
    {"#f6f4f1", "#f4f6f8", "window"},
    {"#292524", "#1e252c", "text"},
    {"#57534e", "#5c6672", "text_secondary"},
    {"#a8a29e", "#a2abb5", "text_disabled"},
    {"#a29a92", "#a3acb6", "checkbox 描边环"},
    {"#c2410c", "#2f6fd0", "accent"},
    {"#9a3412", "#275cb3", "accent hover"},
    {"#7c2d12", "#1f4c96", "accent pressed"},
    {"#e7e5e4", "#dee3e8", "divider"},
    {"#dcd7d1", "#d2d9e0", "边框/分隔强"},
    {"#f5f2ee", "#eef1f4", "hover 底"},
    {"#edeae5", "#e3e8ed", "控件底"},
    {"#e8e3dd", "#e1e8f0", "primaryButton disabled 底"},
    {"#cac2b9", "#c5ccd4", "卡片 hover 描边"},
};
// C「graphite emerald」暗色(近黑石墨 + 祖母绿)
const RedesignHexMap kGraphiteDark[] = {
    {"#1b1714", "#0f1214", "window"},
    {"#262019", "#161b1e", "card"},
    {"#37302a", "#262d31", "divider"},
    {"#f2ede8", "#eef2f0", "text"},
    {"#b5aca3", "#9ba5a2", "text_secondary"},
    {"#6e665e", "#5d6663", "text_disabled"},
    {"#312a24", "#1d2326", "控件底(按钮/输入)"},
    {"#4a423a", "#333c40", "控件描边"},
    {"#3a322b", "#222a2d", "分段钮 checked 底"},
    {"#554b41", "#3b4449", "卡片 hover 描边"},
    {"#ffa07a", "#34d399", "accent"},
    {"#ffb28c", "#5fe0ae", "accent hover"},
    {"#e0875e", "#1fb07e", "accent pressed"},
    {"#27140a", "#052017", "accent 上文字"},
};
// C「graphite emerald」亮色(冷灰绿白面;#ffffff 不动)
const RedesignHexMap kGraphiteLight[] = {
    {"#f6f4f1", "#f4f7f5", "window"},
    {"#292524", "#1b231f", "text"},
    {"#57534e", "#55605b", "text_secondary"},
    {"#a8a29e", "#a1aaa6", "text_disabled"},
    {"#a29a92", "#a3aca8", "checkbox 描边环"},
    {"#c2410c", "#0b7d59", "accent"},
    {"#9a3412", "#096649", "accent hover"},
    {"#7c2d12", "#064f39", "accent pressed"},
    {"#e7e5e4", "#e0e6e3", "divider"},
    {"#dcd7d1", "#d3ddd8", "边框/分隔强"},
    {"#f5f2ee", "#eef2f0", "hover 底"},
    {"#edeae5", "#e2e9e5", "控件底"},
    {"#e8e3dd", "#dcebe3", "primaryButton disabled 底"},
    {"#cac2b9", "#c2cdc7", "卡片 hover 描边"},
};

struct RedesignDirection {
    const char* slug;       // 输出文件名前缀
    const char* title;      // 提案名(日志用)
    const RedesignHexMap* dark_map;
    std::size_t dark_n;
    const RedesignHexMap* light_map;
    std::size_t light_n;
};
const RedesignDirection kRedesignDirections[] = {
    {"warm", "A warm console II",
     kWarmIiDark, sizeof(kWarmIiDark) / sizeof(kWarmIiDark[0]),
     kWarmIiLight, sizeof(kWarmIiLight) / sizeof(kWarmIiLight[0])},
    {"cold", "B cold utility II",
     kColdIiDark, sizeof(kColdIiDark) / sizeof(kColdIiDark[0]),
     kColdIiLight, sizeof(kColdIiLight) / sizeof(kColdIiLight[0])},
    {"graphite", "C graphite emerald",
     kGraphiteDark, sizeof(kGraphiteDark) / sizeof(kGraphiteDark[0]),
     kGraphiteLight, sizeof(kGraphiteLight) / sizeof(kGraphiteLight[0])},
};

/// 热 hex → 方向 hex;未命中回落热值(accent 不动的方向零条目即零替换)
QString hex_mapped(const RedesignHexMap* map, std::size_t n, const char* warm)
{
    for (std::size_t i = 0; i < n; ++i) {
        if (std::strcmp(map[i].warm, warm) == 0) {
            return QString::fromLatin1(map[i].variant);
        }
    }
    return QString::fromLatin1(warm);
}

/// 方向 QPalette(锚点镜像 theme_manager palette_for,色值经方向映射)
QPalette direction_palette(const RedesignHexMap* map, std::size_t n, bool dark)
{
    const char* window_hex = dark ? "#1b1714" : "#f6f4f1";
    const char* card_hex = dark ? "#262019" : "#ffffff";
    const char* text_hex = dark ? "#f2ede8" : "#292524";
    const char* secondary_hex = dark ? "#b5aca3" : "#57534e";
    const char* disabled_hex = dark ? "#6e665e" : "#a8a29e";
    const char* accent_hex = dark ? "#ffa07a" : "#c2410c";
    const char* accent_text_hex = dark ? "#27140a" : "#ffffff";
    const char* divider_hex = dark ? "#37302a" : "#e7e5e4";
    auto to_color = [&](const char* hex) {
        return QColor(hex_mapped(map, n, hex));
    };
    const QColor window = to_color(window_hex);
    const QColor card = to_color(card_hex);
    const QColor text = to_color(text_hex);
    const QColor text_secondary = to_color(secondary_hex);
    const QColor text_disabled = to_color(disabled_hex);
    const QColor accent = to_color(accent_hex);
    const QColor accent_text = to_color(accent_text_hex);
    const QColor divider = to_color(divider_hex);
    QPalette pal;
    pal.setColor(QPalette::Window, window);
    pal.setColor(QPalette::WindowText, text);
    pal.setColor(QPalette::Base, card);
    pal.setColor(QPalette::AlternateBase, window);
    pal.setColor(QPalette::Text, text);
    pal.setColor(QPalette::PlaceholderText, text_secondary);
    pal.setColor(QPalette::Button, card);
    pal.setColor(QPalette::ButtonText, text);
    pal.setColor(QPalette::BrightText, text);
    pal.setColor(QPalette::Highlight, accent);
    pal.setColor(QPalette::HighlightedText, accent_text);
    pal.setColor(QPalette::Link, accent);
    pal.setColor(QPalette::ToolTipBase, card);
    pal.setColor(QPalette::ToolTipText, text);
    pal.setColor(QPalette::Light, card);
    pal.setColor(QPalette::Midlight, divider);
    pal.setColor(QPalette::Mid, divider);
    pal.setColor(QPalette::Dark, divider);
    pal.setColor(QPalette::Disabled, QPalette::Window, window);
    pal.setColor(QPalette::Disabled, QPalette::WindowText, text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::Text, text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, text_disabled);
    pal.setColor(QPalette::Disabled, QPalette::Base, window);
    return pal;
}

/// 生成方向 × 主题的 checkbox SVG 变体(结构逐字节同生产 assets/icons/
/// checkbox-*.svg,仅色值取方向 token:checked = accent 底 + accent_text
/// 勾;unchecked = card 底 + 描边环)。QSS url() 接受绝对文件路径。
void write_checkbox_variants(const QString& dir, const QString& accent,
                             const QString& accent_text, const QString& card,
                             const QString& ring, QString* checked_path,
                             QString* unchecked_path)
{
    QDir().mkpath(dir);
    const QString checked_svg = QStringLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
        "<rect width=\"16\" height=\"16\" rx=\"3\" fill=\"%1\"/>"
        "<path d=\"M4 8.5l2.8 2.8L12 5.5\" fill=\"none\" stroke=\"%2\" "
        "stroke-width=\"1.8\" stroke-linecap=\"round\" "
        "stroke-linejoin=\"round\"/></svg>");
    const QString unchecked_svg = QStringLiteral(
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" height=\"16\">"
        "<rect x=\"0.5\" y=\"0.5\" width=\"15\" height=\"15\" rx=\"3\" "
        "fill=\"%1\" stroke=\"%2\"/></svg>");
    *checked_path = dir + QStringLiteral("/checkbox-checked.svg");
    *unchecked_path = dir + QStringLiteral("/checkbox-unchecked.svg");
    const std::pair<QString, QString> files[] = {
        {checked_svg.arg(accent, accent_text), *checked_path},
        {unchecked_svg.arg(card, ring), *unchecked_path},
    };
    for (const auto& file : files) {
        QFile out(file.second);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::fprintf(stderr, "cannot write %s\n",
                         qPrintable(file.second));
            continue;
        }
        out.write(file.first.toUtf8());
    }
}

/// 会话内应用方向变体:QSS hex 映射 + checkbox qrc 路径 → 变体文件路径
void apply_direction_theme(const QString& qss_in, const RedesignHexMap* map,
                           std::size_t n, bool dark,
                           const QString& checked_path,
                           const QString& unchecked_path)
{
    QString qss = qss_in;
    for (std::size_t i = 0; i < n; ++i) {
        qss.replace(QString::fromLatin1(map[i].warm),
                    QString::fromLatin1(map[i].variant));
    }
    // B23 起 checkbox 走 qrc 图标——两条 url() 引用是全部 QSS 中仅有的
    // 资源引用,整路径替换即可(无 disabled 变体图标,与生产一致)
    qss.replace(dark ? QStringLiteral(":/icons/checkbox-checked-dark.svg")
                     : QStringLiteral(":/icons/checkbox-checked-light.svg"),
                checked_path);
    qss.replace(dark ? QStringLiteral(":/icons/checkbox-unchecked-dark.svg")
                     : QStringLiteral(":/icons/checkbox-unchecked-light.svg"),
                unchecked_path);
    qApp->setStyleSheet(qss);
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

    // --prototype <目录>:主视图设计原型两版;否则全矩阵截图
    bool prototype_mode = false;
    bool redesign_mode = false;
    bool speed_float_mode = false;
    QString out_dir = QStringLiteral("ui_shots");
    if (argc > 1 && qstrcmp(argv[1], "--prototype") == 0) {
        prototype_mode = true;
        out_dir = argc > 2 ? QString::fromLocal8Bit(argv[2])
                           : QStringLiteral("ui_prototypes");
    } else if (argc > 1 && qstrcmp(argv[1], "--redesign") == 0) {
        redesign_mode = true;
        out_dir = argc > 2 ? QString::fromLocal8Bit(argv[2])
                           : QStringLiteral("ui_redesign");
    } else if (argc > 1 && qstrcmp(argv[1], "--speed-float") == 0) {
        speed_float_mode = true;
        out_dir = argc > 2 ? QString::fromLocal8Bit(argv[2])
                           : QStringLiteral("speed_float_shots");
    } else if (argc > 1) {
        out_dir = QString::fromLocal8Bit(argv[1]);
    }
    if (!QDir().mkpath(out_dir)) {
        std::fprintf(stderr, "cannot create output dir: %s\n",
                     qPrintable(out_dir));
        return 1;
    }

    if (speed_float_mode) {
        // 悬浮速度窗走查:B21/B22 重构后的紧凑横条。布局无动画,帧推进
        // 只为让布局/省略号稳定;mid/fast 档喂任务预览(长文件名触发
        // 省略 + 暂停/继续钮 + 进度行),idle 档验证无任务占位形态。
        struct SpeedCase {
            const char* tag;
            std::uint64_t speed;
            int tasks;
            bool with_task;
            double progress;
        };
        const SpeedCase speed_cases[] = {
            {"idle", 0, 0, false, 0.0},
            {"mid", 2ULL * 1024 * 1024, 3, true, 0.42},
            {"fast", 15ULL * 1024 * 1024, 5, true, 0.87},
        };
        const ThemeType themes[] = {ThemeType::Light, ThemeType::Dark};
        const auto pump = [&app](int frames) {
            for (int i = 0; i < frames; ++i) {
                app.processEvents();
                QThread::msleep(16);
            }
        };
        for (const ThemeType theme_type : themes) {
            SpeedFloatWidget float_widget;
            float_widget.apply_theme(theme_type);
            float_widget.set_show_active_tasks(true);
            float_widget.set_show_total_progress(true);
            float_widget.move(40, 40);
            float_widget.show();
            pump(6);
            for (const auto& speed_case : speed_cases) {
                SpeedFloatWidget::Stats st;
                st.download_speed = speed_case.speed;
                st.active_tasks = speed_case.tasks;
                st.overall_progress =
                    speed_case.speed > 0 ? speed_case.progress : -1.0;
                float_widget.update_stats(st);
                if (speed_case.with_task) {
                    SpeedFloatWidget::TaskPreview tp;
                    tp.has_task = true;
                    tp.task_id = 1;
                    tp.running = speed_case.speed > 0;
                    tp.url = QStringLiteral("https://example.com/falcon/"
                                            "speed-float-preview.bin");
                    tp.file_name = QStringLiteral(
                        "speed_float_preview_very_long_file_name.bin");
                    tp.directory = QStringLiteral("/tmp");
                    tp.progress = speed_case.progress;
                    float_widget.update_task_preview(tp);
                } else {
                    float_widget.update_task_preview({});
                }
                pump(8);
                const QString prefix = out_dir + "/"
                    + (theme_type == ThemeType::Light ? "light" : "dark")
                    + "_" + speed_case.tag + ".png";
                float_widget.grab().save(prefix);
                std::printf("saved %s\n", qPrintable(prefix));
            }
        }
        return 0;
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

    // 托盘菜单:生产 tray_menu_ 同构(显示主窗口 / 分隔线 / 退出),QMenu 自动
    // 继承 qApp 样式表——第 14 个功能面,此前从未进截图验收
    const auto snap_tray_menu = [&](const QString& name) {
        QMenu menu;
        menu.addAction(QString::fromUtf8("显示主窗口"));
        menu.addSeparator();
        menu.addAction(QString::fromUtf8("退出"));
        menu.ensurePolished();
        menu.show();
        for (int i = 0; i < 8; ++i) {
            app.processEvents();
            app.sendPostedEvents(nullptr, QEvent::LayoutRequest);
        }
        const QString path = out_dir + "/" + name + ".png";
        if (!menu.grab().save(path)) {
            std::fprintf(stderr, "failed to save %s\n", qPrintable(path));
            return;
        }
        std::printf("saved %s\n", qPrintable(path));
        menu.hide();
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
        // 本沙盒从不进入 app.exec()：DeferredDelete 事件在同层
        // processEvents() 中不会被投递（仅在返回更低层事件循环或此处
        // 显式冲刷时才清理），网格视图 deleteLater() 摘除的旧卡片会
        // 以孤儿形态残留在截图里——显式冲刷后再抓帧
        app.sendPostedEvents(nullptr, QEvent::DeferredDelete);
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

    if (redesign_mode) {
        // 重设计提案全集:三方向 × 亮暗 × 14 功能面(1200 宽;960 挤压不在
        // 本单范围)。token 替换只发生在沙盒会话内,生产主题文件零触碰。
        shell.root->resize(1200, 800);
        for (const auto& dir : kRedesignDirections) {
            for (int t = 0; t < 2; ++t) {
                const bool dark = t == 1;
                const ThemeType theme_type = dark ? ThemeType::Dark : ThemeType::Light;
                theme.set_theme(theme_type);
                // checkbox 变体锚点(经方向映射):checked = accent 底 +
                // accent_text 勾;unchecked = card 底 + 描边环(环色独立于
                // token,见区块注释)。dark/light 各自取自对应方向映射。
                const RedesignHexMap* map = dark ? dir.dark_map : dir.light_map;
                const std::size_t n = dark ? dir.dark_n : dir.light_n;
                QString checked_path;
                QString unchecked_path;
                const QString assets_dir = out_dir + "/assets/"
                    + QString::fromLatin1(dir.slug) + "-"
                    + (dark ? QStringLiteral("dark") : QStringLiteral("light"));
                write_checkbox_variants(assets_dir,
                                        hex_mapped(map, n, dark ? "#ffa07a" : "#c2410c"),
                                        hex_mapped(map, n, dark ? "#27140a" : "#ffffff"),
                                        hex_mapped(map, n, dark ? "#262019" : "#ffffff"),
                                        hex_mapped(map, n, dark ? "#6e665e" : "#a29a92"),
                                        &checked_path, &unchecked_path);
                apply_direction_theme(theme.stylesheet(theme_type), map, n,
                                      dark, checked_path, unchecked_path);
                qApp->setPalette(direction_palette(map, n, dark));
                const QString suffix = QString::fromLatin1(dir.slug) + "_"
                    + (dark ? QStringLiteral("dark") : QStringLiteral("light"));
                std::printf("=== redesign %s ===\n", dir.title);
                shoot_all(suffix);
                snap_tray_menu(QStringLiteral("tray_menu_") + suffix);
            }
        }
        delete shell.root;
        return 0;
    } else if (prototype_mode) {
        // 主视图原型:暗色 1200×800,「下载中·表格」视图(应用落地第一屏,
        // hero 摘要卡 + 七列任务表 + 侧栏/顶栏/状态栏全要素)
        shell.root->resize(1200, 800);
        theme.set_theme(ThemeType::Dark);
        go(0);
        snap(QStringLiteral("warm-console-dark"));
        // 变体 B:会话内 token 替换,生产主题文件零触碰
        apply_cold_dark_theme(theme.stylesheet(ThemeType::Dark));
        snap(QStringLiteral("cold-utility-dark"));
    } else {
        for (const auto& sz : size_cases) {
            shell.root->resize(sz.w, sz.h);
            theme.set_theme(ThemeType::Light);
            shoot_all(QString("light_") + sz.tag);
            theme.set_theme(ThemeType::Dark);
            shoot_all(QString("dark_") + sz.tag);
        }
    }

    delete shell.root;
    return 0;
}
