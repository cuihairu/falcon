/**
 * @file task_order.cpp
 * @brief 任务手动排序算法实现（纯 C++，见 task_order.hpp 语义说明）
 * @author Falcon Team
 * @date 2026-09-30
 */

#include "task_order.hpp"

#include <algorithm>
#include <cstdlib>
#include <unordered_map>

namespace falcon::desktop::task_order {

std::vector<falcon::TaskId> sort_ids(const std::vector<falcon::TaskId>& order,
                                     const std::vector<falcon::TaskId>& ids)
{
    // id → order 位置（order 内重复 id 取首现，防御持久化串被手改出重复）
    std::unordered_map<falcon::TaskId, std::size_t> position;
    position.reserve(order.size() * 2);
    for (std::size_t i = 0; i < order.size(); ++i) {
        position.emplace(order[i], i);
    }

    std::vector<falcon::TaskId> sorted = ids;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&position](falcon::TaskId a, falcon::TaskId b) {
                         const auto ia = position.find(a);
                         const auto ib = position.find(b);
                         // 排序键 = (order 位置, id)：未知任务排在已知之后，
                         // 未知与未知之间按 id 升序（与 order 为空的默认行为一致）
                         if (ia != position.end() && ib != position.end()) {
                             if (ia->second != ib->second) {
                                 return ia->second < ib->second;
                             }
                             return a < b;
                         }
                         if (ia != position.end()) {
                             return true;   // a 已知 b 未知 → a 在前
                         }
                         if (ib != position.end()) {
                             return false;  // a 未知 b 已知 → b 在前
                         }
                         return a < b;      // 双未知按 id
                     });
    return sorted;
}

std::vector<falcon::TaskId> move_to(std::vector<falcon::TaskId> ids,
                                    std::size_t from, std::size_t to)
{
    if (ids.empty() || from >= ids.size() || to >= ids.size() || from == to) {
        return ids;
    }
    const falcon::TaskId item = ids[from];
    ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(from));
    ids.insert(ids.begin() + static_cast<std::ptrdiff_t>(to), item);
    return ids;
}

std::vector<falcon::TaskId> apply_drag(const std::vector<falcon::TaskId>& order,
                                       const std::vector<falcon::TaskId>& subset_new)
{
    // subset 成员集合（去重防御：拖拽行集天然无重复，持久化串可能被手改）
    std::vector<falcon::TaskId> subset = subset_new;
    subset.erase(std::unique(subset.begin(), subset.end()), subset.end());

    std::vector<falcon::TaskId> result;
    result.reserve(order.size() + subset.size());
    std::size_t next = 0;  // subset 指针：order 中每遇到一个成员位置取下一个新序元素
    for (const falcon::TaskId id : order) {
        if (std::find(subset.begin(), subset.end(), id) != subset.end()) {
            if (next < subset.size()) {
                result.push_back(subset[next]);
                ++next;
            }
            // next 耗尽意味着 order 里成员数多于 subset（快照竞速），位置丢弃
        } else {
            result.push_back(id);
        }
    }
    // subset 中不在 order 的 id（新任务在拖拽瞬间出现等防御路径）追加尾部
    for (; next < subset.size(); ++next) {
        result.push_back(subset[next]);
    }
    return result;
}

std::string serialize(const std::vector<falcon::TaskId>& order)
{
    std::string text;
    text.reserve(order.size() * 4);
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (i > 0) {
            text.push_back(',');
        }
        text += std::to_string(order[i]);
    }
    return text;
}

std::vector<falcon::TaskId> deserialize(const std::string& text)
{
    std::vector<falcon::TaskId> order;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        const std::size_t comma = text.find(',', begin);
        const std::size_t end = (comma == std::string::npos) ? text.size() : comma;
        const std::string token = text.substr(begin, end - begin);
        // 纯数字才接受：负号/十六进制/垃圾/空 token 全部跳过
        // （stoull 对 "-3" 会回绕接受，不能依赖它做校验）
        if (!token.empty()
                && token.find_first_not_of("0123456789") == std::string::npos) {
            order.push_back(static_cast<falcon::TaskId>(
                std::strtoull(token.c_str(), nullptr, 10)));
        }
        if (comma == std::string::npos) {
            break;
        }
        begin = comma + 1;
    }
    return order;
}

} // namespace falcon::desktop::task_order
