/**
 * @file v2_http_download_adapter.cpp
 * @brief V2 HTTP 下载适配器实现（worker 阻塞语义的桥接轮询）
 * @author Falcon Team
 * @date 2026-09-13
 */

#include "v2_http_download_adapter.hpp"

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/request_group.hpp>
#include <falcon/protocols/v2_engine_host.hpp>

#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace falcon {
namespace {

/// 桥接轮询粒度（update_progress 内建节流，实际下发频率取两者较大值）
constexpr auto kPollInterval = std::chrono::milliseconds(200);

}  // namespace

bool V2HttpDownloadAdapter::supports(const DownloadOptions& options) {
    if (!V2EngineHost::instance().v2_http_enabled()) {
        return false;
    }
    // curl 专属能力 → 回退 V1（同一任务两侧行为完全一致）
    if (parse_http_proxy(options).kind == HttpProxyKind::Unsupported) {
        return false;  // socks/https 代理与非法 proxy 表达式
    }
    if (!options.cookie_file.empty() || !options.cookie_jar.empty()) {
        return false;  // cookie 引擎（CURLOPT_COOKIEFILE/JAR）
    }
    if (!options.http_username.empty() || !options.http_password.empty()) {
        return false;  // HTTP 401 认证
    }
    if (!options.referer.empty()) {
        return false;  // Referer 头（防盗链语义），V2 不发送
    }
    return true;
}

V2HttpDownloadAdapter::V2HttpDownloadAdapter(DownloadTask::Ptr task)
    : task_(std::move(task)) {}

void V2HttpDownloadAdapter::sync_final_progress(const RequestGroup& group) {
    const auto progress = group.get_progress();
    task_->update_progress(progress.downloaded, progress.total,
                           progress.speed);
}

void V2HttpDownloadAdapter::run() {
    auto* host = &V2EngineHost::instance();
    auto engine = host->engine();
    auto* group_man = engine->request_group_man();

    const TaskId id = task_->id();

    // 组对齐：有 PAUSED 组（V1 resume → 重新 download()）则续跑；
    // 终态组（常驻引擎按周期回收终态组，metalink 桥接完成后的同 id
    // 阶段1 回落重注入等不会等周期）提前回收让同 id 立即可复用；否则
    // 注入新组（V1 id + 已确定的 output_path，两侧写同一文件）
    auto* group = group_man->find_group(id);
    if (group != nullptr &&
        group->status() == RequestGroupStatus::PAUSED) {
        if (!engine->resume_task(id)) {
            throw std::runtime_error("V2 引擎恢复任务失败: " +
                                     std::to_string(id));
        }
    } else if (group == nullptr ||
               group->status() == RequestGroupStatus::COMPLETED ||
               group->status() == RequestGroupStatus::FAILED ||
               group->status() == RequestGroupStatus::REMOVED) {
        if (group != nullptr) {
            group_man->purge_finished_groups();
            group = nullptr;  // 被回收组在锁外析构,指针立即失效
        }
        const TaskId injected = engine->add_download_as(
            id, {task_->url()}, task_->options(), task_->output_path());
        if (injected == INVALID_TASK_ID) {
            throw std::runtime_error(
                "V2 引擎无法接受任务（ID 冲突或 URL 无效）");
        }
    } else {
        // 同 id 组已存在且非暂停/终态：V1 同一任务不会并发 download()
        throw std::runtime_error("V2 任务组状态异常（非暂停态已存在）");
    }

    // 桥接轮询：V1 侧控制优先，组状态为主判据
    while (true) {
        std::this_thread::sleep_for(kPollInterval);

        const auto v1_status = task_->status();
        if (v1_status == TaskStatus::Paused) {
            // 兜底同步（正常路径 HttpHandler::pause 已转发，幂等）
            engine->pause_task(id);
            return;
        }
        if (v1_status == TaskStatus::Cancelled) {
            engine->cancel_task(id);
            return;
        }

        // 每轮重取：宿主停机后引擎实例会被销毁，不长期持有
        engine = host->try_engine();
        if (!engine) {
            throw std::runtime_error("V2 引擎已停机");
        }
        group_man = engine->request_group_man();
        group = group_man->find_group(id);
        if (group == nullptr) {
            throw std::runtime_error("V2 任务组丢失");
        }

        switch (group->status()) {
        case RequestGroupStatus::COMPLETED:
            // 终态先同步最终进度（200ms 粒度下最后一次轮询可能落在
            // 终态分支——完成/暂停任务必须携带完整 total/downloaded）
            sync_final_progress(*group);
            // 成品已由 temp_extension 原子改名发布
            task_->set_status(TaskStatus::Completed);
            return;
        case RequestGroupStatus::FAILED:
            // 失败同样收口进度（半程进度是错误报告的上下文）
            sync_final_progress(*group);
            // throw → V1 worker catch：set_error（on_error）+
            // set_status(Failed)（on_status_changed），与 V1 序列一致
            throw std::runtime_error(
                group->error_message().empty() ? "V2 引擎下载失败"
                                               : group->error_message());
        case RequestGroupStatus::PAUSED:
            // V2 侧被暂停（如宿主停机 pause_all）：V1 状态对齐后挂起，
            // resume 时经 download() 重新进入续跑
            sync_final_progress(*group);
            task_->set_status(TaskStatus::Paused);
            return;
        default:
            // WAITING/ACTIVE/REMOVED：桥接进度（组内部进度聚合）
            const auto progress = group->get_progress();
            task_->update_progress(progress.downloaded, progress.total,
                                   progress.speed);
            break;
        }
    }
}

}  // namespace falcon
