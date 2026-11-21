/**
 * @file http_commands_detail.hpp
 * @brief HTTP 命令 TU 间共享的内部辅助（socket 收口/错误码/镜像轮转/连接级重试构造/任务组终态收口）
 * @author Falcon Team
 * @date 2026-11-22
 */

#pragma once

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>      // _open/_write/_close（file-allocation 直写预分配）
#include <fcntl.h>   // _O_WRONLY/_O_BINARY
// Windows 没有 ssize_t，使用 SSIZE_T 或 ptrdiff_t
typedef SSIZE_T ssize_t;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <memory>
#include <string>

namespace falcon {
namespace protocols {
namespace detail {

inline void close_socket_fd(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
}

#ifdef _WIN32
// Winsock 的 socket 调用失败后不设置 errno，必须经 WSAGetLastError() 获取；
// 其错误码（如 WSAEWOULDBLOCK=10035）也与 BSD 的 EAGAIN/EWOULDBLOCK 不同，
// 以下辅助统一收口，避免各处直接比较 errno
inline int sock_errno() { return WSAGetLastError(); }
inline std::string sock_err_str(int err) { return "WSA error " + std::to_string(err); }
inline bool sock_would_block(int err) { return err == WSAEWOULDBLOCK; }
#else
inline int sock_errno() { return errno; }
inline std::string sock_err_str(int err) { return strerror(err); }
inline bool sock_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
#endif

/// 段级换源的无状态镜像轮转：下一镜像 = uris_ 中 failed_url 的下一个
///（取模）。failed_url 不在列表（重定向后 URL）时回落主镜像 uris_[0]。
/// 无需游标成员，不触碰 try_next_uri 的钳位语义
inline std::string mirror_after(const RequestGroup& group,
                         const std::string& failed_url) {
    const auto& uris = group.uris();
    if (uris.empty()) {
        return {};
    }
    for (std::size_t i = 0; i < uris.size(); ++i) {
        if (uris[i] == failed_url) {
            return uris[(i + 1) % uris.size()];
        }
    }
    return uris.front();
}

/// 构造连接级重试命令；返回 nullptr 表示不可重试：
/// - 多镜像任务（组 uris>1）：换下一镜像重试（不问 max_connections——
///   多源分发本就意图多连接，小文件不分段时连接失败同样可换源），
///   预算仍按 retry_count/max_retries 链
/// - 单 URL 多连接意图的任务不参与连接级重试（分段失败语义不同）
/// - 已达 max_retries（首连 + max_retries 次重试）
inline std::unique_ptr<Command> make_connection_retry(DownloadEngineV2* engine,
                                               TaskId task_id,
                                               const std::string& url,
                                               const DownloadOptions& options,
                                               int retry_count) {
    if (engine) {
        auto* man = engine->request_group_man();
        auto* group = man ? man->find_group(task_id) : nullptr;
        if (group && group->uris().size() > 1) {
            if (retry_count >= static_cast<int>(options.max_retries)) {
                return nullptr;
            }
            const std::string next_url = mirror_after(*group, url);
            if (next_url.empty()) return nullptr;
            FALCON_LOG_WARN_STREAM("连接失败，切换镜像重试 ("
                                  << (retry_count + 1) << "/"
                                  << options.max_retries << "): " << next_url);
            return std::make_unique<HttpRetryCommand>(task_id, next_url,
                                                      options, retry_count + 1);
        }
    }
    if (options.max_connections > 1) return nullptr;
    if (retry_count >= static_cast<int>(options.max_retries)) return nullptr;
    FALCON_LOG_WARN_STREAM("连接失败，调度延迟重试 ("
                          << (retry_count + 1) << "/" << options.max_retries
                          << "): " << url);
    return std::make_unique<HttpRetryCommand>(task_id, url, options,
                                              retry_count + 1);
}

/// 任务组终态收口（连接级失败重试耗尽时调用）。此前初始连接失败
/// 无人标终态：任务悬空 Downloading、all_completed 永不成立、
/// run() 无法退出。仅用于单连接路径（多段组由段失败聚合终态）
inline void fail_group_terminal(DownloadEngineV2* engine, TaskId task_id,
                         const std::string& reason) {
    if (!engine) return;
    auto* man = engine->request_group_man();
    auto* group = man ? man->find_group(task_id) : nullptr;
    if (!group) return;
    group->save_resume_now();  // 续传追踪中则固化断点（未追踪时空操作）
    group->set_error_message(reason);
    group->set_status(RequestGroupStatus::FAILED);
    if (auto task = group->download_task()) {
        if (task->error_message().empty()) {
            task->set_error(reason);
        }
        task->set_status(TaskStatus::Failed);
    }
}

} // namespace detail
} // namespace protocols
} // namespace falcon
