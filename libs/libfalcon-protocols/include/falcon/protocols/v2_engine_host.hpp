/**
 * @file v2_engine_host.hpp
 * @brief V2 引擎宿主——进程级共享引擎实例（V1 契约下的 HTTP 数据面）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * V2 引擎以 V1 契约下的 HTTP 数据面接入生产：V1 引擎/TaskManager/
 * 事件/持久化全不动，HttpHandler::download() 按进程级开关分叉到本
 * 宿主持有的共享 V2 引擎实例（逐任务经 V2HttpDownloadAdapter 桥接，
 * 可回退 curl），默认关闭。
 */

#pragma once

#include <falcon/protocols/download_engine_v2.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

namespace falcon {

/**
 * @brief 进程级 V2 引擎宿主（单例）
 *
 * 生命周期：
 * - engine() 惰性启动：首次调用创建引擎并以专用线程驱动 run()
 *   （wait_when_idle 强置 true——引擎跨任务存活，直到显式停机）
 * - shutdown_and_join() 排水停机：pause_all → 轮询活跃组归零 →
 *   shutdown → join run 线程。run() 退出后引擎实例不复用（halt 标志
 *   永置位），实例销毁，下次 engine() 重新惰性启动
 * - 析构兜底 shutdown_and_join()
 *
 * 停机契约：shutdown_and_join 必须在所有 download() 调用者（V1
 * worker 线程）返回之后调用。宿主侧时序为 TaskManager pause_all →
 * 桥接 worker 收到 Paused 转发 pause_task 并返回 → join worker →
 * 本方法。
 */
class V2EngineHost {
public:
    /// 进程单例
    static V2EngineHost& instance();

    V2EngineHost(const V2EngineHost&) = delete;
    V2EngineHost& operator=(const V2EngineHost&) = delete;

    /// 进程级 HTTP 引擎开关（默认 false——V1 curl 数据面）
    void set_v2_http_enabled(bool enabled) { http_enabled_.store(enabled); }
    [[nodiscard]] bool v2_http_enabled() const { return http_enabled_.load(); }

    /// 引擎启动前的配置（幂等覆盖；引擎已启动后调用不生效并告警）
    void configure(const EngineConfigV2& config);

    /// 惰性启动并返回共享引擎（首次调用启动专用 run 线程）
    std::shared_ptr<DownloadEngineV2> engine();

    /// 已启动的引擎（不触发惰性启动；未启动返回 nullptr）
    [[nodiscard]] std::shared_ptr<DownloadEngineV2> try_engine() const;

    /// 排水停机：pause_all → 轮询活跃组归零（≤ drain_timeout，组应
    /// 固化为 PAUSED 并保存断点）→ shutdown → join run 线程；排水
    /// 超时仍直接 shutdown（run() 退出自带 fd 排水）。
    void shutdown_and_join(
        std::chrono::milliseconds drain_timeout = std::chrono::milliseconds(2000));

private:
    V2EngineHost() = default;
    ~V2EngineHost();

    mutable std::mutex mutex_;
    EngineConfigV2 pending_config_;
    std::shared_ptr<DownloadEngineV2> engine_;
    std::thread run_thread_;
    std::atomic<bool> http_enabled_{false};
};

}  // namespace falcon
