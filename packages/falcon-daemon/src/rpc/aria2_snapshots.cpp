#include "rpc/aria2_snapshots.hpp"

#include <cstdlib>

namespace falcon::daemon::rpc {

namespace {

/// aria2 的字节数字段是十进制字符串；空值/非数字按 0 处理
std::uint64_t parse_u64(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) return 0;
    if (it->is_number_unsigned()) return it->get<std::uint64_t>();
    if (it->is_number_integer()) {
        const auto v = it->get<std::int64_t>();
        return v > 0 ? static_cast<std::uint64_t>(v) : 0;
    }
    if (it->is_string()) {
        try {
            const auto v = std::stoull(it->get<std::string>());
            return v;
        } catch (const std::exception&) {
            return 0;
        }
    }
    return 0;
}

std::string first_file_path(const nlohmann::json& status) {
    const auto files = status.find("files");
    if (files == status.end() || !files->is_array() || files->empty()) return {};
    return files->front().value("path", std::string{});
}

std::string first_uri(const nlohmann::json& status) {
    const auto files = status.find("files");
    if (files != status.end() && files->is_array() && !files->empty()) {
        const auto& file = files->front();
        const auto uris = file.find("uris");
        if (uris != file.end() && uris->is_array() && !uris->empty()) {
            return uris->front().value("uri", std::string{});
        }
    }
    // tellStatus 精简视图可能没有 files/uris，回退到扩展字段
    return status.value("url", std::string{});
}

} // namespace

bool TaskSnapshot::is_finished() const noexcept {
    switch (status) {
        case falcon::TaskStatus::Completed:
        case falcon::TaskStatus::Failed:
        case falcon::TaskStatus::Cancelled:
            return true;
        default:
            return false;
    }
}

std::optional<falcon::TaskStatus> task_status_from_aria2_string(const std::string& s) {
    if (s == "active") return falcon::TaskStatus::Downloading;
    if (s == "waiting") return falcon::TaskStatus::Pending;
    if (s == "paused") return falcon::TaskStatus::Paused;
    if (s == "complete") return falcon::TaskStatus::Completed;
    if (s == "error") return falcon::TaskStatus::Failed;
    if (s == "removed") return falcon::TaskStatus::Cancelled;
    return std::nullopt;
}

std::optional<falcon::TaskId> task_id_from_gid(const std::string& gid) {
    std::string s = gid;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        s = s.substr(2);
    }
    if (s.empty() || s.size() > 16) return std::nullopt;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        const auto value = std::stoull(s, nullptr, 16);
        if (value == 0) return std::nullopt;
        return static_cast<falcon::TaskId>(value);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::optional<TaskSnapshot> snapshot_from_status_json(const nlohmann::json& status) {
    if (!status.is_object()) return std::nullopt;

    auto id = task_id_from_gid(status.value("gid", std::string{}));
    if (!id) return std::nullopt;

    auto status_value = task_status_from_aria2_string(status.value("status", std::string{}));
    if (!status_value) return std::nullopt;

    TaskSnapshot snap;
    snap.id = *id;
    snap.status = *status_value;
    snap.url = first_uri(status);
    snap.output_path = first_file_path(status);
    snap.total_bytes = parse_u64(status, "totalLength");
    snap.downloaded_bytes = parse_u64(status, "completedLength");
    snap.speed = parse_u64(status, "downloadSpeed");
    snap.error_message = status.value("errorMessage", std::string{});

    const auto total = static_cast<double>(snap.total_bytes);
    snap.progress = total > 0.0
        ? static_cast<double>(snap.downloaded_bytes) / total
        : (snap.status == falcon::TaskStatus::Completed ? 1.0 : 0.0);

    const auto prio = status.find("priority");
    if (prio != status.end() && prio->is_number_integer()) {
        const auto v = prio->get<int>();
        if (v >= 0 && v <= 3) snap.priority = static_cast<falcon::TaskPriority>(v);
    }
    return snap;
}

std::vector<TaskSnapshot> snapshots_from_status_array(const nlohmann::json& array) {
    std::vector<TaskSnapshot> out;
    if (!array.is_array()) return out;
    out.reserve(array.size());
    for (const auto& entry : array) {
        if (auto snap = snapshot_from_status_json(entry)) {
            out.push_back(std::move(*snap));
        }
    }
    return out;
}

std::optional<GlobalStats> stats_from_global_stat_json(const nlohmann::json& stat) {
    if (!stat.is_object()) return std::nullopt;

    GlobalStats out;
    out.download_speed = parse_u64(stat, "downloadSpeed");
    const auto active = parse_u64(stat, "numActive");
    const auto waiting = parse_u64(stat, "numWaiting");
    const auto stopped = parse_u64(stat, "numStopped");
    out.active_tasks = static_cast<std::size_t>(active);
    out.waiting_tasks = static_cast<std::size_t>(waiting);
    out.stopped_tasks = static_cast<std::size_t>(stopped);
    return out;
}

} // namespace falcon::daemon::rpc
