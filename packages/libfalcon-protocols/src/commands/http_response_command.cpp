/**
 * @file http_response_command.cpp
 * @brief HttpResponseCommand 实现（响应头解析/重定向/分段与续传调度）
 * @author Falcon Team
 * @date 2026-11-22
 */

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
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
using falcon::protocols::detail::make_connection_retry;
using falcon::protocols::detail::parse_www_authenticate;
using falcon::protocols::detail::build_basic_authorization;
using falcon::protocols::detail::generate_cnonce;
using falcon::protocols::detail::digest_request_uri;
#ifdef FALCON_ENABLE_OPENSSL
using falcon::protocols::detail::build_digest_authorization;
#endif

namespace {

// POSIX send 的 SIGPIPE 免疫（与 http_commands.cpp 同款：Windows 无
// 此语义传 0；macOS 走 socket 级 SO_NOSIGPIPE，MSG_NOSIGNAL 可用即用）
#ifdef _WIN32
constexpr int kSendFlags = 0;
#elif defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

std::string to_lower(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

/// 解析 Content-Range 头的起始偏移（"bytes <start>-<end>/<total>"，
/// 宽容处理 "bytes=" 与空白变体）。断点续传响应必须校验起点：长度
/// 一致而起点错位的数据同样会破坏其他分段
bool parse_content_range_start(const std::string& value, Bytes& start) {
    std::size_t i = 0;
    while (i < value.size() && !std::isdigit(static_cast<unsigned char>(value[i]))) {
        ++i;
    }
    if (i >= value.size()) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const unsigned long long v = std::stoull(value.substr(i), &consumed);
        start = static_cast<Bytes>(v);
        return consumed > 0;
    } catch (...) {
        return false;
    }
}
} // namespace

//==============================================================================
// HttpResponseCommand 实现
//==============================================================================

HttpResponseCommand::HttpResponseCommand(
    TaskId task_id,
    int socket_fd,
    std::shared_ptr<HttpRequest> request,
    const DownloadOptions& options
#ifdef FALCON_ENABLE_OPENSSL
    , HttpTlsSessionPtr tls_session
#endif
    , bool use_https,
    std::string source_url,
    SegmentId segment_id,
    Bytes range_offset,
    Bytes range_length)
    : AbstractCommand(task_id)
    , socket_fd_(socket_fd)
    , http_request_(std::move(request))
    , options_(options)
    , source_url_(std::move(source_url))
    , segment_id_(segment_id)
    , range_offset_(range_offset)
    , range_length_(range_length)
    , use_https_(use_https)
#ifdef FALCON_ENABLE_OPENSSL
    , tls_session_(std::move(tls_session))
#endif
{
}

HttpResponseCommand::~HttpResponseCommand() = default;

const char* HttpResponseCommand::name() const {
    return "HttpResponse";
}

