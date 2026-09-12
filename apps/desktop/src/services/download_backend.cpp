/**
 * @file download_backend.cpp
 * @brief IDownloadBackend 两个实现：进程内引擎 / daemon RPC
 * @author Falcon Team
 * @date 2026-09-11
 */

#include "download_backend.hpp"

#include <rpc/websocket_rpc_client.hpp>

#include <falcon/download_engine.hpp>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <mutex>
#include <utility>

namespace falcon::desktop {

namespace {

// ============================================================================
// DownloadTask → TaskSnapshot（进程内后端专用；RPC 后端走 aria2 JSON 转换）
// ============================================================================

falcon::daemon::rpc::TaskSnapshot snapshot_from_download_task(
    const falcon::DownloadTask& task) {
    falcon::daemon::rpc::TaskSnapshot snap;
    snap.id = task.id();
    snap.url = task.url();
    snap.output_path = task.output_path();
    snap.status = task.status();
    snap.progress = static_cast<double>(task.progress());
    snap.total_bytes = task.total_bytes();
    snap.downloaded_bytes = task.downloaded_bytes();
    snap.speed = static_cast<std::uint64_t>(task.speed());
    snap.error_message = task.error_message();
    snap.priority = task.get_priority();
    return snap;
}

// ============================================================================
// InProcessBackend
// ============================================================================

class InProcessBackend final : public IDownloadBackend {
public:
    InProcessBackend() : engine_(std::make_unique<falcon::DownloadEngine>()) {}
    ~InProcessBackend() override = default;

    AddTaskResult add_task(const std::string& url,
                           const falcon::DownloadOptions& options,
                           bool start_immediately) override {
        AddTaskResult result;
        auto task = engine_->add_task(url, options);
        if (!task) {
            result.error = "URL is not supported";
            return result;
        }
        result.id = task->id();
        if (start_immediately) {
            (void)engine_->start_task(result.id);
        }
        result.ok = true;
        return result;
    }

    bool pause_task(falcon::TaskId id) override { return engine_->pause_task(id); }
    bool resume_task(falcon::TaskId id) override { return engine_->resume_task(id); }
    bool remove_task(falcon::TaskId id) override { return engine_->remove_task(id); }

    std::size_t remove_finished_tasks() override {
        return engine_->remove_finished_tasks();
    }

    bool set_priority(falcon::TaskId id, falcon::TaskPriority priority) override {
        return engine_->adjust_task_priority(id, priority);
    }

    void apply_global_settings(std::size_t max_concurrent_tasks,
                               std::size_t global_speed_limit_bytes) override {
        engine_->set_max_concurrent_tasks(max_concurrent_tasks);
        engine_->set_global_speed_limit(global_speed_limit_bytes);
    }

    std::vector<falcon::daemon::rpc::TaskSnapshot> fetch_tasks() override {
        std::vector<falcon::daemon::rpc::TaskSnapshot> out;
        out.reserve(engine_->get_total_task_count());
        for (const auto& task : engine_->get_all_tasks()) {
            if (task) {
                out.push_back(snapshot_from_download_task(*task));
            }
        }
        return out;
    }

    std::optional<falcon::daemon::rpc::GlobalStats> fetch_stats() override {
        falcon::daemon::rpc::GlobalStats stats;
        stats.download_speed = static_cast<std::uint64_t>(engine_->get_total_speed());
        std::size_t waiting = 0;
        std::size_t stopped = 0;
        for (const auto& task : engine_->get_all_tasks()) {
            if (!task) {
                continue;
            }
            switch (task->status()) {
                case falcon::TaskStatus::Pending:
                    ++waiting;
                    break;
                case falcon::TaskStatus::Completed:
                case falcon::TaskStatus::Failed:
                case falcon::TaskStatus::Cancelled:
                    ++stopped;
                    break;
                default:
                    break;  // Downloading/Preparing 计入 active
            }
        }
        stats.active_tasks = engine_->get_active_task_count();
        stats.waiting_tasks = waiting;
        stats.stopped_tasks = stopped;
        return stats;
    }

private:
    std::unique_ptr<falcon::DownloadEngine> engine_;
};

// ============================================================================
// DaemonRpcBackend
// ============================================================================

std::string gid_for(falcon::TaskId id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
    return buf;
}

/// DownloadOptions → aria2 addUri options。只填非空项，空字符串值会被
/// aria2 语义拒绝。
nlohmann::json options_to_aria2(const falcon::DownloadOptions& options) {
    nlohmann::json out = nlohmann::json::object();
    if (!options.output_directory.empty()) {
        out["dir"] = options.output_directory;
    }
    if (!options.output_filename.empty()) {
        out["out"] = options.output_filename;
    }
    if (options.max_connections > 0) {
        // aria2 的 max-connection-per-server 上限为 16
        const auto capped = std::min<std::size_t>(options.max_connections, 16);
        out["max-connection-per-server"] = std::to_string(capped);
        out["split"] = std::to_string(capped);
    }
    if (!options.user_agent.empty()) {
        out["user-agent"] = options.user_agent;
    }
    if (!options.referer.empty()) {
        out["referer"] = options.referer;
    }
    out["max-download-limit"] =
        options.speed_limit > 0 ? std::to_string(options.speed_limit) : "0";
    const auto cookie = options.headers.find("Cookie");
    if (cookie != options.headers.end() && !cookie->second.empty()) {
        out["header"] = nlohmann::json::array({"Cookie: " + cookie->second});
    }
    return out;
}

class DaemonRpcBackend final : public IDownloadBackend {
public:
    explicit DaemonRpcBackend(falcon::daemon::rpc::JsonRpcClientConfig config)
        : client_(std::move(config)) {
        // 事件流驱动：daemon 的全部通知（aria2.onDownloadStart/Pause/Complete/
        // Error/Stop + falcon.onProgress）都意味着快照有变化，唤醒调用方刷新。
        // 与 set_wake_callback 互斥（client 侧保证），析构时序安全。
        client_.set_notification_handler(
            [this](const std::string& method, const std::string& params_json) {
                (void)method;
                (void)params_json;
                std::lock_guard<std::mutex> lock(dispatch_mutex_);
                if (wake_callback_) wake_callback_();
            });
    }

