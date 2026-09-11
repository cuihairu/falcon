/**
 * @file download_backend.hpp
 * @brief 下载后端抽象：进程内引擎 / daemon RPC 两种实现
 * @author Falcon Team
 * @date 2026-09-11
 */

#pragma once

#include <rpc/aria2_snapshots.hpp>
#include <rpc/json_rpc_client.hpp>

#include <falcon/download_options.hpp>
#include <falcon/types.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace falcon::desktop {

/// 新建任务的结果（同步返回；任务本身随后经快照轮询进入 UI）
struct AddTaskResult {
    bool ok = false;
    TaskId id = 0;
    std::string error;
};

/// 下载后端接口。所有方法在 DownloadService 的 worker 线程调用，
/// 实现无需额外的线程安全保证；查询方法允许阻塞（RPC 为网络往返）。
class IDownloadBackend {
public:
    virtual ~IDownloadBackend() = default;

    // ---- 任务控制 ----
    virtual AddTaskResult add_task(const std::string& url,
                                   const falcon::DownloadOptions& options,
                                   bool start_immediately) = 0;
    virtual bool pause_task(falcon::TaskId id) = 0;
    virtual bool resume_task(falcon::TaskId id) = 0;
    virtual bool remove_task(falcon::TaskId id) = 0;
    /// 返回清除的任务数
    virtual std::size_t remove_finished_tasks() = 0;
    virtual bool set_priority(falcon::TaskId id, falcon::TaskPriority priority) = 0;

    // ---- 全局设置 ----
    virtual void apply_global_settings(std::size_t max_concurrent_tasks,
                                       std::size_t global_speed_limit_bytes) = 0;

    // ---- 查询 ----
    /// 全量任务快照（active + waiting + stopped）；后端不可达时返回空列表
    virtual std::vector<falcon::daemon::rpc::TaskSnapshot> fetch_tasks() = 0;
    /// 全局统计；不可用时返回 nullopt
    virtual std::optional<falcon::daemon::rpc::GlobalStats> fetch_stats() = 0;
};

/// 进程内引擎后端：直接持有 DownloadEngine（原 desktop 行为）
std::unique_ptr<IDownloadBackend> make_inprocess_backend();

/// daemon RPC 后端：经 aria2 兼容 JSON-RPC 访问 falcon-daemon
std::unique_ptr<IDownloadBackend> make_daemon_rpc_backend(
    falcon::daemon::rpc::JsonRpcClientConfig config);

} // namespace falcon::desktop