bool HttpResponseCommand::execute(DownloadEngineV2* engine) {
    if (!engine) {
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
    auto task = group ? group->download_task() : nullptr;

    // 已暂停：静默退出（响应阶段无落盘状态，头内残带字节随连接丢弃；
    // 恢复时重新发起请求，断点从已落盘数据计）
    if (group && group->status() == RequestGroupStatus::PAUSED) {
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::OK);
    }

    auto fail = [&](const std::string& msg, bool retryable = false) {
        // 段上下文（分段响应或段 0 重试连接的响应，判定同
        // determine_download_strategy 的段分支）：先试段级换源重试，
        // 预算耗尽才走失败收口。段响应拒绝（200 代替 206 等）由此
        // 换源；暂停恢复的初始连接不在此列（走连接级重试保留恢复
        // 语义）——含段号 k > 0 的恢复初始连接（其响应若按段路由会
        // 跳过其余段重建），连接级重试重建处重打恢复标记
        const bool in_segment_context =
            (segment_id_ > 0 && !resume_initial_) || segment_retry_routing_;
        if (in_segment_context) {
            if (HttpSegmentRetryCommand::schedule_retry(
                    engine, get_task_id(), segment_id_,
                    range_offset_, range_length_, source_url_)) {
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                return handle_result(ExecutionResult::OK);
            }
        } else if (retryable) {
            // 连接级失败（对端重置/立即断开等）：单连接任务调度延迟
            // 重试，任务组保持 ACTIVE 等待重试链。语义性失败（HTTP
            // 状态错误、重定向不支持等）重试无价值，直接收口
            if (auto retry_cmd = make_connection_retry(engine, get_task_id(),
                                                       source_url_, options_,
                                                       retry_count_)) {
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                schedule_next(engine, std::move(retry_cmd));
                return handle_result(ExecutionResult::OK);
            }
        }
        if (task) {
            task->set_error(msg);
            task->set_status(TaskStatus::Failed);
        }
        if (group) {
            if (group->is_multi_segment()) {
                group->finish_segment(false);
            }
            group->set_error_message(msg);
            group->set_status(RequestGroupStatus::FAILED);
            // 失败收口固化断点：控制文件带最新进度，重加任务从断点继续
            group->save_resume_now();
        }
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    };

    // 401 挑战-应答循环：同连接重发认证请求并重新接收响应，直到非
    // 401 的最终响应。auth_retried_ 一次性门禁保证至多一轮重发——
    // 第二个 401 即凭据错误的确定性收口。WAIT_FOR_SOCKET 重入从
    // execute 顶部重来，进行中的重发状态由 auth_send_data_ 承载
    for (;;) {
        // 同连接重发进行中（begin_auth_resend 已构造完整请求）：
        // 优先续推发送
        if (!auth_send_data_.empty()) {
            auto res = continue_auth_send(engine);
            if (res == ExecutionResult::WAIT_FOR_SOCKET) {
                return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
            }
            if (res != ExecutionResult::OK) {
                return fail("Failed to send authenticated request",
                            /*retryable=*/true);
            }
            auth_send_data_.clear();
            auth_send_offset_ = 0;
        }

        if (!headers_received_) {
            auto res = receive_response_headers(engine);
            if (res == ExecutionResult::WAIT_FOR_SOCKET) {
                return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
            }
            if (res != ExecutionResult::OK) {
                return fail("Failed to receive HTTP response headers",
                            /*retryable=*/true);
            }
        }

        if (!parse_headers() || !http_response_) {
            return fail("Failed to parse HTTP response headers");
        }

        if (is_redirect_) {
            // 跟随重定向：解析 Location 重新发起连接（深度沿命令链传递，
            // 超链/无法解析/https 目标在此按失败收口）
            if (!handle_redirect(engine)) {
                return fail("Failed to follow redirect to \"" + redirect_url_ +
                            "\" (status " + std::to_string(status_code_) + ")");
            }
            return handle_result(ExecutionResult::OK);  // 本命令完成，新命令接管
        }

        if (status_code_ == 304) {
            // RFC 7232 §4.1：条件请求命中——本地文件仍是最新，按成功收口
            // 且原样保留（aria2 --conditional-get 同语义）。仅当本组确实
            // 携带过 If-Modified-Since（全新条件 GET）才走此路：304 没有
            // 响应体，无条件下误当成功会把"空响应"当成果，落进下载路径
            // 更会把本地文件截断成零字节——必须按失败收口
            const auto group_status = group ? group->status()
                                            : RequestGroupStatus::REMOVED;
            const bool conditional_armed =
                group && task && !group->if_modified_since().empty() &&
                group_status != RequestGroupStatus::PAUSED &&
                group_status != RequestGroupStatus::REMOVED &&
                group_status != RequestGroupStatus::COMPLETED &&
                group_status != RequestGroupStatus::FAILED;
            if (conditional_armed) {
                FALCON_LOG_INFO_STREAM("条件请求命中（304 Not Modified），本地文件"
                                       "已是最新: " << task->output_path());
                std::error_code size_ec;
                const auto local_size =
                    std::filesystem::file_size(task->output_path(), size_ec);
                const auto total =
                    size_ec ? static_cast<std::uintmax_t>(task->downloaded_bytes())
                            : local_size;
                // 终态进度不经节流（监听者必须看到 100%）；无网络传输，
                // 进度取自本地文件尺寸
                task->update_progress(total, total, 0);
                task->set_status(TaskStatus::Completed);
                group->set_status(RequestGroupStatus::COMPLETED);
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                return handle_result(ExecutionResult::OK);
            }
            return fail("Unexpected HTTP status: 304 (conditional-get not armed)");
        }

        if (status_code_ == 401) {
            // RFC 7235 挑战-应答：按 WWW-Authenticate 构造凭据（Basic/
            // Digest，aria2 同语义）同连接重发请求。一次性门禁：第二
            // 个 401 即凭据错误，确定性收口不再重试
            if (auth_retried_) {
                return fail("HTTP 401 (Unauthorized) after authentication — "
                            "check username/password");
            }
            auto res = begin_auth_resend(engine);
            if (res == ExecutionResult::WAIT_FOR_SOCKET) {
                return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
            }
            if (res != ExecutionResult::OK) {
                return fail(auth_error_.empty()
                                ? "HTTP 401 authentication failed"
                                : auth_error_);
            }
            continue;  // 同连接重发并重新接收响应
        }

        break;  // 非 401 的最终响应：进入常规响应处理
    }

    if (status_code_ < 200 || status_code_ >= 300) {
        return fail("Unexpected HTTP status: " + std::to_string(status_code_));
    }

    // 多连接分段的响应（非首段）必须为 206 且长度与计划一致
    if (segment_id_ > 0 && !validate_segment_response()) {
        return fail("Segment " + std::to_string(segment_id_) +
                    " response rejected (status " + std::to_string(status_code_) +
                    ", length " + std::to_string(content_length_) + ")");
    }

    // 总量设置：仅初始连接执行（分段响应的 Content-Length 是段长而非文件长）。
    // 续传初始响应的 Content-Length 是剩余量，进度由续传调度统一预置
    if (task && content_length_ > 0 && segment_id_ == 0 &&
        !(group && group->has_resume_state())) {
        task->update_progress(task->downloaded_bytes(), content_length_, 0);
    }

    if (!determine_download_strategy(engine)) {
        return fail("Failed to schedule HTTP download command");
    }

    return handle_result(ExecutionResult::OK);
}

//------------------------------------------------------------------------------
// HTTP 401 认证（同连接重发）
//------------------------------------------------------------------------------

