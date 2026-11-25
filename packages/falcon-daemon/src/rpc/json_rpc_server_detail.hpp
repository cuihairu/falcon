/**
 * @file json_rpc_server_detail.hpp
 * @brief JSON-RPC 服务器 TU 间共享的内部辅助（aria2 方法分发/序列化/鉴权/分页）
 * @author Falcon Team
 * @date 2026-11-22
 */

#pragma once

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>

#include <nlohmann/json.hpp>

#ifdef FALCON_HAS_SQLITE3
#include "storage/task_storage.hpp"
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace falcon {
namespace daemon {
namespace rpc {
namespace detail {

using json = nlohmann::json;

inline std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

inline std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

// per-download 数值选项的字符串/整数双形态解析（aria2 addUri options 惯例）。
// 缺省语义由调用方以 contains 判定；key 存在但无法解析时返回 nullopt——
// addUri 必须以 -32602 收口，而非让裸 std::stoull 抛异常被 dispatch 兜底成
// -32603 Internal error（错误码与 JSON-RPC 参数错误语义不符）。stoull 前缀
// 解析（"12abc" -> 12）与既有 getOption/seed 系键一致，取宽松方向
inline std::optional<std::size_t> parse_numeric_option(const json& value) {
    if (value.is_number_unsigned()) return value.get<std::size_t>();
    if (value.is_number_integer()) {
        const auto n = value.get<std::int64_t>();
        if (n < 0) return std::nullopt;
        return static_cast<std::size_t>(n);
    }
    if (value.is_string()) {
        try {
            return static_cast<std::size_t>(std::stoull(value.get<std::string>()));
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// per-download 布尔选项的 JSON 布尔/"true"/"false" 字符串双形态解析；
// 其余形态（数字、非法字面量）返回 nullopt
inline std::optional<bool> parse_bool_option(const json& value) {
    if (value.is_boolean()) return value.get<bool>();
    if (value.is_string()) {
        const std::string s = to_lower(trim(value.get<std::string>()));
        if (s == "true") return true;
        if (s == "false") return false;
    }
    return std::nullopt;
}

inline std::string task_id_to_gid(falcon::TaskId id) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << static_cast<std::uint64_t>(id);
    return oss.str();
}

inline std::optional<falcon::TaskId> gid_to_task_id(const std::string& gid) {
    std::string s = gid;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        s = s.substr(2);
    }
    if (s.empty() || s.size() > 16) return std::nullopt;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        std::uint64_t value = std::stoull(s, nullptr, 16);
        if (value == 0) return std::nullopt;
        return static_cast<falcon::TaskId>(value);
    } catch (...) {
        return std::nullopt;
    }
}

inline std::string aria2_status_from_task_status(falcon::TaskStatus status) {
    switch (status) {
        case falcon::TaskStatus::Downloading:
        case falcon::TaskStatus::Preparing:
            return "active";
        case falcon::TaskStatus::Pending:
            return "waiting";
        case falcon::TaskStatus::Paused:
            return "paused";
        case falcon::TaskStatus::Completed:
            return "complete";
        case falcon::TaskStatus::Cancelled:
            return "removed";
        case falcon::TaskStatus::Failed:
            return "error";
    }
    return "error";
}

inline std::string aria2_status_from_task(const falcon::DownloadTask& task) {
    return aria2_status_from_task_status(task.status());
}

// aria2 风格的文件条目（tellStatus.files / getFiles 共用）
inline json files_json(const std::string& path, falcon::Bytes total,
                       falcon::Bytes completed, const std::string& url,
                       bool include_selected) {
    json file{
        {"path", path},
        {"length", std::to_string(total)},
        {"completedLength", std::to_string(completed)},
        {"uris", json::array({json{{"uri", url}, {"status", "used"}}})},
    };
    if (include_selected) {
        file["selected"] = "true";
    }
    return json::array({std::move(file)});
}

// 将任务的下载选项映射回 aria2 选项键（getOption / 便于客户端回显）
inline json download_options_to_json(const falcon::DownloadOptions& options) {
    json out;
    out["dir"] = options.output_directory;
    out["out"] = options.output_filename;
    out["user-agent"] = options.user_agent;
    out["referer"] = options.referer;
    out["load-cookies"] = options.cookie_file;
    out["save-cookies"] = options.cookie_jar;
    out["http-user"] = options.http_username;
    out["http-passwd"] = options.http_password;
    out["certificate"] = options.client_certificate;
    out["private-key"] = options.client_private_key;
    out["all-proxy"] = options.proxy;
    out["all-proxy-user"] = options.proxy_username;
    out["all-proxy-passwd"] = options.proxy_password;
    out["check-certificate"] = options.verify_ssl ? "true" : "false";
    out["max-tries"] = std::to_string(options.max_retries);
    out["retry-wait"] = std::to_string(options.retry_delay_seconds);
    out["timeout"] = std::to_string(options.timeout_seconds);
    out["max-connection-per-server"] = std::to_string(options.max_connections);
    out["min-split-size"] = std::to_string(options.min_segment_size);
    out["max-download-limit"] = std::to_string(options.speed_limit);
    out["split"] = std::to_string(options.max_connections);
    out["continue"] = options.resume_enabled ? "true" : "false";
    out["allow-overwrite"] = options.overwrite_existing ? "true" : "false";
    out["auto-file-renaming"] = options.auto_file_renaming ? "true" : "false";
    out["file-allocation"] = options.file_allocation;
    out["conditional-get"] = options.conditional_get ? "true" : "false";
    out["seed-ratio"] = std::to_string(options.seed_ratio);
    out["seed-time"] = std::to_string(options.seed_time_minutes);
    out["p2sp-share"] = options.p2sp_share;
    json headers = json::object();
    for (const auto& [key, value] : options.headers) {
        headers[key] = value;
    }
    out["header"] = std::move(headers);
    return out;
}

// aria2 tellWaiting/tellStopped 的 offset/num 分页语义
inline json paginate_items(const std::vector<json>& items, int offset, int num) {
    json out = json::array();
    const int start = std::max(0, offset);
    const int end = std::min<int>(static_cast<int>(items.size()), start + std::max(0, num));
    for (int i = start; i < end; ++i) {
        out.push_back(items[static_cast<std::size_t>(i)]);
    }
    return out;
}

inline json task_to_status_json(const falcon::DownloadTask& task) {
    json out;
    out["gid"] = task_id_to_gid(task.id());
    out["status"] = aria2_status_from_task(task);
    // Falcon 扩展字段：aria2 原生没有优先级，桌面客户端用它还原任务列表
    out["priority"] = static_cast<int>(task.get_priority());
    out["totalLength"] = std::to_string(task.total_bytes());
    out["completedLength"] = std::to_string(task.downloaded_bytes());
    out["downloadSpeed"] = std::to_string(task.speed());
    out["errorMessage"] = task.error_message();
    out["files"] = files_json(task.output_path(), task.total_bytes(),
                              task.downloaded_bytes(), task.url(), false);
    // Falcon 扩展字段：做种计量快照（BitTorrent；非 BT 任务恒零）
    const auto seed = task.get_seed_info();
    out["seedUploadedBytes"] = std::to_string(seed.uploaded_bytes);
    out["seedDownloadedBytes"] = std::to_string(seed.downloaded_bytes);
    out["seedTotalSize"] = std::to_string(seed.total_size);
    out["seededSeconds"] = seed.seeded_seconds;
    out["seedingActive"] = seed.seeding_active;
    out["seedStopReason"] = seed.stop_reason;
    return out;
}

#ifdef FALCON_HAS_SQLITE3
// 引擎内任务（内存态）转状态 JSON；历史记录转状态 JSON
inline json task_record_to_status_json(const TaskRecord& record) {
    json out;
    out["gid"] = task_id_to_gid(record.id);
    out["status"] = aria2_status_from_task_status(record.status);
    // storage 未持久化优先级，历史记录回默认 Normal
    out["priority"] = static_cast<int>(falcon::TaskPriority::Normal);
    out["totalLength"] = std::to_string(record.total_bytes);
    out["completedLength"] = std::to_string(record.downloaded_bytes);
    out["downloadSpeed"] = std::to_string(record.speed);
    out["errorMessage"] = record.error_message;
    out["files"] = files_json(record.output_path, record.total_bytes,
                              record.downloaded_bytes, record.url, false);
    return out;
}

// 收集 storage 中存在、但引擎当前不持有的记录（典型场景：重启后
// 终态任务只保留在数据库中，引擎恢复时跳过终态）。引擎内任务以
// 内存状态为准，通过 engine_ids 去重避免同一任务重复出现。
inline std::vector<TaskRecord> storage_extra_records(
    TaskStorage* storage,
    const std::vector<falcon::DownloadTask::Ptr>& engine_tasks,
    const std::vector<falcon::TaskStatus>& statuses) {
    std::unordered_set<falcon::TaskId> engine_ids;
    for (const auto& task : engine_tasks) {
        if (task) engine_ids.insert(task->id());
    }
    std::vector<TaskRecord> out;
    for (auto status : statuses) {
        for (auto& record : storage->get_tasks_by_status(status)) {
            if (engine_ids.count(record.id) == 0) {
                out.push_back(std::move(record));
            }
        }
    }
    return out;
}
#endif

inline json make_error(const json& id, int code, std::string message) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id.is_null() ? json(nullptr) : id},
        {"error", json{{"code", code}, {"message", std::move(message)}}},
    };
}

inline json make_result(const json& id, json result) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id.is_null() ? json(nullptr) : id},
        {"result", std::move(result)},
    };
}

inline bool validate_and_strip_token(json& params, const std::string& secret) {
    if (secret.empty()) {
        return true;
    }
    if (!params.is_array() || params.empty() || !params[0].is_string()) {
        return false;
    }
    const std::string token = params[0].get<std::string>();
    const std::string expected = "token:" + secret;
    if (token != expected) {
        return false;
    }
    params.erase(params.begin());
    return true;
}

inline void maybe_strip_token(json& params, const std::string& secret) {
    if (secret.empty()) return;
    if (!params.is_array() || params.empty() || !params[0].is_string()) return;
    const std::string token = params[0].get<std::string>();
    const std::string expected = "token:" + secret;
    if (token == expected) {
        params.erase(params.begin());
    }
}

} // namespace detail
} // namespace rpc
} // namespace daemon
} // namespace falcon
