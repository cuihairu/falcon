/**
 * @file http_download_command.cpp
 * @brief HttpDownloadCommand 实现（数据接收/分块解码/写缓冲/发布与预分配）
 * @author Falcon Team
 * @date 2026-11-22
 */

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/protocols/fs_rename.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

// OpenSSL 头文件（用于 TLS 支持）
#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#endif

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
using falcon::protocols::detail::close_socket_fd;
using falcon::protocols::detail::sock_errno;
using falcon::protocols::detail::sock_err_str;
using falcon::protocols::detail::sock_would_block;

namespace {

#ifdef _WIN32
// memmem 不是标准 C/C++，Windows 没有，提供简单实现
static const void* memmem_alt(const void* haystack, size_t haystack_len,
                              const void* needle, size_t needle_len) {
    if (needle_len == 0) return haystack;
    if (haystack_len < needle_len) return nullptr;

    const char* haystack_str = static_cast<const char*>(haystack);
    const char* needle_str = static_cast<const char*>(needle);

    for (size_t i = 0; i <= haystack_len - needle_len; ++i) {
        if (std::memcmp(haystack_str + i, needle_str, needle_len) == 0) {
            return haystack_str + i;
        }
    }
    return nullptr;
}

#define memmem(haystack, haystack_len, needle, needle_len) \
    memmem_alt(haystack, haystack_len, needle, needle_len)
#endif
} // namespace

//==============================================================================
// 输出文件预分配（DownloadOptions::file_allocation 消费点）
//==============================================================================

namespace {

/// 预分配策略（aria2 --file-allocation 同语义）
enum class AllocMode { None, Trunc, Falloc, Prealloc };

/// "none" 与一切未知值都按稀疏处理（现状行为，不分配）
AllocMode parse_file_allocation(const std::string& mode) {
    if (mode == "trunc") return AllocMode::Trunc;
    if (mode == "falloc") return AllocMode::Falloc;
    if (mode == "prealloc") return AllocMode::Prealloc;
    return AllocMode::None;
}

/// 写零块真实占盘（prealloc 语义，全平台一致）。输出文件已被
/// ofstream 以 trunc 创建，这里独立 fd 写入互不干扰
bool write_zero_fill(const std::string& path, uint64_t total,
                     std::string& err) {
#ifdef _WIN32
    const int fd = ::_open(path.c_str(), _O_WRONLY | _O_BINARY);
#else
    const int fd = ::open(path.c_str(), O_WRONLY);
#endif
    if (fd < 0) {
        err = "open failed: " + std::string(std::strerror(errno));
        return false;
    }
    // 零块写满 total；写失败（磁盘满 ENOSPC 最常见）立即收口
    static char zero_block[256 * 1024] = {0};
    uint64_t remaining = total;
    bool ok = true;
    while (remaining > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            remaining < sizeof(zero_block) ? remaining : sizeof(zero_block));
#ifdef _WIN32
        const int n = ::_write(fd, zero_block, static_cast<unsigned>(chunk));
#else
        const ssize_t n = ::write(fd, zero_block, chunk);
#endif
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            err = "zero-fill write failed: " +
                  std::string(std::strerror(errno));
            ok = false;
            break;
        }
        remaining -= static_cast<uint64_t>(n);
    }
#ifdef _WIN32
    ::_close(fd);
#else
    ::close(fd);
#endif
    return ok;
}

/// 按策略把输出文件预分配到 total 字节
/// @return false 分配失败（err 携带原因；磁盘满是典型）
bool preallocate_file(const std::string& path, uint64_t total,
                      AllocMode mode, std::string& err) {
    if (mode == AllocMode::Prealloc) {
        return write_zero_fill(path, total, err);
    }
    if (mode == AllocMode::Trunc) {
        // ftruncate 语义（稀疏：st_size == total 但不占块），三平台一致
        std::error_code ec;
        std::filesystem::resize_file(path, total, ec);
        if (ec) {
            err = ec.message();
            return false;
        }
        return true;
    }
    // AllocMode::Falloc：文件系统层快速分配
#if defined(__APPLE__)
    // F_PREALLOCATE 只保留空间不改文件大小，预分配后补一次 resize
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        err = "open failed: " + std::string(std::strerror(errno));
        return false;
    }
    fstore_t fst = {};
    fst.fst_flags = F_ALLOCATECONTIG;
    fst.fst_posmode = F_PEOFPOSMODE;
    fst.fst_length = static_cast<off_t>(total);
    int rc = ::fcntl(fd, F_PREALLOCATE, &fst);
    if (rc < 0) {
        // 连续空间不足：允许碎片化分配再试一次
        fst.fst_flags = F_ALLOCATEALL;
        rc = ::fcntl(fd, F_PREALLOCATE, &fst);
    }
    if (rc >= 0) {
        // ftruncate 伸出文件尾至 total（F_PREALLOCATE 后此调用只改尺寸）
        if (::ftruncate(fd, static_cast<off_t>(total)) < 0) {
            err = std::string(std::strerror(errno));
            rc = -1;
        }
    } else {
        err = std::string(std::strerror(errno));
    }
    ::close(fd);
    if (rc < 0) {
        return false;
    }
    return true;