AbstractCommand::ExecutionResult HttpResponseCommand::continue_auth_send(
    DownloadEngineV2* engine) {
    while (auth_send_offset_ < auth_send_data_.size()) {
        const char* data = auth_send_data_.data() + auth_send_offset_;
        const std::size_t remaining =
            auth_send_data_.size() - auth_send_offset_;
#ifdef _WIN32
        const ssize_t n = send(socket_fd_, data,
                               static_cast<int>(remaining), kSendFlags);
#else
        const ssize_t n = send(socket_fd_, data, remaining, kSendFlags);
#endif
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                if (engine && !engine->register_socket_event(
                                  socket_fd_,
                                  static_cast<int>(net::IOEvent::WRITE),
                                  id())) {
                    return ExecutionResult::ERROR_OCCURRED;
                }
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR_STREAM("认证请求重发 send() 失败: "
                                    << sock_err_str(sock_errno()));
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (n == 0) {
            // 非零长度 send 返回 0 属异常，按错误收口防忙旋
            FALCON_LOG_ERROR_STREAM("认证请求重发 send() 返回 0");
            return ExecutionResult::ERROR_OCCURRED;
        }
        auth_send_offset_ += static_cast<std::size_t>(n);
    }
    return ExecutionResult::OK;
}

AbstractCommand::ExecutionResult HttpResponseCommand::begin_auth_resend(
    DownloadEngineV2* engine) {
    if (!engine || socket_fd_ < 0) {
        return ExecutionResult::ERROR_OCCURRED;
    }
    // 一次性门禁置位先行：后续任何失败路径都以"已重试过"收口
    auth_retried_ = true;

    const auto auth_it = headers_.find("www-authenticate");
    std::string authorization;
    std::string scheme_desc;
    if (auth_it != headers_.end()) {
        const auto challenge = parse_www_authenticate(auth_it->second);
        if (challenge.scheme == "basic") {
            authorization = build_basic_authorization(options_.http_username,
                                                      options_.http_password);
            scheme_desc = "Basic";
        } else if (challenge.scheme == "digest") {
#ifdef FALCON_ENABLE_OPENSSL
            authorization = build_digest_authorization(
                challenge, options_.http_username, options_.http_password,
                http_request_->method(),
                digest_request_uri(http_request_->url()), generate_cnonce());
            scheme_desc = "Digest";
            if (authorization.empty()) {
                auth_error_ = "HTTP 401: unsupported Digest challenge "
                              "(algorithm or qop)";
            }
#else
            auth_error_ =
                "HTTP 401: Digest authentication requires an OpenSSL build";
#endif
        } else {
            // NTLM/Negotiate 等不受支持：明确失败（aria2 同语义，
            // 不静默降级 Basic——服务器点名方案即协商结果）
            auth_error_ =
                "HTTP 401: unsupported authentication scheme \"" +
                challenge.scheme + "\" (supported: Basic, Digest)";
        }
    } else {
        // 401 无 WWW-Authenticate：有凭据按 Basic 兜底（个别服务器
        // 漏发挑战头，curl 同姿态）；无凭据明确失败
        if (!options_.http_username.empty() || !options_.http_password.empty()) {
            authorization = build_basic_authorization(options_.http_username,
                                                      options_.http_password);
            scheme_desc = "Basic";
        } else {
            auth_error_ =
                "HTTP 401 (Unauthorized) — no credentials provided";
        }
    }
    if (!auth_error_.empty()) {
        return ExecutionResult::ERROR_OCCURRED;
    }

    http_request_->set_header("Authorization", authorization);
    FALCON_LOG_INFO_STREAM("HTTP 401 认证: " << scheme_desc << " (task "
                            << get_task_id() << ")，同连接重发请求");
    auth_send_data_ = http_request_->to_string();
    auth_send_offset_ = 0;

    // 复位全部解析状态：同一连接上重新接收并解析响应
    response_buffer_.clear();
    initial_body_.clear();
    headers_received_ = false;
    status_code_ = 0;
    headers_.clear();
    is_redirect_ = false;
    redirect_url_.clear();
    supports_resume_ = false;
    content_length_ = 0;
    accepts_range_ = false;
    is_chunked_response_ = false;
    content_encoding_.clear();
    http_response_.reset();

    return continue_auth_send(engine);
}

AbstractCommand::ExecutionResult HttpResponseCommand::receive_response_headers(DownloadEngineV2* engine) {
    char buffer[4096];
    for (;;) {
        ssize_t n = 0;
#ifdef FALCON_ENABLE_OPENSSL
        if (use_https_ && tls_session_) {
            // 使用 SSL_read 接收 HTTPS 数据
            n = SSL_read(tls_session_.get(), buffer, sizeof(buffer));
            if (n <= 0) {
                int ssl_error = SSL_get_error(tls_session_.get(), static_cast<int>(n));
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                    if (engine) {
                        if (!engine->register_socket_event(
                                socket_fd_, static_cast<int>(net::IOEvent::READ),
                                id())) {
                            return ExecutionResult::ERROR_OCCURRED;
                        }
                    }
                    return ExecutionResult::WAIT_FOR_SOCKET;
                }
                FALCON_LOG_ERROR_STREAM("SSL_read() 失败: " << ssl_error);
                return ExecutionResult::ERROR_OCCURRED;
            }
        } else {
#endif
            // 使用普通 recv 接收 HTTP 数据
            n = recv(socket_fd_, buffer, static_cast<int>(sizeof(buffer)), 0);
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
                FALCON_LOG_ERROR_STREAM("接收响应头时连接关闭");
                return ExecutionResult::ERROR_OCCURRED;
            }
#ifdef FALCON_ENABLE_OPENSSL
        }
