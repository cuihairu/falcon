/**
 * @file task_order.hpp
 * @brief 任务手动排序的纯 C++ 算法与持久化序列化（不依赖 Qt）
 *
 * 「任务拖拽排序」的排序键语义：持久化 order 是全部已知任务 id 的一个
 * 子序列（拖拽时按当前快照剪枝）；显示排序键 = (order 中的位置, 任务 id)——
 * 不在 order 中的任务排在已知任务之后（按 id 升序稳定），order 为空时整体
 * 退化为纯 id 升序（与拖拽功能上线前的行为一致）。
 *
 * 过滤视图（顶栏搜索/已完成视图）中的拖拽只重排「可见子集」：apply_drag
 * 把 order 中可见成员占据的位置按拖拽后的新相对顺序重填，非成员位置不动
 * ——被过滤隐藏的任务不会因可见子集的拖拽而跳动。
 *
 * 本文件与 task_order.cpp 不含任何 Qt 头，可独立编译测试
 * （apps/desktop/tests/task_order_test.cpp）。
 *
 * @author Falcon Team
 * @date 2026-09-30
 */

#pragma once

#include <falcon/types.hpp>

#include <string>
#include <vector>

namespace falcon::desktop::task_order {

/// 显示排序：按 (order 中的位置, id) 稳定排序；order 中的陈旧 id
/// （任务已删除）自然被忽略，不在 order 的 id 排在已知任务之后
std::vector<falcon::TaskId> sort_ids(const std::vector<falcon::TaskId>& order,
                                     const std::vector<falcon::TaskId>& ids);

/// 行移动原语（拖拽 from 行落到 to 行）：取出 from 元素插入 to 位置，
/// 区间内元素顺移；越界下标按容器边界钳制（防御，正常调用方已校验）
std::vector<falcon::TaskId> move_to(std::vector<falcon::TaskId> ids,
                                    std::size_t from, std::size_t to);

/// 拖拽后重建全局 order：order 中出现于 subset_new 的成员位置按
/// subset_new 的相对顺序重填（成员占位数不变），非成员原位保留；
/// subset_new 中不在 order 的 id（防御路径）追加到尾部
std::vector<falcon::TaskId> apply_drag(const std::vector<falcon::TaskId>& order,
                                       const std::vector<falcon::TaskId>& subset_new);

/// 序列化为 "3,1,2" 逗号串（QSettings 持久化格式）
std::string serialize(const std::vector<falcon::TaskId>& order);

/// 解析逗号串；逐 token 容错——非纯数字 token（垃圾/负数/溢出）跳过，
/// 空串/全垃圾解析为空 order（回落 id 升序默认行为）
std::vector<falcon::TaskId> deserialize(const std::string& text);

} // namespace falcon::desktop::task_order