#elif defined(_WIN32)
    // 无常规权限可用的快速 fallocate（SetFileValidData 需要
    // SE_MANAGE_VOLUME_NAME 特权）：退化 resize（trunc 语义）
    std::error_code ec;
    std::filesystem::resize_file(path, total, ec);
    if (ec) {
        err = ec.message();
        return false;
    }
    return true;
#else
    // Linux/其他 POSIX：posix_fallocate（预留块且内容零）
    const int fd = ::open(path.c_str(), O_WRONLY);
    if (fd < 0) {
        err = "open failed: " + std::string(std::strerror(errno));
        return false;
    }
    const int rc = ::posix_fallocate(fd, 0,
                                     static_cast<off_t>(total));
    ::close(fd);
    if (rc != 0) {
        err = std::string(std::strerror(rc));
        return false;
    }
    return true;
#endif
}

}  // namespace

//==============================================================================
// HttpDownloadCommand 实现
//==============================================================================

HttpDownloadCommand::HttpDownloadCommand(
    TaskId task_id,
    int socket_fd,
    std::shared_ptr<HttpResponse> response,
    SegmentId segment_id,
    Bytes offset,
    Bytes length,
    std::string initial_data,
    Bytes resumed_bytes,
    bool truncate_output,
    bool chunked,
    std::string source_url
#ifdef FALCON_ENABLE_OPENSSL
    , HttpTlsSessionPtr tls_session
#endif
    )
    : AbstractCommand(task_id)
    , socket_fd_(socket_fd)
    , http_response_(std::move(response))
    , segment_id_(segment_id)
    , offset_(offset)
    , length_(length)
    , current_offset_(offset)
    , source_url_(std::move(source_url))
    , downloaded_bytes_(resumed_bytes)
    , initial_data_(std::move(initial_data))
    , truncate_output_(truncate_output)
    , chunked_encoding_(chunked)
#ifdef FALCON_ENABLE_OPENSSL
    , tls_session_(std::move(tls_session))
#endif
    , last_update_(std::chrono::steady_clock::now())
{
}

HttpDownloadCommand::~HttpDownloadCommand() {
    // 异常路径兜底：超时清理与停机排水直接销毁命令，不经 execute 的
    // 完成收尾分支；此处尽力把写缓冲落盘，防缓冲数据静默丢失
    //（失败无法上报——析构不抛异常，与 ofstream 析构同语义）
    finish_output();
}

const char* HttpDownloadCommand::name() const {
    return "HttpDownload";
}

