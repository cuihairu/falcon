/**
 * @file task_visibility_test.cpp
 * @brief 任务两视图可见性判定测试（services/task_visibility.hpp 全语义钉住）
 *
 * 覆盖三组语义：
 * - visible_in_downloading：五个非终态 + Failed 全可见——Failed 可见是
 *   本文件的回归钉子（失败任务可「继续」重试，两个视图都不可见 =
 *   用户眼里任务凭空消失）；Completed/Cancelled 不可见
 * - visible_in_completed：仅 Completed（Cancelled 不是完成）
 * - 全枚举巡检：两谓词在 TaskStatus 每个枚举值上恰好一真一假——
 *   任何视图都不允许「看不见的终态」或「两视图重复的终态」
 * @author Falcon Team
 * @date 2026-10-01
 */

#include "services/task_visibility.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

// TaskStatus 全枚举值表（与 falcon::TaskStatus 定义一一对应；新增枚举
// 值时本用例自动扩面——表里不补会编译失败不了，但巡检会漏，改枚举时
// 同步维护这里）
const std::vector<falcon::TaskStatus> kAllStatuses = {
    falcon::TaskStatus::Pending,
    falcon::TaskStatus::Preparing,
    falcon::TaskStatus::Downloading,
    falcon::TaskStatus::Paused,
    falcon::TaskStatus::Completed,
    falcon::TaskStatus::Failed,
    falcon::TaskStatus::Cancelled,
};

} // namespace

// ---------- visible_in_downloading ----------

TEST(TaskVisibilityTest, ActiveStatusesVisibleInDownloadingView)
{
    using falcon::TaskStatus;
    for (const auto s : {TaskStatus::Pending, TaskStatus::Preparing,
                         TaskStatus::Downloading, TaskStatus::Paused}) {
        EXPECT_TRUE(falcon::desktop::task_visibility::visible_in_downloading(s))
            << "status=" << static_cast<int>(s);
    }
}

TEST(TaskVisibilityTest, FailedVisibleInDownloadingView)
{
    // 回归钉子：失败任务此前两个视图都不可见——「下载失败后任务凭空
    // 消失」投诉的直接钉子。失败任务可「继续」重试，必须留在下载中
    EXPECT_TRUE(falcon::desktop::task_visibility::visible_in_downloading(
        falcon::TaskStatus::Failed));
}

TEST(TaskVisibilityTest, TerminalCompletedHiddenInDownloadingView)
{
    EXPECT_FALSE(falcon::desktop::task_visibility::visible_in_downloading(
        falcon::TaskStatus::Completed));
}

TEST(TaskVisibilityTest, CancelledHiddenInDownloadingView)
{
    // Cancelled 是用户显式终止的终态，回收站才是它的去处
    EXPECT_FALSE(falcon::desktop::task_visibility::visible_in_downloading(
        falcon::TaskStatus::Cancelled));
}

// ---------- visible_in_completed ----------

TEST(TaskVisibilityTest, OnlyCompletedVisibleInCompletedView)
{
    using falcon::TaskStatus;
    EXPECT_TRUE(falcon::desktop::task_visibility::visible_in_completed(
        TaskStatus::Completed));
    EXPECT_FALSE(falcon::desktop::task_visibility::visible_in_completed(
        TaskStatus::Failed));
    EXPECT_FALSE(falcon::desktop::task_visibility::visible_in_completed(
        TaskStatus::Cancelled));
    EXPECT_FALSE(falcon::desktop::task_visibility::visible_in_completed(
        TaskStatus::Paused));
}

// ---------- 全枚举巡检 ----------

TEST(TaskVisibilityTest, EveryStatusVisibleInExactlyOneOrZeroViews)
{
    for (const auto s : kAllStatuses) {
        const bool in_dl =
            falcon::desktop::task_visibility::visible_in_downloading(s);
        const bool in_done =
            falcon::desktop::task_visibility::visible_in_completed(s);
        // 两视图互斥：任何状态不得同时可见（同一任务两视图重复出现）
        EXPECT_FALSE(in_dl && in_done) << "status=" << static_cast<int>(s);
        // 下载中视图语义 = 非终态 + Failed；已完成视图语义 = Completed。
        // 两者并集恰好覆盖全枚举 = 没有任何状态凭空消失（Cancelled
        // 唯一例外：终态且两视图皆藏，回收站承载）
        const bool expected_hidden = (s == falcon::TaskStatus::Cancelled);
        EXPECT_EQ(in_dl || in_done, !expected_hidden)
            << "status=" << static_cast<int>(s);
    }
}
