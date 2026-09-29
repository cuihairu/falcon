/**
 * @file trash_store.cpp
 * @brief TrashStore 实现（std::filesystem + nlohmann JSON 原子落盘）
 * @author Falcon Team
 * @date 2026-09-28
 */

#include "trash_store.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace falcon::desktop {

namespace {

namespace fs = std::filesystem;

constexpr const char* kManifestName = "trash.json";
constexpr int kManifestVersion = 1;

/// 文件名部分（无子路径语义——回收站只存成品文件的平铺副本名）
std::string basename_of(const std::string& path)
{
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

/// 回收站内暂存文件名：<id>_<file_name>
std::string trash_file_name(std::uint64_t id, const std::string& file_name)
{
    return std::to_string(id) + "_" + file_name;
}

/// 跨设备移动回退：rename 失败（EXDEV 等）时 copy + remove
bool move_file(const fs::path& from, const fs::path& to)
{
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) {
        return true;
    }
    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        return false;
    }
    ec.clear();
    fs::remove(from, ec);
    return true;
}

/// 恢复目标重名去重：path 已占用时依次尝试 path.1 / path.2 ...（上限 99）
fs::path dedupe_target(const fs::path& target)
{
    if (!fs::exists(target)) {
        return target;
    }
    for (int i = 1; i < 100; ++i) {
        fs::path candidate = target;
        candidate += "." + std::to_string(i);
        if (!fs::exists(candidate)) {
            return candidate;
        }
    }
    return target; // 全占用：交由上层 rename 失败收口
}

nlohmann::json entry_to_json(const TrashEntry& e)
{
    nlohmann::json j;
    j["id"] = e.id;
    j["url"] = e.url;
    j["output_path"] = e.output_path;
    j["file_name"] = e.file_name;
    j["total_bytes"] = e.total_bytes;
    j["status"] = e.status;
    j["file_in_trash"] = e.file_in_trash;
    j["trash_file_path"] = e.trash_file_path;
    j["deleted_at"] = e.deleted_at;
    return j;
}

TrashEntry entry_from_json(const nlohmann::json& j)
{
    TrashEntry e;
    e.id = j.value("id", std::uint64_t{0});
    e.url = j.value("url", std::string{});
    e.output_path = j.value("output_path", std::string{});
    e.file_name = j.value("file_name", std::string{});
    e.total_bytes = j.value("total_bytes", std::uint64_t{0});
    e.status = j.value("status", std::string{});
    e.file_in_trash = j.value("file_in_trash", false);
    e.trash_file_path = j.value("trash_file_path", std::string{});
    e.deleted_at = j.value("deleted_at", std::int64_t{0});
    return e;
}

} // namespace

TrashStore::TrashStore(std::string store_dir)
    : store_dir_(std::move(store_dir))
{
    load();
}

void TrashStore::load()
{
    if (store_dir_.empty()) {
        return;
    }
    std::ifstream in(fs::path(store_dir_) / kManifestName);
    if (!in) {
        return; // 首次使用 / 尚无删除记录
    }
    try {
        nlohmann::json root = nlohmann::json::parse(in);
        if (!root.is_object() || root.value("version", 0) != kManifestVersion) {
            return;
        }
        const auto it = root.find("entries");
        if (it == root.end() || !it->is_array()) {
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
        for (const auto& item : *it) {
            if (item.is_object()) {
                entries_.push_back(entry_from_json(item));
            }
        }
    } catch (const std::exception&) {
        // 清单损坏按空回收站处理（半成品文件仍可经文件管理器手工取回）
    }
}

void TrashStore::save_locked() const
{
    const fs::path dir(store_dir_);
    std::error_code ec;
    fs::create_directories(dir, ec);
    ec.clear();

    nlohmann::json root;
    root["version"] = kManifestVersion;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : entries_) {
        arr.push_back(entry_to_json(e));
    }
    root["entries"] = std::move(arr);

    const fs::path tmp = dir / (std::string(kManifestName) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) {
            return; // 目录不可写：条目仅在内存（重启后丢失，可接受）
        }
        out << root.dump(2);
    }
    fs::rename(tmp, dir / kManifestName, ec);
    if (ec) {
        fs::remove(tmp, ec);
    }
}