bool HttpDownloadCommand::execute(DownloadEngineV2* engine) {
    if (!engine) {
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
    if (!group || group->status() == RequestGroupStatus::REMOVED) {
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::OK);
    }

    // 任务已被判定失败（如其他分段出错）：静默退出，不覆盖终态
    if (group->status() == RequestGroupStatus::FAILED) {
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::OK);
    }

    // 已暂停：冲刷残留缓冲并上报断点后静默退出（非失败语义；恢复
    // 时由重新发起的命令从断点继续）
    if (group->status() == RequestGroupStatus::PAUSED) {
        prepare_sweep(engine);
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::OK);
    }

    auto task = group->download_task();
    if (!task) {
        group->set_error_message("V2 HttpDownload 缺少 DownloadTask");
        group->set_status(RequestGroupStatus::FAILED);
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    if (!file_opened_) {
        // 临时文件语义（temp_extension 消费点）：数据实际写
        // <最终名><扩展名>，组完成时原子改名为最终名——下载中途与
        // 失败之后，半成品都不会顶着最终名出现
        const std::string& final_path = task->output_path();
        const std::string& ext = engine->config().temp_extension;
        write_path_ = ext.empty() ? final_path : final_path + ext;

        if (segment_id_ == 0 && truncate_output_) {
            // 全新下载的首段/单连接模式：创建（截断）文件
            output_.open(write_path_, std::ios::binary | std::ios::trunc);
        } else {
            // 不截断打开：多连接分段（文件由首段创建，本段按偏移定位
            // 写入）与断点续传的首段（临时文件已有断点数据）共用；
            // 续传文件缺失在 try_load_resume_state 校验阶段已被拒绝
            output_.open(write_path_, std::ios::binary | std::ios::in | std::ios::out);
        }
        if (!output_) {
            task->set_error("Failed to open output file: " + write_path_);
            const bool failed =
                fail_group_on_segment_error(engine, *group, task);
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            // 重试已接管：按非失败收口（命令完成，重试链继续）
            return handle_result(failed ? ExecutionResult::ERROR_OCCURRED
                                        : ExecutionResult::OK);
        }
        file_opened_ = true;

        // 文件预分配（aria2 --file-allocation 消费点）：仅全新下载的
        // 首段/单连接（文件刚按 trunc 创建）且总长已知时执行一次；
        // 续传（truncate_output_=false）与多段的非首段不触碰。分配
        // 失败按段错误收口——磁盘满绝不退化成"稀疏下载假装预分配"
        if (segment_id_ == 0 && truncate_output_) {
            const AllocMode alloc_mode =
                parse_file_allocation(group->options().file_allocation);
            if (alloc_mode != AllocMode::None) {
                // 总长优先取组 total（多段在 begin_multi_segment 时
                // set_total_size）；单连接组 total 保持 0（仅多段设置），
                // 回落 length_（Content-Length）。两者皆未知（chunked
                // 等）保持 0 → 不分配
                const uint64_t alloc_total = static_cast<uint64_t>(
                    group->file_info().total_size > 0
                        ? group->file_info().total_size
                        : length_);
                if (alloc_total > 0) {
                    std::string alloc_err;
                    if (!preallocate_file(write_path_, alloc_total,
                                          alloc_mode, alloc_err)) {
                        task->set_error("File pre-allocation failed (" +
                                        group->options().file_allocation +
                                        "): " + alloc_err);
                        const bool failed =
                            fail_group_on_segment_error(engine, *group,
                                                        task);
                        close_socket_fd(socket_fd_);
                        socket_fd_ = -1;
                        return handle_result(
                            failed ? ExecutionResult::ERROR_OCCURRED
                                   : ExecutionResult::OK);
                    }
                    FALCON_LOG_INFO_STREAM("输出文件已预分配 ("
                                           << group->options().file_allocation
                                           << "): " << write_path_ << " ("
                                           << alloc_total << " bytes)");
                }
            }
        }

        // 磁盘写缓冲容量取定（引擎级配置，任务选项无法承载）：
        // enable_disk_cache=false 或容量为 0 时保持直写
        if (engine->config().enable_disk_cache &&
            engine->config().disk_cache_size > 0) {
            write_buffer_.reserve(engine->config().disk_cache_size);
            write_buffer_capacity_ = engine->config().disk_cache_size;
        }

        if (segment_id_ == 0) {
            task->mark_started();
            task->set_status(TaskStatus::Downloading);
            if (length_ > 0 && !group->is_multi_segment()) {
                // 断点续传时 downloaded_bytes_ 已预置上一会话的断点
                task->update_progress(downloaded_bytes_, length_, 0);
            }
        }
    }

    if (!initial_written_ && !initial_data_.empty()) {
        // chunked 响应捎带的首批字节仍是分块协议数据（块大小行开头），
        // 必须过状态机解帧，直写会把协议杂质写进文件
        const bool initial_ok =
            chunked_encoding_
                ? handle_chunked_encoding(initial_data_.data(),
                                          initial_data_.size(), engine)
                : write_to_segment(initial_data_.data(), initial_data_.size(),
                                   engine);
        if (!initial_ok) {
            task->set_error("Failed to write initial body bytes");
            const bool failed =
                fail_group_on_segment_error(engine, *group, task);
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return handle_result(failed ? ExecutionResult::ERROR_OCCURRED
                                        : ExecutionResult::OK);
        }
        initial_data_.clear();
        initial_written_ = true;
        // 捎带体可能已收满本段（小文件的响应头与响应体常在同一 TCP
        // 段到达）：立即判定完成，否则落入 receive_data 等更多数据——
        // keep-alive 服务器下要挂到对端关闭连接才收口。chunked 场景
        // check_completion 返回 download_complete_（状态机见到终止块
        // 才置位），不会提前收口
        if (check_completion()) {
            download_complete_ = true;
        }
    } else {
        initial_written_ = true;
    }

    if (download_complete_) {
        if (!finish_output()) {
            task->set_error("Failed to flush buffered data to disk");
            const bool failed =
                fail_group_on_segment_error(engine, *group, task);
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return handle_result(failed ? ExecutionResult::ERROR_OCCURRED
                                        : ExecutionResult::OK);
        }
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        complete_group_if_all_segments_done(engine, *group, task, true);
        return handle_result(ExecutionResult::OK);
    }

    auto res = receive_data(engine);
    if (res == ExecutionResult::ERROR_OCCURRED) {
        finish_output();  // 已在失败路径：尽力落盘，错误不覆盖主因
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        if (task->error_message().empty()) {
            task->set_error("Socket recv failed");
        }
        const bool failed = fail_group_on_segment_error(engine, *group, task);
        return handle_result(failed ? ExecutionResult::ERROR_OCCURRED
                                    : ExecutionResult::OK);
    }

    if (download_complete_) {
        if (!finish_output()) {
            task->set_error("Failed to flush buffered data to disk");
            const bool failed =
                fail_group_on_segment_error(engine, *group, task);
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return handle_result(failed ? ExecutionResult::ERROR_OCCURRED
                                        : ExecutionResult::OK);
        }
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        complete_group_if_all_segments_done(engine, *group, task, true);
        return handle_result(ExecutionResult::OK);
    }

    if (res == ExecutionResult::WAIT_FOR_SOCKET) {
        return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
    }

    update_progress();
    return false;
}