#endif

        response_buffer_.append(buffer, static_cast<std::size_t>(n));

        // 检查是否接收完头部（\r\n\r\n 分隔符）
        auto pos = response_buffer_.find("\r\n\r\n");
        if (pos != std::string::npos) {
            headers_received_ = true;
            initial_body_ = response_buffer_.substr(pos + 4);
            response_buffer_.erase(pos + 4);
            return ExecutionResult::OK;
        }

        // 继续循环尝试读取更多，直到遇到 EAGAIN 或完整头部
        if (response_buffer_.size() > 1024 * 1024) {
            FALCON_LOG_ERROR_STREAM("响应头过大，终止解析");
            return ExecutionResult::ERROR_OCCURRED;
        }
    }
}

bool HttpResponseCommand::parse_status_line(const std::string& line) {
    // 格式: HTTP/1.1 200 OK
    size_t pos1 = line.find(' ');
    if (pos1 == std::string::npos) return false;

    size_t pos2 = line.find(' ', pos1 + 1);
    if (pos2 == std::string::npos) return false;

    std::string version = line.substr(0, pos1);
    std::string status_str = line.substr(pos1 + 1, pos2 - pos1 - 1);

    status_code_ = std::stoi(status_str);
    FALCON_LOG_INFO_STREAM("HTTP 状态: " << status_code_);

    return true;
}

bool HttpResponseCommand::parse_header_line(const std::string& line) {
    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) return false;

    std::string key = to_lower(line.substr(0, colon_pos));
    std::string value = line.substr(colon_pos + 1);

    // 去除前后空格
    while (!value.empty() && value[0] == ' ') value.erase(0, 1);
    while (!value.empty() && value.back() == ' ') value.pop_back();

    headers_[key] = value;

    // 解析关键头部
    if (key == "content-length") {
        content_length_ = std::stoull(value);
    } else if (key == "transfer-encoding") {
        // chunked 编码（值可能携带逗号分隔的编码链，含 chunked 即按
        // 分块解析；实际置零在 headers 解析完成后统一做——头序不定）
        is_chunked_response_ =
            value.find("chunked") != std::string::npos;
    } else if (key == "content-encoding") {
        // 强制压缩响应的观测记录（值可能是逗号分隔编码链，整串小写
        // 保留；原样落盘的日志判定在 headers 解析完成后统一做）
        content_encoding_ = to_lower(value);
    } else if (key == "location") {
        redirect_url_ = value;
        is_redirect_ = (status_code_ >= 300 && status_code_ < 400);
    } else if (key == "accept-ranges") {
        accepts_range_ = (value == "bytes");
        supports_resume_ = accepts_range_;
    }

    return true;
}

namespace {

/// 重定向跟随上限（含非重定向的原始请求不算，纯跳数）
constexpr int kMaxRedirects = 5;

/// RFC 7231 §6.4 规定应跟随的重定向状态码
bool is_redirect_status(int code) {
    return code == 301 || code == 302 || code == 303 ||
           code == 307 || code == 308;
}

/// 去掉 URL 的 query/fragment（相对路径解析基于纯 path）
std::string strip_query(std::string path) {
    const auto cut = path.find_first_of("?#");
    if (cut != std::string::npos) {
        path.resize(cut);
    }
    return path;
}

/// 按 RFC 3986 §5.2 归一化 path 中的 "." / ".." 段
std::string normalize_dots(std::string path) {
    std::vector<std::string> segs;
    std::size_t pos = 0;
    while (pos < path.size()) {
        auto next = path.find('/', pos);
        if (next == std::string::npos) {
            next = path.size();
        }
        const std::string seg = path.substr(pos, next - pos);
        pos = next + 1;
        if (seg == "..") {
            if (!segs.empty()) {
                segs.pop_back();
            }
        } else if (seg != "." && !seg.empty()) {
            segs.push_back(seg);
        }
    }
    std::string out;
    for (const auto& seg : segs) {
        out += '/';
        out += seg;
    }
    if (out.empty()) {
        out = "/";
    }
    return out;
}

/**
 * @brief 解析 Location 头为绝对 URL（RFC 3986 §5 引用解析）
 *
 * 四种形态：
 * - 绝对 URL（http:// / https://）→ 原样
 * - 协议相对（//host/path）→ 沿用当前 scheme
 * - 绝对路径（/path）→ scheme://authority + path
 * - 相对路径（rel、../up）→ 基于当前请求 path 的目录解析并归一化
 *
 * @return 空串表示无法解析（无效 Location）
 */
std::string resolve_redirect_location(const std::string& location,
                                      const std::string& request_url) {
    if (location.empty()) {
        return {};
    }
    if (location.rfind("http://", 0) == 0 ||
        location.rfind("https://", 0) == 0) {
        return location;
    }

    // 当前 URL：scheme://authority/path?query
    const auto scheme_end = request_url.find("://");
    if (scheme_end == std::string::npos) {
        return {};
    }
    const std::string scheme = request_url.substr(0, scheme_end);
    const auto authority_begin = scheme_end + 3;
    auto path_pos = request_url.find('/', authority_begin);
    const std::string authority =
        request_url.substr(authority_begin,
                           path_pos == std::string::npos
                               ? std::string::npos
                               : path_pos - authority_begin);
    if (authority.empty()) {
        return {};
    }

    // 协议相对：//host/path
    if (location.rfind("//", 0) == 0) {
        return scheme + ":" + location;
    }

    // 当前请求的目录（相对路径的基准）；无 path 的 URL 视为 "/"
    std::string base_path =
        path_pos == std::string::npos ? "/" : strip_query(request_url.substr(path_pos));
    const auto last_slash = base_path.rfind('/');
    const std::string base_dir =
        last_slash == std::string::npos ? "/" : base_path.substr(0, last_slash + 1);

    if (location[0] == '/') {
        // 绝对路径
        return scheme + "://" + authority + strip_query(location);
    }

    // 相对路径：基于当前目录拼接后归一化
    return scheme + "://" + authority +
           normalize_dots(base_dir + strip_query(location));
}

} // namespace

