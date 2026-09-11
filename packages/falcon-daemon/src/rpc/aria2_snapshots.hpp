#pragma once

#include <falcon/event_listener.hpp>
#include <falcon/types.hpp>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace falcon::daemon::rpc {

/// 任务快照：桌面 UI 渲染一行任务所需的全部字段。
/// 与引擎对象解耦——进程内后端从 DownloadTask 填充，RPC 后端从
/// aria2 tell* JSON 填充，DownloadPage 只消费快照。
struct TaskSnapshot {
    falcon::TaskId id = 0;
    std::string url;
    std::string output_path;
    falcon::TaskStatus status = falcon::TaskStatus::Pending;
    double progress = 0.0;
    std::uint64_t total_bytes = 0;
    std::uint64_t downloaded_bytes = 0;
    std::uint64_t speed = 0;
    std::string error_message;
    falcon::TaskPriority priority = falcon::TaskPriority::Normal;

    bool is_finished() const noexcept;
};

/// 全局统计快照（getGlobalStat / 引擎统计 getter 的公共形状）
struct GlobalStats {
    std::uint64_t download_speed = 0;
    std::size_t active_tasks = 0;
    std::size_t waiting_tasks = 0;
    std::size_t stopped_tasks = 0;
    std::size_t total_tasks() const noexcept {
        return active_tasks + waiting_tasks + stopped_tasks;
    }
};

// ---- aria2 JSON → 快照（RPC 后端用） ----

/// tellStatus 单对象 → 快照；JSON 形状不符返回 nullopt
std::optional<TaskSnapshot> snapshot_from_status_json(const nlohmann::json& status);

/// tellActive/tellWaiting/tellStopped 的数组 → 快照列表
std::vector<TaskSnapshot> snapshots_from_status_array(const nlohmann::json& array);

/// getGlobalStat 对象 → 统计快照；无法解析时返回 nullopt
std::optional<GlobalStats> stats_from_global_stat_json(const nlohmann::json& stat);

/// aria2 状态字符串（"active"/"waiting"/"paused"/"complete"/"error"/"removed"）
/// → TaskStatus；无法识别返回 nullopt
std::optional<falcon::TaskStatus> task_status_from_aria2_string(const std::string& s);

/// gid 十六进制字符串 → TaskId
std::optional<falcon::TaskId> task_id_from_gid(const std::string& gid);

} // namespace falcon::daemon::rpc