void HttpDownloadCommand::complete_group_if_all_segments_done(
    DownloadEngineV2* engine, RequestGroup& group, const DownloadTask::Ptr& task,
    bool success) {
    if (!success) {
        return;  // 失败路径由 fail_group_on_segment_error 收尾，临时文件保留
    }

    // 最终落盘进度上报（finish_output 冲刷了残留缓冲，此刻 downloaded
    // 与磁盘一致；恢复段按段内绝对坐标）；未开启续传追踪时为无代价
    // 空操作
    group.report_segment_flushed(segment_id_, flushed_progress_absolute(group));

    // 单段模式：本段结束即任务完成（保持既有行为）；多分段模式：仅当
    // 全部分段结束且无失败时才置完成（失败已在
    // fail_group_on_segment_error 中立即置失败）
    const bool all_done = !group.is_multi_segment() ||
                          (group.finish_segment(success) &&
                           !group.has_segment_failure());
    if (!all_done) {
        return;
    }

    // 发布下载成果：临时文件原子改名为最终名。改名在 Completed 之前，
    // 监听者看到完成时成品必然已就位；改名失败按失败收尾，不会假报
    // COMPLETED
    if (!publish_output(task)) {
        fail_group_on_segment_error(engine, group, task);
        return;
    }
    // 成品已发布：续传使命完成，删除控制文件（半成品挂点随之消失）
    group.clear_resume_tracking();
    task->set_status(TaskStatus::Completed);
    group.set_status(RequestGroupStatus::COMPLETED);
}

bool HttpDownloadCommand::publish_output(const DownloadTask::Ptr& task) {
    if (write_path_.empty() || write_path_ == task->output_path()) {
        return true;  // 直写最终名（未启用临时扩展名）或已发布
    }
    // Windows 实机/CI 的杀毒实时扫描与索引服务会在写入结束后短暂持有
    // 临时文件句柄，紧随其后的发布改名报 sharing violation——共享重试
    // helper 吞掉这个瞬时窗口（CI 实测 run 38138445554 击穿了旧实现
    // 4×200ms 的固定预算），只有"被占用"类错误重试，句柄持续占用或
    // 永久性错误按最后一次错误收尾
    const auto rr =
        protocols::detail::rename_with_retry(write_path_, task->output_path());
    if (!rr.ok) {
        task->set_error("下载完成但发布失败（临时文件改名 " + write_path_ +
                        " → " + task->output_path() + "）: " + rr.error);
        return false;
    }
    write_path_.clear();  // 幂等防护：重复收尾不再尝试改名
    return true;
}

bool HttpDownloadCommand::fail_group_on_segment_error(DownloadEngineV2* engine,
                                                      RequestGroup& group,
                                                      const DownloadTask::Ptr& task) {
    // 失败收口固化断点：finish_output 已冲刷残留缓冲，此刻把最终落盘
    // 进度（段内绝对坐标）写进控制文件，重加任务/换源重试即可从断点
    // 继续
    group.report_segment_flushed(segment_id_, flushed_progress_absolute(group));

    // 组不可失败态（暂停/移除竞态）：不得把 Paused 改写成 Failed
    const auto st = group.status();
    if (st == RequestGroupStatus::PAUSED || st == RequestGroupStatus::REMOVED ||
        st == RequestGroupStatus::COMPLETED || st == RequestGroupStatus::FAILED) {
        return false;
    }

    // 段级换源重试（预算内换下一镜像）；预算耗尽才走既有失败收口
    if (HttpSegmentRetryCommand::schedule_retry(engine, get_task_id(),
                                                segment_id_, offset_,
                                                length_, source_url_)) {
        return false;
    }
    if (group.is_multi_segment()) {
        group.finish_segment(false);
    }
    FALCON_LOG_ERROR_STREAM("分段下载失败: task=" << get_task_id()
                          << ", segment=" << segment_id_
                          << ", reason=" << task->error_message());
    group.set_error_message(task->error_message());
    group.set_status(RequestGroupStatus::FAILED);
    task->set_status(TaskStatus::Failed);
    group.save_resume_now();
    return true;
}