    /// set_wake_callback 与通知分发互斥：返回后读线程不会再进入旧回调，
    /// DownloadService 可据此安全地先行析构其成员
    void set_wake_callback(std::function<void()> callback) override {
        std::lock_guard<std::mutex> lock(dispatch_mutex_);
        wake_callback_ = std::move(callback);
    }

    ~DaemonRpcBackend() override {
        // 成员按声明逆序析构会先销毁 wake_callback_ 再析构 client_——
        // 若读线程此时正在分发通知会调用已析构的回调。先断开连接
        // （join 读线程）保证分发静止。
        client_.disconnect();
    }

    AddTaskResult add_task(const std::string& url,
                           const falcon::DownloadOptions& options,
                           bool start_immediately) override {
        AddTaskResult result;
        (void)start_immediately;  // aria2.addUri 默认即启动（除非 forcePause 选项）
        falcon::daemon::rpc::JsonRpcError err;
        auto gid = client_.add_uri({url}, options_to_aria2(options), &err);
        if (!gid) {
            result.error = err.message.empty() ? "addUri failed" : err.message;
            return result;
        }
        auto id = falcon::daemon::rpc::task_id_from_gid(*gid);
        if (!id) {
            result.error = "daemon returned invalid gid: " + *gid;
            return result;
        }
        result.ok = true;
        result.id = *id;
        return result;
    }

    bool pause_task(falcon::TaskId id) override {
        falcon::daemon::rpc::JsonRpcError err;
        return static_cast<bool>(client_.pause(gid_for(id), &err));
    }

    bool resume_task(falcon::TaskId id) override {
        falcon::daemon::rpc::JsonRpcError err;
        return static_cast<bool>(client_.unpause(gid_for(id), &err));
    }

    bool remove_task(falcon::TaskId id) override {
        falcon::daemon::rpc::JsonRpcError err;
        return static_cast<bool>(client_.remove(gid_for(id), &err));
    }

    std::size_t remove_finished_tasks() override {
        // aria2 不返回清除数量；快照刷新后列表自然收敛
        (void)client_.purge_download_result();
        return 0;
    }

    bool set_priority(falcon::TaskId id, falcon::TaskPriority priority) override {
        falcon::daemon::rpc::JsonRpcError err;
        return static_cast<bool>(client_.change_priority(
            gid_for(id), static_cast<int>(priority), &err));
    }

    void apply_global_settings(std::size_t max_concurrent_tasks,
                               std::size_t global_speed_limit_bytes) override {
        nlohmann::json options = nlohmann::json::object();
        options["max-concurrent-downloads"] = std::to_string(max_concurrent_tasks);
        options["max-overall-download-limit"] =
            global_speed_limit_bytes > 0 ? std::to_string(global_speed_limit_bytes) : "0";
        (void)client_.change_global_option(options);
    }

    std::vector<falcon::daemon::rpc::TaskSnapshot> fetch_tasks() override {
        std::vector<falcon::daemon::rpc::TaskSnapshot> out;
        falcon::daemon::rpc::JsonRpcError err;

        if (auto active = client_.tell_active(&err)) {
            auto snaps = falcon::daemon::rpc::snapshots_from_status_array(*active);
            out.insert(out.end(), snaps.begin(), snaps.end());
        }
        if (auto waiting = client_.tell_waiting(&err)) {
            auto snaps = falcon::daemon::rpc::snapshots_from_status_array(*waiting);
            out.insert(out.end(), snaps.begin(), snaps.end());
        }
        if (auto stopped = client_.tell_stopped(&err)) {
            auto snaps = falcon::daemon::rpc::snapshots_from_status_array(*stopped);
            out.insert(out.end(), snaps.begin(), snaps.end());
        }
        return out;
    }

    std::optional<falcon::daemon::rpc::GlobalStats> fetch_stats() override {
        falcon::daemon::rpc::JsonRpcError err;
        auto stat = client_.get_global_stat(&err);
        if (!stat) {
            return std::nullopt;
        }
        return falcon::daemon::rpc::stats_from_global_stat_json(*stat);
    }

private:
    // WebSocket 单连接承载全部 RPC（请求/响应 + 通知）；断线下次 call 自动重连
    falcon::daemon::rpc::WebSocketRpcClient client_;
    std::mutex dispatch_mutex_;  // wake_callback_ 与通知分发互斥
    std::function<void()> wake_callback_;
};

} // namespace

std::unique_ptr<IDownloadBackend> make_inprocess_backend() {
    return std::make_unique<InProcessBackend>();
}

std::unique_ptr<IDownloadBackend> make_daemon_rpc_backend(
    falcon::daemon::rpc::JsonRpcClientConfig config) {
    return std::make_unique<DaemonRpcBackend>(std::move(config));
}

} // namespace falcon::desktop
