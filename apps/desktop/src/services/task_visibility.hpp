/**
 * @file task_visibility.hpp
 * @brief 任务在下载页两视图中的可见性判定（纯 C++，不依赖 Qt）
 *
 * 下载中视图 = 未终态任务 + Failed：失败任务是可操作的（右键/行内
 * 「继续」重试语义在位），把它藏进任何视图之外会让用户以为任务凭空
 * 消失——「下载完成却不在已完成列表」投诉的次级根因正是失败任务在
 * 两个视图都不可见。Cancelled 是用户显式终止的终态，保持隐藏（与
 * 既有 UX 一致：回收站才是它的去处）。
 *
 * 已完成视图 = 仅 Completed。
 *
 * 本文件不含任何 Qt 头，可独立编译测试
 * （apps/desktop/tests/task_visibility_test.cpp）。
 *
 * @author Falcon Team
 * @date 2026-10-01
 */

#pragma once

#include <falcon/event_listener.hpp>

namespace falcon::desktop::task_visibility {

/// 下载中视图可见：Pending/Preparing/Downloading/Paused/Failed
inline bool visible_in_downloading(falcon::TaskStatus status) {
    switch (status) {
        case falcon::TaskStatus::Pending:
        case falcon::TaskStatus::Preparing:
        case falcon::TaskStatus::Downloading:
        case falcon::TaskStatus::Paused:
        case falcon::TaskStatus::Failed:
            return true;
        case falcon::TaskStatus::Completed:
        case falcon::TaskStatus::Cancelled:
            return false;
    }
    return false;  // 不可达（枚举全覆盖），防御兜底
}

/// 已完成视图可见：仅 Completed
inline bool visible_in_completed(falcon::TaskStatus status) {
    return status == falcon::TaskStatus::Completed;
}

} // namespace falcon::desktop::task_visibility