AbstractCommand::ExecutionResult HttpDownloadCommand::receive_data(DownloadEngineV2* engine) {
    char buffer[65536];  // 64KB 缓冲区

    // 单任务限速值（任务 options.speed_limit，0 = 不限）：
    // 从请求组读取一次，recv 循环内复用
    std::uint64_t task_limit = 0;
    if (engine) {
        if (auto* group = engine->request_group_man()->find_group(get_task_id())) {
            task_limit = group->options().speed_limit;
        }
    }

    for (int iter = 0; iter < 64; ++iter) {
        // 限速（全局 + 单任务取严）：本轮接收预算耗尽即挂起——引擎
        // 节流窗口恢复前不再读数据（单次 execute 最多循环 64 轮，若
        // 不在读层限流，一次就能把整个文件拉完，循环级节流永远插不
        // 进来）。预算挂起不注册 socket 事件：此时数据通常已在内核
        // 缓冲，注册会被立即唤醒形成忙旋；直接回队，由引擎把 poll
        // 拉长到预算恢复点
        std::uint64_t chunk = sizeof(buffer);
        if (engine) {
            const std::uint64_t budget = engine->recv_budget(get_task_id(), task_limit);
            if (budget == 0) {
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            // 单次读取量截断到预算内：预算很小时不能整缓冲读，
            // 否则窗口瞬间超额、长期均速可到限值的 2 倍
            chunk = std::min<std::uint64_t>(sizeof(buffer), budget);
        }
        ssize_t n = 0;
#ifdef FALCON_ENABLE_OPENSSL
        if (tls_session_) {
            // TLS 连接必须经 SSL_read 解密（明文 recv 只能读到密文）
            n = SSL_read(tls_session_.get(), buffer, static_cast<int>(chunk));
            if (n <= 0) {
                const int ssl_error =
                    SSL_get_error(tls_session_.get(), static_cast<int>(n));
                if (ssl_error == SSL_ERROR_WANT_READ ||
                    ssl_error == SSL_ERROR_WANT_WRITE) {
                    // WANT_WRITE（写入响应/重协商被阻塞）同样可能出现在
                    // 读路径上，按所需方向注册事件，避免注册错方向挂死
                    if (engine) {
                        if (!engine->register_socket_event(
                                socket_fd_,
                                static_cast<int>(ssl_error == SSL_ERROR_WANT_WRITE
                                                     ? net::IOEvent::WRITE
                                                     : net::IOEvent::READ),
                                id())) {
                            return ExecutionResult::ERROR_OCCURRED;
                        }
                    }
                    return ExecutionResult::WAIT_FOR_SOCKET;
                }
                if (ssl_error == SSL_ERROR_ZERO_RETURN ||
                    ssl_error == SSL_ERROR_SYSCALL) {
                    // TLS 层干净关闭（close_notify）或底层连接断开——
                    // 均按 EOF 交给下方完成判定（chunked 必须见到终止
                    // 块、Content-Length 必须收满，截断不会假报完成）
                    n = 0;
                } else {
                    FALCON_LOG_ERROR_STREAM("SSL_read() 失败: " << ssl_error);
                    return ExecutionResult::ERROR_OCCURRED;
                }
            }
        } else
#endif
        {
#ifdef _WIN32
            n = recv(socket_fd_, buffer, static_cast<int>(chunk), 0);
#else
            n = recv(socket_fd_, buffer, chunk, 0);
#endif
        }
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                if (engine) {
                    if (!engine->register_socket_event(
                            socket_fd_, static_cast<int>(net::IOEvent::READ),
                            id())) {
                        return ExecutionResult::ERROR_OCCURRED;
                    }
                }
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR_STREAM("recv() 失败: " << sock_err_str(sock_errno()));
            return ExecutionResult::ERROR_OCCURRED;
        }

        if (n == 0) {
            // 对端关闭连接
            if (chunked_encoding_) {
                // 分块响应必须见到终止块才算完成（状态机已置位）；
                // 终止块未到先断连即截断——总长未知，EOF 本身不构成
                // 完成证据，按失败收尾防半截数据假报完成
                return download_complete_ ? ExecutionResult::OK
                                          : ExecutionResult::ERROR_OCCURRED;
            }
            if (length_ == 0 || downloaded_bytes_ >= length_) {
                download_complete_ = true;
                return ExecutionResult::OK;
            }
            return ExecutionResult::ERROR_OCCURRED;
        }

        // 限速统计：报告接收量给引擎（全局 + 单任务滑动窗口 → 节流）
        if (engine) {
            engine->report_downloaded_bytes(get_task_id(), static_cast<Bytes>(n));
        }

        if (chunked_encoding_) {
            if (!handle_chunked_encoding(buffer, static_cast<std::size_t>(n), engine)) {
                return ExecutionResult::ERROR_OCCURRED;
            }
        } else {
            if (!write_to_segment(buffer, static_cast<std::size_t>(n), engine)) {
                return ExecutionResult::ERROR_OCCURRED;
            }
        }

        if (check_completion()) {
            download_complete_ = true;
            return ExecutionResult::OK;
        }
    }

    // 读到上限后主动让出，下一轮继续（避免单命令长时间占用循环）
    return ExecutionResult::NEED_RETRY;
}