bool HttpResponseCommand::handle_redirect(DownloadEngineV2* engine) {
    if (!engine) {
        return false;
    }
    if (!is_redirect_status(status_code_)) {
        // 304 等其他 3xx：无跟随语义，按失败收口
        FALCON_LOG_WARN_STREAM("收到非跟随语义的 3xx 响应: " << status_code_);
        return false;
    }
    if (redirect_depth_ >= kMaxRedirects) {
        FALCON_LOG_WARN_STREAM("重定向链超过上限 " << kMaxRedirects
                              << "，停止跟随: task=" << get_task_id());
        return false;
    }

    const std::string redirect_url =
        resolve_redirect_location(redirect_url_, source_url_);
    if (redirect_url.empty()) {
        FALCON_LOG_WARN_STREAM("Location 无法解析: \"" << redirect_url_
                              << "\"，按失败收口: task=" << get_task_id());
        return false;
    }

    FALCON_LOG_INFO_STREAM("跟随重定向(" << redirect_depth_ + 1 << "/"
                          << kMaxRedirects << "): " << redirect_url);

    // 关闭当前连接——重定向目标通常是另一台服务器/另一条路径，
    // 本连接的响应已完成使命
    close_socket_fd(socket_fd_);
    socket_fd_ = -1;

    auto follow = std::make_unique<HttpInitiateConnectionCommand>(
        get_task_id(), redirect_url, options_);
    follow->set_redirect_depth(redirect_depth_ + 1);
    // 恢复初始连接的 3xx 跟随仍是恢复初始连接：标记原样传播（未标记
    // 时为无操作），响应路由不因重定向退化为段路由（k>0 形态会跳过
    // 其余段重建）
    follow->set_resume_initial(resume_initial_);
    // 段连接的 3xx 不再退化为初始响应：跟随连接原样携带段范围（判定
    // 与 fail 路径的 in_segment_context 一致——段 0 重试连接的跟随同
    // 样保留）；segment_retry_ 让跟随响应维持段上下文路由与换源资格
    if (segment_id_ > 0 || segment_retry_routing_) {
        follow->set_range(segment_id_, range_offset_, range_length_);
        follow->set_segment_retry(true);
    } else {
        // 条件下载：条件作用于最终资源——重定向后的全新 GET 仍携带
        // If-Modified-Since（段连接无条件头，组级值非空才有效）
        auto* group_man = engine->request_group_man();
        auto* group =
            group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (group && !group->if_modified_since().empty()) {
            follow->set_if_modified_since(group->if_modified_since());
        }
    }
    schedule_next(engine, std::move(follow));
    return true;
}

bool HttpResponseCommand::validate_segment_response() const {
    // 分段连接期望 206 Partial Content；若服务器忽略 Range 返回 200，
    // 该响应体是完整文件，写入段偏移会破坏其他分段的数据
    if (status_code_ != 206) {
        return false;
    }
    // 实际段长必须与计划一致，防止越界覆盖相邻分段
    return content_length_ == range_length_;
}