TrashEntry* TrashStore::find_locked(std::uint64_t id)
{
    const auto it = std::find_if(entries_.begin(), entries_.end(),
                                 [id](const TrashEntry& e) { return e.id == id; });
    return it == entries_.end() ? nullptr : &*it;
}

std::vector<TrashEntry> TrashStore::list() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
}

std::size_t TrashStore::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

bool TrashStore::add(TrashEntry entry, const std::string& move_source)
{
    if (store_dir_.empty()) {
        return false;
    }
    entry.file_name = basename_of(entry.output_path);
    entry.trash_file_path.clear();
    entry.file_in_trash = false;
    // 调用方忘填删除时间时兜底当前时刻（0 会被过期清理立即清除）
    if (entry.deleted_at == 0) {
        entry.deleted_at = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now()
                                   .time_since_epoch())
                               .count();
    }

    if (!move_source.empty()) {
        const fs::path dir(store_dir_);
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path target = dir / trash_file_name(entry.id, entry.file_name);
        if (!move_file(move_source, target)) {
            return false; // 移动失败不入账（文件留在原位，任务已移除可重试）
        }
        entry.file_in_trash = true;
        entry.trash_file_path = target.string();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    // 同 id 旧条目（理论上不可能：id 单调递增）先移除防重复
    if (auto* existing = find_locked(entry.id)) {
        *existing = std::move(entry);
    } else {
        entries_.push_back(std::move(entry));
    }
    save_locked();
    return true;
}

std::optional<std::string> TrashStore::restore(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrashEntry* entry = find_locked(id);
    if (!entry) {
        return std::nullopt;
    }

    std::optional<std::string> restored = entry->output_path;
    if (entry->file_in_trash && !entry->trash_file_path.empty()) {
        const fs::path source(entry->trash_file_path);
        std::error_code ec;
        if (fs::exists(source, ec)) {
            const fs::path target_path(entry->output_path);
            ec.clear();
            fs::create_directories(target_path.parent_path(), ec);
            const fs::path final_target = dedupe_target(target_path);
            if (move_file(source, final_target)) {
                restored = final_target.string();
            }
        }
        // 文件已不存在的记录级恢复：仅移除条目，返回原目标路径
    }

    entries_.erase(entries_.begin() + (entry - entries_.data()));
    save_locked();
    return restored;
}

bool TrashStore::purge(std::uint64_t id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    TrashEntry* entry = find_locked(id);
    if (!entry) {
        return false;
    }
    if (entry->file_in_trash && !entry->trash_file_path.empty()) {
        std::error_code ec;
        fs::remove(fs::path(entry->trash_file_path), ec); // 尽力而为
    }
    entries_.erase(entries_.begin() + (entry - entries_.data()));
    save_locked();
    return true;
}

int TrashStore::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    int removed = 0;
    for (const auto& entry : entries_) {
        if (entry.file_in_trash && !entry.trash_file_path.empty()) {
            std::error_code ec;
            fs::remove(fs::path(entry.trash_file_path), ec);
        }
        ++removed;
    }
    entries_.clear();
    save_locked();
    return removed;
}

int TrashStore::purge_expired(int days, std::int64_t now_epoch_seconds)
{
    if (days <= 0) {
        return 0;
    }
    const std::int64_t cutoff = now_epoch_seconds
        - static_cast<std::int64_t>(days) * 24 * 3600;
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint64_t> expired;
    for (const auto& entry : entries_) {
        // <=：保留 N 天的语义是「满 N 天即清」（deleted + days <= now）
        if (entry.deleted_at <= cutoff) {
            expired.push_back(entry.id);
        }
    }
    for (const auto id : expired) {
        if (TrashEntry* entry = find_locked(id)) {
            if (entry->file_in_trash && !entry->trash_file_path.empty()) {
                std::error_code ec;
                fs::remove(fs::path(entry->trash_file_path), ec);
            }
            entries_.erase(entries_.begin() + (entry - entries_.data()));
        }
    }
    if (!expired.empty()) {
        save_locked();
    }
    return static_cast<int>(expired.size());
}

} // namespace falcon::desktop