bool HttpDownloadCommand::write_to_segment(const char* data,
                                           std::size_t size,
                                           DownloadEngineV2* engine) {
    if (!file_opened_ || !output_) {
        return false;
    }

    // 越界保护：服务器发送的数据不能超过本段计划长度，
    // 否则会覆盖相邻分段的数据（多连接模式）
    std::size_t allowed = size;
    if (length_ > 0) {
        if (downloaded_bytes_ >= length_) {
            // 本段已写满：丢弃越界数据（视为正常完成路径的一部分）
            FALCON_LOG_WARN_STREAM("分段 " << segment_id_ << " 收到越界数据 "
                                   << size << " 字节，已丢弃");
            return true;
        }
        const Bytes capacity = length_ - downloaded_bytes_;
        if (static_cast<Bytes>(size) > capacity) {
            FALCON_LOG_WARN_STREAM("分段 " << segment_id_ << " 服务器越界发送，截断 "
                                   << (size - static_cast<std::size_t>(capacity)) << " 字节");
            allowed = static_cast<std::size_t>(capacity);
        }
    }
    if (allowed == 0) {
        return true;
    }

    if (write_buffer_capacity_ > 0) {
        // 磁盘写缓冲：数据攒在内存，攒满一次性落盘（顺序追加，落盘
        // 内容与位置和逐块直写完全一致）
        write_buffer_.insert(write_buffer_.end(), data, data + allowed);
    } else {
        // 直写路径——定位写入：每个 ofstream 独立维护写位置，无条件
        // seek 到本段当前写入点（offset_ + 已落盘前缀）。首段/单连接
        // 续传同样如此：断点前缀已在临时文件里，必须从断点处接写，
        // 绝不能从文件头覆盖
        output_.seekp(static_cast<std::streamoff>(offset_ + downloaded_bytes_));
        if (!output_) {
            return false;
        }

        output_.write(data, static_cast<std::streamsize>(allowed));
        if (!output_) {
            return false;
        }
    }

    downloaded_bytes_ += static_cast<Bytes>(allowed);
    bytes_since_last_update_ += static_cast<Bytes>(allowed);

    bool flushed_now = write_buffer_capacity_ == 0;

    if (engine) {
        auto* group_man = engine->request_group_man();
        auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (group) {
            group->add_downloaded_bytes(static_cast<Bytes>(allowed));
            if (auto task = group->download_task()) {
                if (group->is_multi_segment()) {
                    // 多连接模式：任务进度为全组聚合值
                    task->update_progress(group->downloaded_bytes(),
                                          group->file_info().total_size,
                                          download_speed_);
                } else {
                    task->update_progress(downloaded_bytes_, length_, download_speed_);
                }
            }
            if (flushed_now) {
                // 直写路径数据已确认落盘：同步上报续传进度（恢复段按
                // 段内绝对坐标；未开启追踪时为无代价空操作；缓冲路径
                // 在攒满冲刷后上报）
                group->report_segment_flushed(segment_id_,
                                              flushed_progress_absolute(*group));
            }
        }
    }

    FALCON_LOG_DEBUG_STREAM("写入 " << allowed << " 字节到分段 " << segment_id_);

    // 攒满落盘：冲刷失败按写失败处理（调用方段错误收尾）
    if (write_buffer_capacity_ > 0 &&
        write_buffer_.size() >= write_buffer_capacity_) {
        if (!flush_write_buffer()) {
            return false;
        }
        // 缓冲已清空，此刻 downloaded_bytes_ 与磁盘一致
        if (engine) {
            auto* group_man = engine->request_group_man();
            auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
            if (group) {
                group->report_segment_flushed(segment_id_,
                                              flushed_progress_absolute(*group));
            }
        }
    }
    return true;
}

