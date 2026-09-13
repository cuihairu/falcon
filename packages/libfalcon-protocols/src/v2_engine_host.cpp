/**
 * @file v2_engine_host.cpp
 * @brief V2 引擎宿主实现（惰性启动 + 排水停机）
 * @author Falcon Team
 * @date 2026-09-13
 */

#include <falcon/protocols/v2_engine_host.hpp>

#include <falcon/logger.hpp>

#include <algorithm>

namespace falcon {

V2EngineHost& V2EngineHost::instance() {
    static V2EngineHost host;
    return host;
}

V2EngineHost::~V2EngineHost() {
    shutdown_and_join();
}

void V2EngineHost::configure(const EngineConfigV2& config) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_) {
        FALCON_LOG_WARN_STREAM(
            "V2 引擎已启动，configure 不生效（下次启动时应用）");
        return;
    }
    pending_config_ = config;
}

std::shared_ptr<DownloadEngineV2> V2EngineHost::engine() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!engine_) {
            auto config = pending_config_;
            // 宿主语义强制：常驻引擎（wait_when_idle）+ 大并发槽位——
            // V1 TaskManager 是权威排队者，V2 侧不得二次排队
            config.wait_when_idle = true;
            config.max_concurrent_tasks =
                std::max<std::size_t>(config.max_concurrent_tasks, 64);
            engine_ = std::make_shared<DownloadEngineV2>(config);
            FALCON_LOG_INFO_STREAM("V2 引擎宿主已启动（HTTP 数据面，线程托管 run()）");
            run_thread_ = std::thread([engine = engine_]() { engine->run(); });
        }
    }
    // 等线程真正进入 run()（running_ 置位）再返回：run() 入口会复位
    // halt_requested_（实例复用语义），「创建后立即停机」的 shutdown
    // 若先于该复位点到达会被吞掉，run 线程将永久轮询、join 挂死
    std::shared_ptr<DownloadEngineV2> engine;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = engine_;
    }
    while (engine && !engine->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return engine;
}

std::shared_ptr<DownloadEngineV2> V2EngineHost::try_engine() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return engine_;
}

void V2EngineHost::shutdown_and_join(std::chrono::milliseconds drain_timeout) {
    std::shared_ptr<DownloadEngineV2> engine;
    std::thread run_thread;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine = engine_;
        if (!engine) {
            return;
        }
        // 线程移出成员在锁外 join（run() 不回调宿主，无锁依赖）
        run_thread = std::move(run_thread_);
        run_thread_ = {};
    }

    // 排水：全部组固化为 PAUSED（断点保存）或终态后再收引擎
    engine->pause_all();
    auto* group_man = engine->request_group_man();
    const auto deadline =
        std::chrono::steady_clock::now() + drain_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (group_man->all_settled()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    engine->shutdown();
    if (run_thread.joinable()) {
        run_thread.join();
    }

    FALCON_LOG_INFO_STREAM("V2 引擎宿主已停机");

    // halt 后的引擎不可复用：实例销毁，下次 engine() 重建
    std::lock_guard<std::mutex> lock(mutex_);
    if (engine_ == engine) {
        engine_.reset();
    }
}

}  // namespace falcon
