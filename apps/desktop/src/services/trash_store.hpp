/**
 * @file trash_store.hpp
 * @brief 回收站存储：删除任务的记录与成品文件的暂存（纯 C++，不依赖 Qt）
 * @author Falcon Team
 * @date 2026-09-28
 */

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace falcon::desktop {

/// 回收站条目：一次删除任务留下的记录
struct TrashEntry {
    std::uint64_t id = 0;          ///< 原任务 id
    std::string url;               ///< 原下载地址
    std::string output_path;       ///< 原成品路径（恢复目标）
    std::string file_name;         ///< 显示名（output_path 的文件名部分）
    std::uint64_t total_bytes = 0; ///< 原任务总大小
    std::string status;            ///< completed / cancelled / failed
    bool file_in_trash = false;    ///< 成品文件是否已移入回收站目录
    std::string trash_file_path;   ///< 回收站内文件路径（file_in_trash 时有效）
    std::int64_t deleted_at = 0;   ///< 删除时刻（epoch 秒）
};

/**
 * @brief 回收站存储
 *
 * 目录布局：<下载目录>/.falcon-trash/
 *   - trash.json   条目清单（原子写：tmp + rename）
 *   - <id>_<name>  暂存的成品文件（id 前缀天然防同名冲突）
 *
 * 仅 Completed 任务的成品文件移入（Cancelled/Failed 只记录不移动半成品）。
 * 所有方法线程安全（内部互斥锁）；list/size 供 GUI 线程直读，变更类方法
 * 由 DownloadService 的 worker 线程调用。
 */
class TrashStore {
public:
    /// store_dir 为空目录路径（不要求已存在，首次写入时创建）
    explicit TrashStore(std::string store_dir);

    std::vector<TrashEntry> list() const;
    std::size_t size() const;

    /// 入站。move_source 非空时把该文件移动进回收站目录并置
    /// entry.file_in_trash（entry 内部字段会被补全）。返回 false 表示
    /// 落库失败（条目未入账）。
    bool add(TrashEntry entry, const std::string& move_source = {});

    /// 恢复：文件移回 output_path（父目录自动创建、同名占用时 ".N" 去重），
    /// 删除记录。返回恢复后的实际路径；记录不存在返回 nullopt。
    std::optional<std::string> restore(std::uint64_t id);

    /// 彻底删除：删除回收站文件 + 记录。记录不存在返回 false。
    bool purge(std::uint64_t id);

    /// 清空全部条目与回收站文件，返回清除的条数。
    int clear();

    /// 清理删除超过 days 天的条目（days <= 0 不清理），返回清除条数。
    int purge_expired(int days, std::int64_t now_epoch_seconds);

    const std::string& store_dir() const { return store_dir_; }

private:
    void load();
    void save_locked() const;
    TrashEntry* find_locked(std::uint64_t id);

    std::string store_dir_;
    mutable std::mutex mutex_;
    std::vector<TrashEntry> entries_;
};

} // namespace falcon::desktop