bool HttpDownloadCommand::flush_write_buffer() {
    if (write_buffer_.empty()) {
        return true;
    }
    // 按段偏移定位：已落盘字节数 = 已接收字节数 − 缓冲滞留数。无条件
    // seek（首段续传同样适用——缓冲里是断点之后的数据，必须写到断点处）
    output_.seekp(static_cast<std::streamoff>(
        offset_ + downloaded_bytes_ - write_buffer_.size()));
    if (!output_) {
        return false;
    }
    output_.write(write_buffer_.data(),
                  static_cast<std::streamsize>(write_buffer_.size()));
    const bool ok = static_cast<bool>(output_);
    write_buffer_.clear();
    return ok;
}

bool HttpDownloadCommand::finish_output() {
    if (!file_opened_) {
        return true;
    }
    const bool flushed = flush_write_buffer();
    if (output_.is_open()) {
        output_.close();
    }
    return flushed && static_cast<bool>(output_);
}

void HttpDownloadCommand::prepare_sweep(DownloadEngineV2* engine) {
    // 暂停清扫检查点：冲刷残留缓冲（滞留数据不丢），随后把最终落盘
    // 进度（恢复段按段内绝对坐标）上报任务组并固化断点——析构路径只
    // 冲刷不上报（命令可能比引擎后销毁），清扫在引擎线程内执行可以
    // 安全触达任务组
    if (!finish_output() || !engine) {
        return;
    }
    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
    if (group) {
        group->report_segment_flushed(segment_id_, flushed_progress_absolute(*group));
        group->save_resume_now();
    }
}