bool HttpResponseCommand::schedule_multi_segment_download(DownloadEngineV2* engine) {
    const auto plan = compute_http_segment_ranges(
        content_length_,
        static_cast<Bytes>(options_.min_segment_size),
        options_.max_connections);
    if (plan.size() <= 1) {
        return false;  // 无法拆分，回退单连接
    }

    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
    if (!group) {
        return false;
    }

    // 纵深防御（理论不可达）：组已在多段模式中再次收到初始响应，二次
    // begin_multi_segment 会重置段计数造成完成判定错乱——拒绝二次分段，
    // 回退单连接
    if (group->is_multi_segment()) {
        FALCON_LOG_WARN_STREAM("多段下载中收到二次分段调度，回退单连接: task="
                              << get_task_id());
        return false;
    }

    FALCON_LOG_INFO_STREAM("启用多连接分段下载: " << plan.size() << " 个分段, 文件大小 "
                          << content_length_);
    group->begin_multi_segment(plan.size());
    group->set_total_size(content_length_);

    // 建立续传追踪：分段计划 + 验证头写入控制文件（各段进度初始为 0），
    // 失败/中断后重加任务即可按此计划断点续传
    {
        std::vector<ResumeSegment> resume_segs;
        resume_segs.reserve(plan.size());
        for (const auto& r : plan) {
            resume_segs.push_back(ResumeSegment{r.offset, r.length, 0});
        }
        const auto hdr = [this](const char* key) -> std::string {
            const auto it = headers_.find(key);
            return it != headers_.end() ? it->second : std::string();
        };
        group->begin_resume_tracking(source_url_, content_length_,
                                     hdr("etag"), hdr("last-modified"),
                                     std::move(resume_segs));
    }

    // 段 0：复用当前连接（服务器对该连接的 200/206 响应体按计划长度截取）
    schedule_next(engine,
                  std::make_unique<HttpDownloadCommand>(get_task_id(),
                                                        socket_fd_,
                                                        http_response_,
                                                        /*segment_id=*/0,
                                                        /*offset=*/0,
                                                        plan[0].length,
                                                        initial_body_,
                                                        /*resumed_bytes=*/0,
                                                        /*truncate_output=*/true,
                                                        /*chunked=*/false,
                                                        source_url_
#ifdef FALCON_ENABLE_OPENSSL
                                                        ,
                                                        /*tls_session=*/tls_session_
#endif
                                                        ));

    // 段 1..N-1：每段独立连接 + Range 请求。多镜像时按段号轮转 URL
    //（Metalink 阶段2：各段落到不同镜像；单 URL 时 seg_url ==
    // source_url_，逐字节保持原行为）
    for (std::size_t i = 1; i < plan.size(); ++i) {
        const std::string seg_url = group->uris().size() > 1
                                        ? group->uris()[i % group->uris().size()]
                                        : source_url_;
        auto conn = std::make_unique<HttpInitiateConnectionCommand>(
            get_task_id(), seg_url, options_);
        conn->set_range(static_cast<SegmentId>(i), plan[i].offset, plan[i].length);
        schedule_next(engine, std::move(conn));
    }
    return true;
}

bool HttpResponseCommand::schedule_resume_download(DownloadEngineV2* engine,
                                                    RequestGroup& group) {
    const ResumeControl plan = group.resume_plan();
    if (plan.segments.empty()) {
        return false;  // 理论不可达（has_resume_state 为真必有计划）
    }
    const ResumeSegment& seg0 = plan.segments.front();

    // 防御：多段任务不参与连接级重试，多段模式下不应再收到初始响应；
    // 一旦出现（引擎状态被外部扰动）按放弃续传处理，绝不重复调度
    if (group.is_multi_segment()) {
        FALCON_LOG_WARN_STREAM("续传响应出现在多段下载中，放弃续传: task="
                              << get_task_id());
        group.abandon_resume();
        // 重置段跟踪：重启的全新下载若再次分段，二次 begin_multi_segment
        // 会撞上残留段计数（完成判定错乱的根因）
        group.reset_multi_segment_tracking();
        schedule_next(engine, std::make_unique<HttpInitiateConnectionCommand>(
                                  get_task_id(), source_url_, options_));
        return true;
    }

    // 响应一致性校验：带 Range 的续传请求必须得到 206 且长度与请求
    // 一致、Content-Range 起点与请求起点一致；未带 Range（断点为 0）
    // 的请求期望全量响应。服务器资源已变更（If-Range 失效）或不再
    // 支持 Range 时校验失败——断点数据是旧内容的，绝不能接续新内容
    const bool range_requested = range_length_ > 0;
    bool response_ok = false;
    if (range_requested) {
        response_ok = status_code_ == 206 && content_length_ == range_length_;
        if (response_ok) {
            Bytes content_start = 0;
            response_ok = parse_content_range_start(
                              headers_["content-range"], content_start) &&
                          content_start == range_offset_;
        }
    } else {
        // 无 Range 的初始请求只允许在段 0 无半截断点时放行——响应体
        // 起点是资源 0，半截断点（0 < downloaded < length）下按断点
        // 续写必然错位污染成品。「有半截断点却未带 Range」是真实可
        // 达的竞速态：resume 抢在暂停清扫命令执行之前时，fill 激活
        // 时断点还在旧命令的写缓冲里未固化（apply_group_resume_range
        // 看到 downloaded==0），随后清扫才固化断点。段 0 已完成
        //（downloaded == length，增量由其他段连接承载、响应体不消
        // 费）与段 0 全新（downloaded == 0）两种形态合法。total 相等
        // 不足以证明兼容，按不一致收口（abandon 全新重下，断点数据
        // 绝不接续不兼容的响应体）
        response_ok = content_length_ == plan.total &&
                      (seg0.downloaded == 0 ||
                       seg0.downloaded >= seg0.length);
    }

    if (!response_ok) {
        FALCON_LOG_INFO_STREAM("续传响应不一致（status=" << status_code_
                              << ", length=" << content_length_
                              << ", 请求起点=" << range_offset_
                              << ", 请求长度=" << range_length_
                              << ", content-range=" << headers_["content-range"]
                              << "），放弃续传: task=" << get_task_id());
        group.abandon_resume();
        // 本连接的响应体起点与全新请求不符，不能复用——重新发起无
        // Range 的全新下载。（同进程 pause→resume 后组保持 multi_segment
        // 状态的场景由函数入口的防御收口：is_multi_segment 为真在
        // 1908 处即 abandon+reset+return，此处条件与彼处恒同值——
        // 校验与 abandon 均不触碰段计数）
        schedule_next(engine, std::make_unique<HttpInitiateConnectionCommand>(
                                  get_task_id(), source_url_, options_));
        return true;
    }

    FALCON_LOG_INFO_STREAM("断点续传继续下载: task=" << get_task_id()
                          << ", 段 0 断点 " << seg0.downloaded << "/"
                          << seg0.length << ", 共 " << plan.segments.size()
                          << " 段");

    if (plan.segments.size() > 1) {
        // 组级状态重建：分段跟踪、已完成段预记账、聚合进度预置
        group.prepare_resumed_multi_segment();
    } else {
        // 单连接续传：断点直接计入组聚合（多段路径由 prepare 预置）
        group.add_downloaded_bytes(seg0.downloaded);
    }
    if (auto task = group.download_task()) {
        task->update_progress(group.downloaded_bytes(), plan.total, 0);
    }

    // 段 0 未完成：本连接继续收尾该段（服务器已从断点发送）。不截断
    // 打开——临时文件里是上一会话的断点数据，截断即销毁续传基础
    if (seg0.downloaded < seg0.length) {
        schedule_next(engine,
                      std::make_unique<HttpDownloadCommand>(get_task_id(),
                                                            socket_fd_,
                                                            http_response_,
                                                            /*segment_id=*/0,
                                                            /*offset=*/0,
                                                            seg0.length,
                                                            initial_body_,
                                                            /*resumed_bytes=*/seg0.downloaded,
                                                            /*truncate_output=*/false,
                                                            /*chunked=*/false,
                                                            source_url_
#ifdef FALCON_ENABLE_OPENSSL
                                                            ,
                                                            /*tls_session=*/tls_session_
#endif
                                                            ));
    } else {
        // 段 0 已完成（初始连接承载的是其他未完成段，由 segment 分支
        // 调度）：本连接不再承载下载，直接释放
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
    }

    // 其余未完成分段各自建立续传连接（已完成段不建连接）
    schedule_remaining_resume_segments(engine, group, /*except_segment=*/0);
    return true;
}

