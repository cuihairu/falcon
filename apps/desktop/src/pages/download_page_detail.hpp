/**
 * @file download_page_detail.hpp
 * @brief Shared layout constants and status text helper for DownloadPage
 */

#pragma once

#include <QObject>

#include <falcon/types.hpp>

namespace falcon::desktop {

namespace download_page_detail {

constexpr int kRowHeight = 56;
constexpr int kSummaryCardWidth = 168;
// 紧凑横条标准（用户 bug 反馈）：hero 与统计卡固定高度，纵向空白不随窗口拉伸
constexpr int kHeroHeight = 78;
constexpr int kSummaryCardHeight = 72;
// 空态卡最小展示高度（用户 bug 反馈：自然高度 ~90px 塌缩挤压，给足 ≥200px）
constexpr int kEmptyStateMinHeight = 232;

// 表格列索引（与 create_task_table 的表头顺序对齐）：
// 下载中视图隐藏「做种」（本视图恒空），已完成视图隐藏「进度」「速度」（恒 100%/—）
constexpr int kColFilename = 0;
constexpr int kColProgress = 1;
constexpr int kColSize = 2;
constexpr int kColSpeed = 3;
constexpr int kColStatus = 4;
constexpr int kColSeed = 5;
constexpr int kColAction = 6;

// 快照状态 → 界面中文文案（引擎状态名不外露）
inline QString status_display_text(falcon::TaskStatus status)
{
    switch (status) {
        case falcon::TaskStatus::Pending:    return QObject::tr("等待中");
        case falcon::TaskStatus::Preparing:  return QObject::tr("准备中");
        case falcon::TaskStatus::Downloading: return QObject::tr("下载中");
        case falcon::TaskStatus::Paused:     return QObject::tr("已暂停");
        case falcon::TaskStatus::Completed:  return QObject::tr("已完成");
        case falcon::TaskStatus::Failed:     return QObject::tr("失败");
        case falcon::TaskStatus::Cancelled:  return QObject::tr("已取消");
    }
    return QObject::tr("未知");
}

} // namespace download_page_detail

} // namespace falcon::desktop