bool HttpDownloadCommand::handle_chunked_encoding(const char* data, std::size_t size, DownloadEngineV2* engine) {
    // 将新数据追加到缓冲区
    chunk_buffer_.append(data, size);

    const char* ptr = chunk_buffer_.data();
    std::size_t remaining = chunk_buffer_.size();

    // 解析已缓存的块大小行文本（大小行终止符补齐后两个入口共用）
    auto parse_chunk_size = [this]() -> bool {
        try {
            chunk_remaining_ = std::stoul(chunk_size_str_, nullptr, 16);
        } catch (const std::exception&) {
            FALCON_LOG_ERROR_STREAM("无效的分块大小: " << chunk_size_str_);
            return false;
        }
        chunk_size_str_.clear();

        if (chunk_remaining_ == 0) {
            // 最后一个块，进入读取尾部状态
            chunk_state_ = ChunkParseState::READ_TRAILER;
            chunk_end_ = true;
        } else {
            chunk_state_ = ChunkParseState::READ_DATA;
        }
        return true;
    };

    while (remaining > 0) {
        switch (chunk_state_) {
            case ChunkParseState::READ_SIZE: {
                // 上轮块大小行的 CR 已单独到达（TCP 分片拆开 CRLF）：
                // 本轮首个字节必须是 LF，补齐后大小行即完整
                if (chunk_cr_pending_) {
                    if (ptr[0] != '\n') {
                        FALCON_LOG_ERROR_STREAM("分块大小行 CR 后未紧跟 LF: "
                                                << *ptr);
                        return false;
                    }
                    ptr++;
                    remaining--;
                    chunk_cr_pending_ = false;
                    if (!parse_chunk_size()) {
                        return false;
                    }
                    break;
                }

                // 查找 CRLF 表示块大小结束
                const char* crlf = static_cast<const char*>(
                    memchr(ptr, '\r', remaining));

                if (!crlf) {
                    // 尚未收到完整的块大小行
                    chunk_size_str_.append(ptr, remaining);
                    chunk_buffer_.clear();
                    return true;
                }

                // 检查是否有 LF
                const std::size_t crlf_offset = static_cast<std::size_t>(crlf - ptr);
                if (crlf_offset + 1 >= remaining) {
                    // CR 位于缓冲末尾、LF 尚未到达：仅缓存 CR 前的字节
                    // 并置位等待——预消费 CR 会让 LF 与块数据被并进大小
                    // 行，静默错帧
                    chunk_size_str_.append(ptr, crlf_offset);
                    chunk_cr_pending_ = true;
                    chunk_buffer_.clear();
                    return true;
                }
                if (crlf[1] != '\n') {
                    FALCON_LOG_ERROR_STREAM("分块大小行 CR 后未紧跟 LF: "
                                            << crlf[1]);
                    return false;
                }

                // 解析块大小
                chunk_size_str_.append(ptr, crlf_offset);
                if (!parse_chunk_size()) {
                    return false;
                }

                ptr += crlf_offset + 2;  // 跳过 CRLF
                remaining -= crlf_offset + 2;
                break;
            }

            case ChunkParseState::READ_DATA: {
                std::size_t to_write = std::min(remaining, chunk_remaining_);

                if (!write_to_segment(ptr, to_write, engine)) {
                    return false;
                }

                ptr += to_write;
                remaining -= to_write;
                chunk_remaining_ -= to_write;

                if (chunk_remaining_ == 0) {
                    chunk_state_ = ChunkParseState::READ_CR;
                }
                break;
            }

            case ChunkParseState::READ_CR: {
                if (*ptr == '\r') {
                    ptr++;
                    remaining--;
                    chunk_state_ = ChunkParseState::READ_LF;
                } else {
                    FALCON_LOG_ERROR_STREAM("分块编码: 期望 CR 但得到: " << *ptr);
                    return false;
                }
                break;
            }

            case ChunkParseState::READ_LF: {
                if (*ptr == '\n') {
                    ptr++;
                    remaining--;
                    chunk_state_ = ChunkParseState::READ_SIZE;
                } else {
                    FALCON_LOG_ERROR_STREAM("分块编码: 期望 LF 但得到: " << *ptr);
                    return false;
                }
                break;
            }

            case ChunkParseState::READ_TRAILER: {
                // 尾部区域的 CR 若在上一轮位于缓冲末尾被留下，本轮首个
                // 字节为 LF 即尾部结束。尾部只做终止检测不承载数据，CR
                // 后非 LF 时按噪音继续扫描（与大小行的严格判定不同：
                // 这里宽松不会损伤载荷完整性）
                if (chunk_cr_pending_) {
                    chunk_cr_pending_ = false;
                    if (ptr[0] == '\n') {
                        download_complete_ = true;
                        chunk_buffer_.clear();
                        return true;
                    }
                    // 非 LF：不消费该字节——它可能是补全 CRLF 的 CR，
                    // 交给下方扫描原样判定
                }

                // 读取可选的尾部头部，直到连续的 CRLF
                const char* double_crlf = static_cast<const char*>(
                    memmem(ptr, remaining, "\r\n\r\n", 4));

                if (!double_crlf) {
                    // 检查是否有单个 CRLF（表示尾部结束）
                    const char* crlf = static_cast<const char*>(
                        memchr(ptr, '\r', remaining));
                    if (crlf && crlf + 1 < ptr + remaining && crlf[1] == '\n') {
                        // 找到 CRLF，分块编码完成
                        download_complete_ = true;
                        chunk_buffer_.clear();
                        return true;
                    }
                    if (crlf && crlf + 1 == ptr + remaining) {
                        // CR 位于缓冲末尾：置位等待下批数据补判 LF——
                        // 丢弃它会让跨缓冲的 CRLF 永远不可见（终止序列
                        // 恰好被 TCP 分片时只能挂到 EOF 判截断）
                        chunk_cr_pending_ = true;
                    }
                    // 尚未收到完整的尾部
                    chunk_buffer_.clear();
                    return true;
                }

                // 分块编码完成
                download_complete_ = true;
                chunk_buffer_.clear();
                return true;
            }
        }
    }

    chunk_buffer_.clear();
    return true;
}

void HttpDownloadCommand::update_progress() {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_update_).count();

    if (elapsed >= 1000) {  // 每秒更新一次
        download_speed_ = static_cast<Speed>(
            bytes_since_last_update_ * 1000 / static_cast<std::size_t>(elapsed));
        bytes_since_last_update_ = 0;
        last_update_ = now;

        FALCON_LOG_DEBUG_STREAM("下载速度: " << (download_speed_ / 1024) << " KB/s");
    }
}

bool HttpDownloadCommand::check_completion() {
    if (length_ > 0) {
        return downloaded_bytes_ >= length_;
    }
    return download_complete_;
}

Bytes HttpDownloadCommand::flushed_progress_absolute(const RequestGroup& group) const {
    if (!group.has_resume_state()) {
        return downloaded_bytes_;
    }
    const ResumeControl plan = group.resume_plan();
    if (segment_id_ >= plan.segments.size()) {
        return downloaded_bytes_;
    }
    // 恢复段连接按剩余量请求（offset_ = 段计划起点 + 断点），下载计数
    // 是剩余量——换算成段计划坐标：绝对进度 = (连接起点 − 段计划起点)
    // + 本连接已收字节。全新段两起点重合，换算恒等
    const Bytes seg_start = plan.segments[segment_id_].offset;
    return (offset_ >= seg_start ? offset_ - seg_start : 0) + downloaded_bytes_;
}

} // namespace falcon