void HttpResponseCommand::schedule_remaining_resume_segments(
    DownloadEngineV2* engine, RequestGroup& group, std::size_t except_segment) {
    const ResumeControl plan = group.resume_plan();
    const std::string if_range = group.resume_if_range();
    for (std::size_t i = 0; i < plan.segments.size(); ++i) {
        if (i == except_segment) {
            continue;
        }
        const auto& seg = plan.segments[i];
        if (seg.downloaded >= seg.length) {
            continue;  // 已完成段：prepare_resumed_multi_segment 已预记账
        }
        // 恢复段与全新段同样轮转镜像；If-Range 是主镜像（uris_[0]，
        // ETag 的归属者）的验证值——仅当所选 URL 仍为主镜像时附带，
        // 非主镜像不带（其内容一致性由整文件哈希/206 校验兜底）
        const std::string seg_url = group.uris().size() > 1
                                        ? group.uris()[i % group.uris().size()]
                                        : source_url_;
        auto conn = std::make_unique<HttpInitiateConnectionCommand>(
            get_task_id(), seg_url, options_);
        conn->set_range(static_cast<SegmentId>(i),
                        seg.offset + seg.downloaded,
                        seg.length - seg.downloaded);
        if (!if_range.empty() && seg_url == group.uris().front()) {
            conn->set_if_range(if_range);
        }
        schedule_next(engine, std::move(conn));
    }
}

