/**
 * @file task_order_test.cpp
 * @brief 任务手动排序算法测试（services/task_order.hpp 全语义钉住）
 *
 * 覆盖四组语义：
 * - sort_ids：空序退化为 id 升序（= 旧默认行为）、order 成员优先按
 *   order 位置、order 外任务排尾按 id、重复 order 项取首现
 * - move_to：下行/上行/同位/越界
 * - apply_drag：可见子集按新序回填原位（非成员冻结）、空序种子、
 *   subset 含 order 外任务追加尾部、order 成员多于 subset（竞速防御）
 * - serialize/deserialize 往返 + 垃圾容错（负号/空白/空 token 跳过——
 *   stoull 对 "-3" 会回绕接受，不能依赖它做校验）
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "services/task_order.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace {

using falcon::TaskId;

std::vector<TaskId> ids(std::initializer_list<TaskId> list)
{
    return std::vector<TaskId>(list);
}

} // namespace

// ---------- sort_ids ----------

TEST(TaskOrderTest, SortEmptyOrderFallsBackToIdAscending)
{
    // 从未拖拽过（QSettings 无记录）→ 空序,行为与旧版纯 id 升序完全一致
    const auto sorted = falcon::desktop::task_order::sort_ids(
        {}, ids({42, 7, 100, 3}));
    EXPECT_EQ(sorted, ids({3, 7, 42, 100}));
}

TEST(TaskOrderTest, SortOrderMembersFirstInOrderPositions)
{
    const auto sorted = falcon::desktop::task_order::sort_ids(
        ids({30, 10, 20}), ids({10, 20, 30, 40, 50}));
    // 30/10/20 按 order 位置排前,40/50 不在 order 排尾
    EXPECT_EQ(sorted, ids({30, 10, 20, 40, 50}));
}

TEST(TaskOrderTest, SortUnknownIdsTrailingById)
{
    // order 里的陈旧条目(任务已删除)与未知任务都排尾,之间按 id 升序
    const auto sorted = falcon::desktop::task_order::sort_ids(
        ids({9, 2}), ids({5, 2, 100, 9, 3}));
    EXPECT_EQ(sorted, ids({9, 2, 3, 5, 100}));
}

TEST(TaskOrderTest, SortDuplicateOrderEntriesTakeFirstOccurrence)
{
    // 持久化串被手改出重复 id 的防御：位置取首现,结果确定
    const auto sorted = falcon::desktop::task_order::sort_ids(
        ids({3, 1, 3}), ids({1, 3}));
    EXPECT_EQ(sorted, ids({3, 1}));
}

// ---------- move_to ----------

TEST(TaskOrderTest, MoveToDownwardShiftsIntermediateUp)
{
    // 行 0 拖到行 2:[10,20,30,40] → [20,30,10,40]
    const auto moved = falcon::desktop::task_order::move_to(
        ids({10, 20, 30, 40}), 0, 2);
    EXPECT_EQ(moved, ids({20, 30, 10, 40}));
}

TEST(TaskOrderTest, MoveToUpwardShiftsIntermediateDown)
{
    // 行 3 拖到行 1:[10,20,30,40] → [10,40,20,30]
    const auto moved = falcon::desktop::task_order::move_to(
        ids({10, 20, 30, 40}), 3, 1);
    EXPECT_EQ(moved, ids({10, 40, 20, 30}));
}

TEST(TaskOrderTest, MoveToSameOrOutOfRangeReturnsUnchanged)
{
    const auto source = ids({10, 20, 30});
    EXPECT_EQ(falcon::desktop::task_order::move_to(source, 1, 1), source);
    EXPECT_EQ(falcon::desktop::task_order::move_to(source, 0, 3), source);
    EXPECT_EQ(falcon::desktop::task_order::move_to(source, 3, 0), source);
    EXPECT_EQ(falcon::desktop::task_order::move_to({}, 0, 0), std::vector<TaskId>{});
}

// ---------- apply_drag ----------

TEST(TaskOrderTest, ApplyDragRefillsMemberPositionsInPlace)
{
    // 可见子集 [1,3] 新序 [3,1]:成员位回填新序,非成员(2/4/5)冻结原位
    const auto result = falcon::desktop::task_order::apply_drag(
        ids({1, 2, 3, 4, 5}), ids({3, 1}));
    EXPECT_EQ(result, ids({3, 2, 1, 4, 5}));
}

TEST(TaskOrderTest, ApplyDragEmptyOrderSeedsFromSubset)
{
    // 首次拖拽（order 为空）:结果即可见子集新序,其余任务此后按未知排尾
    const auto result = falcon::desktop::task_order::apply_drag(
        {}, ids({7, 5, 6}));
    EXPECT_EQ(result, ids({7, 5, 6}));
}

TEST(TaskOrderTest, ApplyDragAppendsIdsMissingFromOrder)
{
    // subset 含 order 外任务（拖拽瞬间出现的新任务等防御路径）→ 追加尾部
    const auto result = falcon::desktop::task_order::apply_drag(
        ids({5, 6}), ids({3, 1}));
    EXPECT_EQ(result, ids({5, 6, 3, 1}));
}

TEST(TaskOrderTest, ApplyDragMoreOrderMembersThanSubsetDropsExtras)
{
    // order 成员多于 subset（快照竞速）:耗尽的成员位置丢弃,不越界不错位
    const auto result = falcon::desktop::task_order::apply_drag(
        ids({1, 2, 3}), ids({2}));
    EXPECT_EQ(result, ids({1, 2, 3}));
}

// ---------- serialize / deserialize ----------

TEST(TaskOrderTest, SerializeDeserializeRoundTrip)
{
    const std::vector<TaskId> order = ids({3, 1, 2});
    const std::string text = falcon::desktop::task_order::serialize(order);
    EXPECT_EQ(text, "3,1,2");
    EXPECT_EQ(falcon::desktop::task_order::deserialize(text), order);
    EXPECT_EQ(falcon::desktop::task_order::serialize({}), std::string());
}

TEST(TaskOrderTest, DeserializeSkipsGarbageTokens)
{
    // 非数字/空 token 全部跳过,只收纯数字
    EXPECT_EQ(falcon::desktop::task_order::deserialize("abc,5,,7,2x"), ids({5, 7}));
    // 负号不是纯数字——stoull 对 "-3" 会回绕接受,必须前置校验挡掉
    EXPECT_EQ(falcon::desktop::task_order::deserialize("-3,5"), ids({5}));
    EXPECT_EQ(falcon::desktop::task_order::deserialize(" 7 ,9"), ids({9}));
}

TEST(TaskOrderTest, DeserializeEmptyOrAllGarbageYieldsEmptyOrder)
{
    // 无记录/全垃圾 → 空序 → sort_ids 退化为 id 升序默认行为
    EXPECT_TRUE(falcon::desktop::task_order::deserialize("").empty());
    EXPECT_TRUE(falcon::desktop::task_order::deserialize(",,,").empty());
    EXPECT_TRUE(falcon::desktop::task_order::deserialize("x,y").empty());
}
