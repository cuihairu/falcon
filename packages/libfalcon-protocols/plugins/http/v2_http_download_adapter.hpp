/**
 * @file v2_http_download_adapter.hpp
 * @brief V1 契约下的 V2 HTTP 数据面桥接（逐任务可回退 curl 的适配层）
 * @author Falcon Team
 * @date 2026-09-13
 */

#pragma once

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>

namespace falcon {

/**
 * @brief V2 HTTP 下载适配器
 *
 * HttpHandler::download() 在开关开启且 options 无 curl 专属能力时，
 * 把本次下载桥接到 V2EngineHost 的共享 V2 引擎：
 * - run() 保持 V1 worker 线程阻塞语义（可重入——V1 resume 即重新
 *   download()）；成功 set_status(Completed)，失败 throw——V1 worker
 *   的 catch 统一 set_error + Failed，事件序列与 V1 逐一对齐
 * - 进度经 DownloadTask::update_progress 桥接（节流内建）
 * - 组状态为主判据（V2 引擎不写 V1 task）；V1 侧 pause/cancel 优先：
 *   检测到即转发 pause_task/cancel_task 并返回（PAUSED 组保留供
 *   resume 续跑，取消组由引擎收尾）
 */
class V2HttpDownloadAdapter {
public:
    /// 回退表：开关开启且 options 无 curl 专属能力时才走 V2 数据面
    static bool supports(const DownloadOptions& options);

    explicit V2HttpDownloadAdapter(DownloadTask::Ptr task);
    ~V2HttpDownloadAdapter() = default;

    V2HttpDownloadAdapter(const V2HttpDownloadAdapter&) = delete;
    V2HttpDownloadAdapter& operator=(const V2HttpDownloadAdapter&) = delete;

    /// 阻塞桥接（V1 worker 线程调用；失败抛 std::runtime_error）
    void run();

private:
    DownloadTask::Ptr task_;  // V1 任务（状态权威归 V1 侧）
};

}  // namespace falcon