bool HttpResponseCommand::determine_download_strategy(DownloadEngineV2* engine) {
    if (!engine) return false;

    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;

    // 分段连接（段 1..N-1）与段 0 重试连接（显式标记）的响应：按该段
    // 范围调度下载命令，不再产生新的分段/连接。断点续传的首个分段
    // 响应（初始连接承载段 k > 0 的跨会话恢复）在此顺带重建组级分段
    // 状态并发起其余分段。段 0 重试与暂停恢复的初始连接都带 Range 且
    // 组处于多段态，从 Range 头无法区分——路由以显式标记为准，恢复
    // 流程不受影响
    if (segment_id_ > 0 ||
        (segment_retry_routing_ && group && group->is_multi_segment())) {
        // 同进程多段暂停恢复的初始连接（显式标记）：激活时段 0 已完成
        // 时续传 Range 的段号 k > 0，响应在此落入段路由分支且组处于
        // 多段态，下方重建门禁（!is_multi_segment）跳过其余段重建——
        // 被暂停清扫收走的段连接无人重建，组 ACTIVE 且零挂起命令，
        // 超时清理扫不到（CI macOS V2PauseThenResume 红面根因）。与
        // 段 0 形态（segment_id_==0 落下方 schedule_resume_download）
        // 同一收敛：转回恢复调度，由多段防御放弃续传全新重下。跨会话
        // 恢复组尚未进入多段态，不经此路（走下方既有重建分支，零变化）
        if (resume_initial_ && group && group->is_multi_segment() &&
            group->has_resume_state()) {
            return schedule_resume_download(engine, *group);
        }
        if (group && group->has_resume_state() && !group->is_multi_segment()) {
            group->prepare_resumed_multi_segment();
            schedule_remaining_resume_segments(engine, *group, segment_id_);
        }
        schedule_next(engine,
                      std::make_unique<HttpDownloadCommand>(get_task_id(),
                                                            socket_fd_,
                                                            http_response_,
                                                            segment_id_,
                                                            range_offset_,
                                                            range_length_,
                                                            initial_body_,
                                                            /*resumed_bytes=*/0,
                                                            /*truncate_output=*/
                                                            false,
                                                            /*chunked=*/false,
                                                            source_url_
#ifdef FALCON_ENABLE_OPENSSL
                                                            ,
                                                            /*tls_session=*/tls_session_
#endif
                                                            ));
        return true;
    }

    // 断点续传：初始连接响应按组内续传计划调度（跨会话恢复 + 同进程
    // 连接级重试共用此路径）。全新下载此时还没有续传状态，走原路径
    if (group && group->has_resume_state()) {
        return schedule_resume_download(engine, *group);
    }

    // 多连接分段：服务器支持 Range、文件足够大、允许多连接、
    // 且本命令持有原始 URL（可为其余分段建立新连接）
    if (accepts_range_ && content_length_ > options_.min_segment_size &&
        options_.max_connections > 1 && !source_url_.empty()) {
        if (schedule_multi_segment_download(engine)) {
            return true;
        }
        // 拆分失败或未启用时回退到单连接
        FALCON_LOG_INFO_STREAM("多连接分段不可用，回退单连接下载");
    } else if (accepts_range_ && content_length_ > options_.min_segment_size) {
        FALCON_LOG_INFO_STREAM("服务器支持分段下载（单连接模式）");
    } else {
        FALCON_LOG_INFO_STREAM("单线程下载模式");
    }

    schedule_next(engine,
                  std::make_unique<HttpDownloadCommand>(get_task_id(),
                                                        socket_fd_,
                                                        http_response_,
                                                        /*segment_id=*/0,
                                                        /*offset=*/0,
                                                        content_length_,
                                                        initial_body_,
                                                        /*resumed_bytes=*/0,
                                                        /*truncate_output=*/true,
                                                        /*chunked=*/
                                                        is_chunked_response_,
                                                        source_url_
#ifdef FALCON_ENABLE_OPENSSL
                                                        ,
                                                        /*tls_session=*/tls_session_
#endif
                                                        ));
    // 建立续传追踪（总长未知/chunked 下载无从续传，begin 内部自行忽略）
    if (group) {
        const auto hdr = [this](const char* key) -> std::string {
            const auto it = headers_.find(key);
            return it != headers_.end() ? it->second : std::string();
        };
        group->begin_resume_tracking(source_url_, content_length_,
                                     hdr("etag"), hdr("last-modified"),
                                     {{/*offset=*/0, content_length_, /*downloaded=*/0}});
    }
    return true;
}

bool HttpResponseCommand::parse_headers() {
    std::istringstream iss(response_buffer_);
    std::string line;

    // 解析状态行
    if (!std::getline(iss, line)) return false;
    if (!parse_status_line(line)) return false;

    // 解析头部行
    while (std::getline(iss, line)) {
        if (line == "\r") break;  // 空行表示头部结束
        // 去除 \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            continue;
        }
        if (!parse_header_line(line)) return false;
    }

    // chunked 响应：总长未知（RFC 7230 Transfer-Encoding 优先于
    // Content-Length）。统一在头循环后置零——头序不定，逐行置零
    // 会被后续 content-length 覆盖；总长未知同时让分段/续传门禁
    // 自然失活（无 Range 请求、不建续传追踪）
    if (is_chunked_response_) {
        content_length_ = 0;
        accepts_range_ = false;
        supports_resume_ = false;
    }

    // 强制压缩的观测点：请求恒带 Accept-Encoding: identity，服务器仍
    // 回 content-encoding 即无视协商（透明代理/动态压缩网关常见）。
    // 按 aria2 同语义原样落盘——输出文件与 URL 响应体逐字节一致是
    // 下载器不变式，这里只记录不改动；静默处理会让用户拿到压缩数据
    // 却毫无线索
    if (status_code_ >= 200 && status_code_ < 300 &&
        !content_encoding_.empty() && content_encoding_ != "identity") {
        FALCON_LOG_INFO_STREAM("服务器无视压缩协商（content-encoding: "
                               << content_encoding_
                               << "），响应体按字节原样落盘");
    }

    http_response_ = std::make_shared<HttpResponse>();
    http_response_->set_status_code(status_code_);
    for (const auto& [k, v] : headers_) {
        http_response_->add_header(k, v);
    }

    return true;
}

} // namespace falcon
