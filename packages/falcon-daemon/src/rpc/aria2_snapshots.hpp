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

    // 做种计量快照（Falcon 扩展字段；非 BT 任务/历史记录恒零）
    std::uint64_t seed_uploaded_bytes = 0;
    std::uint64_t seed_downloaded_bytes = 0;
    std::uint64_t seed_total_size = 0;
    double seeded_seconds = 0.0;
    bool seeding_active = false;
    /// 做种停止原因："" 未停止/未做种；"limit_reached" 达标自动停止；
    /// "manual" 用户手动停止（旧版 daemon 缺省空串）
    std::string seed_stop_reason;

    /// 做种份额比：uploaded / max(downloaded, total_size)；分母 0 返 0
    /// （与 seed_policy 的 ratio 分母同式）
    [[nodiscard]] double seed_ratio() const noexcept {
        const std::uint64_t denom =
            seed_downloaded_bytes > seed_total_size ? seed_downloaded_bytes
                                                    : seed_total_size;
        if (denom == 0) return 0.0;
        return static_cast<double>(seed_uploaded_bytes) /
               static_cast<double>(denom);
    }

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
