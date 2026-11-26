/**
 * @file http_retry_commands.cpp
 * @brief HttpSegmentRetryCommand 与 HttpRetryCommand 实现（段级换源重试与连接级重试命令）
 * @author Falcon Team
 * @date 2026-11-22
 */

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#include "http_commands_detail.hpp"

#include <errno.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>
#include <cstring>
#include <sstream>
#include <iostream>
#include <cctype>
#include <memory>

namespace falcon {
using falcon::protocols::detail::mirror_after;
using falcon::protocols::detail::fail_group_terminal;

//==============================================================================
// HttpSegmentRetryCommand 实现
//==============================================================================

HttpSegmentRetryCommand::HttpSegmentRetryCommand(
    TaskId task_id,
    std::string url,
    const DownloadOptions& options,
    SegmentId segment_id,
    Bytes offset,
    Bytes length,
    std::string if_range,
    int retry_count)
    : AbstractCommand(task_id)
    , url_(std::move(url))
    , options_(options)
    , segment_id_(segment_id)
    , offset_(offset)
    , length_(length)
    , if_range_(std::move(if_range))
    , retry_count_(retry_count)
    , retry_wait_(std::chrono::seconds(options.retry_delay_seconds))
    , retry_at_(std::chrono::steady_clock::now() + retry_wait_)
{
}

const char* HttpSegmentRetryCommand::name() const {
    return "HttpSegmentRetry";
}

bool HttpSegmentRetryCommand::execute(DownloadEngineV2* engine) {
    // 组消失/已暂停/已终态：静默退出（重试链随之终结；暂停后的恢复
    // 由 resume 重新调度，不沿旧重试链续跑）
    if (engine) {
        auto* group_man = engine->request_group_man();
        auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (!group || group->status() == RequestGroupStatus::PAUSED ||
            group->status() == RequestGroupStatus::REMOVED ||
            group->status() == RequestGroupStatus::COMPLETED ||
            group->status() == RequestGroupStatus::FAILED) {
            return handle_result(ExecutionResult::OK);
        }
    } else {
        return handle_result(ExecutionResult::OK);
    }

    // 未到重试时刻：以 NEED_RETRY 回队轮询（不注册 socket 事件），
    // 由引擎 poll 节奏驱动到点检查——不阻塞引擎线程
    if (std::chrono::steady_clock::now() < retry_at_) {
        return handle_result(ExecutionResult::NEED_RETRY);
    }

    FALCON_LOG_INFO_STREAM("段 " << segment_id_ << " 换源重试 ("
                          << retry_count_ << "): " << url_);

    // 重建该段的 Range 连接（响应经 determine_download_strategy 的段
    // 分支路由——显式标记使段 0 的重试响应与暂停恢复的初始连接区分）
    auto conn = std::make_unique<HttpInitiateConnectionCommand>(
        get_task_id(), url_, options_);
    conn->set_retry_count(retry_count_);
    conn->set_segment_retry(true);
    conn->set_range(segment_id_, offset_, length_);
    if (!if_range_.empty()) {
        conn->set_if_range(if_range_);
    }
    schedule_next(engine, std::move(conn));
    return handle_result(ExecutionResult::OK);
}

bool HttpSegmentRetryCommand::schedule_retry(DownloadEngineV2* engine,
                                             TaskId task_id,
                                             SegmentId segment_id,
                                             Bytes plan_offset,
                                             Bytes plan_length,
                                             const std::string& failed_url) {
    if (!engine) {
        return false;
    }
    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(task_id) : nullptr;
    if (!group) {
        return false;
    }
    const auto st = group->status();
    if (st == RequestGroupStatus::PAUSED || st == RequestGroupStatus::REMOVED ||
        st == RequestGroupStatus::COMPLETED || st == RequestGroupStatus::FAILED) {
        return false;
    }
    if (!group->is_multi_segment() || group->uris().empty()) {
        return false;
    }

    // 预算：每段 options.max_retries 次（首连不计）。计数在这里递增，
    // 重试命令本身无预算判定——finish_segment 只在预算耗尽时由调用方
    // 走既有失败收口调用一次
    const int attempt = group->increment_segment_retry(segment_id);
    const int max_retries = static_cast<int>(group->options().max_retries);
    if (attempt > max_retries) {
        FALCON_LOG_WARN_STREAM("段 " << segment_id << " 换源重试预算耗尽 ("
                              << max_retries << "): task=" << task_id);
        return false;
    }

    // 归一化到段计划坐标：有续传计划以计划为准（恢复段连接的起点是
    // 断点而非计划起点，不能直接叠加）；无计划时调用方坐标即计划坐标
    if (group->has_resume_state()) {
        const auto plan = group->resume_plan();
        if (segment_id < plan.segments.size()) {
            plan_offset = plan.segments[segment_id].offset;
            plan_length = plan.segments[segment_id].length;
        }
    }
    const Bytes progress = group->segment_progress(segment_id);
    if (progress >= plan_length) {
        return false;  // 段已收满：无重试意义（防御）
    }

    const std::string next_url = mirror_after(*group, failed_url);
    if (next_url.empty()) {
        return false;
    }

    // If-Range 是主镜像（uris_[0]，ETag 的归属者）的验证值：轮转结果
    // 仍为主镜像才附带，非主镜像不带（内容一致性由 206 校验兜底）
    std::string if_range;
    if (next_url == group->uris().front() && group->has_resume_state()) {
        if_range = group->resume_if_range();
    }

    FALCON_LOG_INFO_STREAM("段 " << segment_id << " 失败，换源重试 "
                          << attempt << "/" << max_retries << ": " << next_url
                          << "（断点 " << progress << "/" << plan_length << "）");

    schedule_next(engine, std::make_unique<HttpSegmentRetryCommand>(
                              task_id, next_url, group->options(), segment_id,
                              plan_offset + progress, plan_length - progress,
                              std::move(if_range), attempt));
    return true;
}

//==============================================================================
// HttpRetryCommand 实现
//==============================================================================

//==============================================================================
// 超时清理的段级换源重试（C1：段命令超时不连坐整组）
//==============================================================================

bool HttpDownloadCommand::retry_expired_segment(DownloadEngineV2* engine) {
    return HttpSegmentRetryCommand::schedule_retry(engine, get_task_id(),
                                                   segment_id_, offset_,
                                                   length_, source_url_);
}

bool HttpResponseCommand::retry_expired_segment(DownloadEngineV2* engine) {
    return HttpSegmentRetryCommand::schedule_retry(engine, get_task_id(),
                                                   segment_id_, range_offset_,
                                                   range_length_, source_url_);
}

bool HttpInitiateConnectionCommand::retry_expired_segment(DownloadEngineV2* engine) {
    // 带 Range 的初始连接是恢复/段重试连接（承载第一个未完成段的
    // 收尾），段级换源语义匹配；无 Range 的全新初始连接走整组失败
    if (!has_range_) {
        return false;
    }
    return HttpSegmentRetryCommand::schedule_retry(engine, get_task_id(),
                                                   range_segment_id_,
                                                   range_offset_,
                                                   range_length_, url_);
}

HttpRetryCommand::HttpRetryCommand(
    TaskId task_id,
    const std::string& url,
    const DownloadOptions& options,
    int retry_count)
    : AbstractCommand(task_id)
    , url_(url)
    , options_(options)
    , retry_count_(retry_count)
    , max_retries_(static_cast<int>(options.max_retries))
    , retry_wait_(std::chrono::seconds(options.retry_delay_seconds))
    , retry_at_(std::chrono::steady_clock::now() + retry_wait_)
{
}

const char* HttpRetryCommand::name() const {
    return "HttpRetry";
}

bool HttpRetryCommand::execute(DownloadEngineV2* engine) {
    // 已暂停：静默退出，不再续建重试链
    if (engine) {
        auto* group_man = engine->request_group_man();
        auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (group && group->status() == RequestGroupStatus::PAUSED) {
            return handle_result(ExecutionResult::OK);
        }
    }

    if (!should_retry()) {
        FALCON_LOG_ERROR_STREAM("达到最大重试次数: " << max_retries_);
        fail_group_terminal(engine, get_task_id(),
                            "Connection failed after " +
                                std::to_string(max_retries_) + " retries");
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    // 未到重试时刻：以 NEED_RETRY 回队轮询（不注册 socket 事件），
    // 由引擎 poll 节奏驱动到点检查。旧实现在引擎事件循环线程里
    // sleep_for(retry_wait_)——一个任务重试时整个引擎所有任务与
    // socket 事件停摆
    const auto now = std::chrono::steady_clock::now();
    if (now < retry_at_) {
        return handle_result(ExecutionResult::NEED_RETRY);
    }

    FALCON_LOG_INFO_STREAM("重试下载 (" << retry_count_ << "/" << max_retries_ << "): " << url_);

    if (engine) {
        // 创建新的连接命令重试下载（携带重试计数供失败时继续链式判定）
        auto next_cmd = std::make_unique<HttpInitiateConnectionCommand>(
            get_task_id(), url_, options_);
        next_cmd->set_retry_count(retry_count_);
        // 连接级重试也可能发生在断点续传响应阶段（Range 已带、响应头
        // 未到即断连）——重发请求必须原样携带续传范围与 If-Range，
        // 否则服务器回 200 全量，进度归零还会覆盖已下载数据
        auto* group_man = engine->request_group_man();
        auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (group) {
            // 恢复初始连接（多段组 + 续传状态）失败后重建仍带组内
            // 续传 Range——恢复标记必须重打，否则 k>0 形态的响应按
            // 段路由跳过其余段重建（永挂根因经由重试链再入）
            if (apply_group_resume_range(*next_cmd, *group) &&
                group->is_multi_segment()) {
                next_cmd->set_resume_initial(true);
            }
            // If-Range 是主镜像（uris_[0]，ETag 的归属者）的验证值：
            // 多镜像轮转后重试连接落在非主镜像时必须剥离（续传内容
            // 一致性由 206 校验兜底，误带会造成有效续传被拒）
            if (group->has_resume_state() && !group->uris().empty() &&
                url_ != group->uris().front()) {
                next_cmd->set_if_range({});
            }
        }
        schedule_next(engine, std::move(next_cmd));
    }

    return handle_result(ExecutionResult::OK);
}

} // namespace falcon
