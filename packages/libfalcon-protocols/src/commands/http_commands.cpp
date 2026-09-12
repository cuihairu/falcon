/**
 * @file http_commands.cpp
 * @brief HTTP 协议命令实现
 * @author Falcon Team
 * @date 2025-12-24
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

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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

#include <errno.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>
#include <iostream>
#include <cctype>

namespace falcon {

//==============================================================================
// 多连接分段计划
//==============================================================================

std::vector<HttpSegmentRange> compute_http_segment_ranges(
    Bytes content_length,
    Bytes min_segment_size,
    std::size_t max_connections) {
    std::vector<HttpSegmentRange> ranges;
    if (content_length == 0 || max_connections == 0) {
        return ranges;
    }

    // 段数上限：不超过 max_connections，也不超过 content/min_segment
    //（保证每段平均长度不低于最小分段大小，参考 SegmentDownloader 的
    // min_segments/max_segments clamp 逻辑）
    Bytes max_by_min = content_length / std::max<Bytes>(min_segment_size, 1);
    if (max_by_min == 0) max_by_min = 1;
    std::size_t num_segments = static_cast<std::size_t>(
        std::min<Bytes>(static_cast<Bytes>(max_connections), max_by_min));
    if (num_segments < 1) num_segments = 1;

    // 等分，余数并入最后一段（与 SegmentDownloader 一致）
    const Bytes base = content_length / num_segments;
    const Bytes remainder = content_length % num_segments;

    ranges.reserve(num_segments);
    Bytes offset = 0;
    for (std::size_t i = 0; i < num_segments; ++i) {
        Bytes length = base;
        if (i == num_segments - 1) {
            length += remainder;
        }
        ranges.push_back({offset, length});
        offset += length;
    }
    return ranges;
}

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

void close_socket_fd(int fd) {
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
int sock_errno() { return WSAGetLastError(); }
std::string sock_err_str(int err) { return "WSA error " + std::to_string(err); }
bool sock_would_block(int err) { return err == WSAEWOULDBLOCK; }
#else
int sock_errno() { return errno; }
std::string sock_err_str(int err) { return strerror(err); }
bool sock_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
#endif

/// 构造连接级重试命令；返回 nullptr 表示不可重试：
/// - 多连接意图的任务不参与连接级重试（分段失败语义不同，暂不覆盖）
/// - 已达 max_retries（首连 + max_retries 次重试）
std::unique_ptr<Command> make_connection_retry(TaskId task_id,
                                               const std::string& url,
                                               const DownloadOptions& options,
                                               int retry_count) {
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
void fail_group_terminal(DownloadEngineV2* engine, TaskId task_id,
                         const std::string& reason) {
    if (!engine) return;
    auto* man = engine->request_group_man();
    auto* group = man ? man->find_group(task_id) : nullptr;
    if (!group) return;
    group->set_error_message(reason);
    group->set_status(RequestGroupStatus::FAILED);
    if (auto task = group->download_task()) {
        if (task->error_message().empty()) {
            task->set_error(reason);
        }
        task->set_status(TaskStatus::Failed);
    }
}

std::string to_lower(std::string s) {
    for (auto& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}
} // namespace

//==============================================================================
// HttpInitiateConnectionCommand 实现
//==============================================================================

HttpInitiateConnectionCommand::HttpInitiateConnectionCommand(
    TaskId task_id,
    std::string url,
    const DownloadOptions& options)
    : AbstractCommand(task_id)
    , url_(std::move(url))
    , options_(options)
    , socket_fd_(-1)
{
    // 解析 URL（最小实现：http://host[:port]/path?query）
    std::string rest = url_;
    if (rest.rfind("https://", 0) == 0) {
        use_https_ = true;
        port_ = 443;
        rest = rest.substr(8);
    } else if (rest.rfind("http://", 0) == 0) {
        use_https_ = false;
        port_ = 80;
        rest = rest.substr(7);
    } else {
        use_https_ = false;
        port_ = 80;
    }

    const auto path_pos = rest.find('/');
    std::string authority;
    if (path_pos == std::string::npos) {
        authority = rest;
        path_ = "/";
    } else {
        authority = rest.substr(0, path_pos);
        path_ = rest.substr(path_pos);
        if (path_.empty()) path_ = "/";
    }

    const auto port_pos = authority.rfind(':');
    if (port_pos != std::string::npos && port_pos + 1 < authority.size()) {
        host_ = authority.substr(0, port_pos);
        port_ = static_cast<uint16_t>(std::stoi(authority.substr(port_pos + 1)));
    } else {
        host_ = authority;
    }

    connection_state_ = HttpConnectionState::DISCONNECTED;
}

HttpInitiateConnectionCommand::~HttpInitiateConnectionCommand() {
#ifdef FALCON_ENABLE_OPENSSL
    // 清理 SSL 连接
    if (ssl_conn_) {
        SSL_shutdown(ssl_conn_);
        SSL_free(ssl_conn_);
        ssl_conn_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
#endif
    // Socket 的清理由连接池负责
}

bool HttpInitiateConnectionCommand::execute(DownloadEngineV2* engine) {
    try {
        if (!engine) {
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }

        switch (connection_state_) {
            case HttpConnectionState::DISCONNECTED:
                // 创建新连接
                // 步骤 1: 创建 Socket
                if (!create_socket()) {
                    FALCON_LOG_ERROR_STREAM("创建 Socket 失败");
                    notify_segment_failure(engine, "Failed to create socket");
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                // 步骤 2: 开始连接
                if (!connect_socket()) {
                    FALCON_LOG_ERROR_STREAM("连接失败: " << host_);
                    notify_segment_failure(engine, "Failed to connect: " + host_);
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                if (connect_in_progress_) {
                    connection_state_ = HttpConnectionState::CONNECTING;
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
                    return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
                }

                connection_state_ = HttpConnectionState::CONNECTED;

#ifdef FALCON_ENABLE_OPENSSL
                // 如果是 HTTPS，执行 TLS 握手
                if (use_https_) {
                    if (!setup_tls()) {
                        FALCON_LOG_ERROR_STREAM("TLS 握手失败: " << host_);
                        notify_segment_failure(engine, "TLS handshake failed: " + host_);
                        close_socket_fd(socket_fd_);
                        socket_fd_ = -1;
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                }
#else
                // 如果没有 OpenSSL，HTTPS 不可用
                if (use_https_) {
                    FALCON_LOG_ERROR_STREAM("HTTPS 支持需要启用 OpenSSL");
                    notify_segment_failure(engine, "HTTPS support requires OpenSSL");
                    close_socket_fd(socket_fd_);
                    socket_fd_ = -1;
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }
#endif

                {
                    auto res = send_http_request(engine);
                    if (res == ExecutionResult::ERROR_OCCURRED) {
                        notify_segment_failure(engine, "Failed to send HTTP request");
                    }
                    return handle_result(res);
                }

            case HttpConnectionState::CONNECTING:
                // 检查连接是否完成（使用 getsockopt SO_ERROR）
                {
                    int error = 0;
#ifdef _WIN32
                    int len = sizeof(error);
                    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len) < 0) {
#else
                    socklen_t len = sizeof(error);
                    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
#endif
                        notify_segment_failure(engine, "getsockopt(SO_ERROR) failed");
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                    if (error != 0) {
                        FALCON_LOG_ERROR_STREAM("连接失败: " << strerror(error));
                        notify_segment_failure(engine, std::string("Connect failed: ") + strerror(error));
                        close_socket_fd(socket_fd_);
                        socket_fd_ = -1;
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                }

                connection_state_ = HttpConnectionState::CONNECTED;

#ifdef FALCON_ENABLE_OPENSSL
                // 如果是 HTTPS，执行 TLS 握手
                if (use_https_) {
                    if (!setup_tls()) {
                        FALCON_LOG_ERROR_STREAM("TLS 握手失败: " << host_);
                        notify_segment_failure(engine, "TLS handshake failed: " + host_);
                        close_socket_fd(socket_fd_);
                        socket_fd_ = -1;
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                }
#else
                // 如果没有 OpenSSL，HTTPS 不可用
                if (use_https_) {
                    FALCON_LOG_ERROR_STREAM("HTTPS 支持需要启用 OpenSSL");
                    notify_segment_failure(engine, "HTTPS support requires OpenSSL");
                    close_socket_fd(socket_fd_);
                    socket_fd_ = -1;
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }
#endif

                {
                    auto res = send_http_request(engine);
                    if (res == ExecutionResult::ERROR_OCCURRED) {
                        notify_segment_failure(engine, "Failed to send HTTP request");
                    }
                    return handle_result(res);
                }

            default:
                return handle_result(ExecutionResult::ERROR_OCCURRED);
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR_STREAM("HTTP 连接异常: " << e.what());
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }
}

bool HttpInitiateConnectionCommand::resolve_host(
    const std::string& host,
    std::string& ip)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;     // IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    int ret = getaddrinfo(host.c_str(), nullptr, &hints, &result);
    if (ret != 0) {
        FALCON_LOG_ERROR_STREAM("getaddrinfo 失败: " << gai_strerror(ret));
        return false;
    }

    // 优先 IPv4；否则退回到首个 IPv6
    const struct addrinfo* chosen = result;
    for (auto* rp = result; rp != nullptr; rp = rp->ai_next) {
        if (rp->ai_family == AF_INET) {
            chosen = rp;
            break;
        }
    }

    char addr_str[INET6_ADDRSTRLEN] = {};
    if (chosen->ai_family == AF_INET) {
        auto* ipv4 = reinterpret_cast<struct sockaddr_in*>(chosen->ai_addr);
        inet_ntop(AF_INET, &ipv4->sin_addr, addr_str, sizeof(addr_str));
    } else if (chosen->ai_family == AF_INET6) {
        auto* ipv6 = reinterpret_cast<struct sockaddr_in6*>(chosen->ai_addr);
        inet_ntop(AF_INET6, &ipv6->sin6_addr, addr_str, sizeof(addr_str));
    } else {
        freeaddrinfo(result);
        return false;
    }

    ip = addr_str;
    freeaddrinfo(result);

    FALCON_LOG_INFO_STREAM(host << " 解析为 " << ip);
    return true;
}

bool HttpInitiateConnectionCommand::create_socket() {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        FALCON_LOG_ERROR_STREAM("socket() 失败: " << sock_err_str(sock_errno()));
        return false;
    }

    // 设置非阻塞模式
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(socket_fd_, FIONBIO, &mode) != 0) {
        closesocket(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
#else
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }
#endif

    FALCON_LOG_DEBUG_STREAM("创建 Socket: fd=" << socket_fd_);
    return true;
}

bool HttpInitiateConnectionCommand::connect_socket() {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    connect_in_progress_ = false;

    std::string ip = host_;
    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        if (!resolved_ip_.empty()) {
            ip = resolved_ip_;
        } else {
            if (!resolve_host(host_, ip)) {
                FALCON_LOG_ERROR_STREAM("解析主机失败: " << host_);
                return false;
            }
            resolved_ip_ = ip;
        }

        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            FALCON_LOG_ERROR_STREAM("inet_pton 失败: " << ip);
            return false;
        }
    }

    int ret = connect(socket_fd_,
                     reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr));
    if (ret < 0) {
        const int err = sock_errno();
        // 非阻塞 connect 的“进行中”语义：Winsock 一律报 WSAEWOULDBLOCK
        //（兼容 WSAEINPROGRESS），BSD 系报 EINPROGRESS（EINTR 视为可重试）
#ifdef _WIN32
        const bool in_progress = (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS);
#else
        const bool in_progress = (err == EINPROGRESS || err == EINTR);
#endif
        if (!in_progress) {
            FALCON_LOG_ERROR_STREAM("connect() 失败: " << sock_err_str(err));
            return false;
        }
    }

    connect_in_progress_ = (ret < 0);
    FALCON_LOG_INFO_STREAM("正在连接 " << host_ << ":" << port_
                                 << (connect_in_progress_ ? " (in progress)" : " (connected)"));
    return true;
}

#ifdef FALCON_ENABLE_OPENSSL
bool HttpInitiateConnectionCommand::setup_tls() {
    // 初始化 SSL 连接
    if (!ssl_ctx_) {
        // 创建 SSL_CTX
        const SSL_METHOD* method = TLS_client_method();
        if (!method) {
            FALCON_LOG_ERROR_STREAM("无法获取 TLS 方法: " << ERR_error_string(ERR_get_error(), nullptr));
            return false;
        }

        ssl_ctx_ = SSL_CTX_new(method);
        if (!ssl_ctx_) {
            FALCON_LOG_ERROR_STREAM("无法创建 SSL_CTX: " << ERR_error_string(ERR_get_error(), nullptr));
            return false;
        }

        // 设置最小 TLS 版本为 1.2
        SSL_CTX_set_min_proto_version(ssl_ctx_, TLS1_2_VERSION);

        // 配置 SSL 选项
        SSL_CTX_set_options(ssl_ctx_, SSL_OP_ALL | SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);

        // 设置验证模式
        if (options_.verify_ssl) {
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_PEER, nullptr);
        } else {
            SSL_CTX_set_verify(ssl_ctx_, SSL_VERIFY_NONE, nullptr);
        }

        // 加载默认证书
        if (!SSL_CTX_set_default_verify_paths(ssl_ctx_)) {
            FALCON_LOG_WARN_STREAM("无法加载默认 CA 证书，继续使用系统证书");
        }
    }

    // 创建 SSL 连接
    ssl_conn_ = SSL_new(ssl_ctx_);
    if (!ssl_conn_) {
        FALCON_LOG_ERROR_STREAM("无法创建 SSL 连接: " << ERR_error_string(ERR_get_error(), nullptr));
        return false;
    }

    // 绑定 Socket 到 SSL
    if (SSL_set_fd(ssl_conn_, static_cast<int>(socket_fd_)) != 1) {
        FALCON_LOG_ERROR_STREAM("无法绑定 Socket 到 SSL: " << ERR_error_string(ERR_get_error(), nullptr));
        return false;
    }

    // 设置 SNI 主机名
    SSL_set_tlsext_host_name(ssl_conn_, host_.c_str());

    // 执行 TLS 握手
    FALCON_LOG_INFO_STREAM("开始 TLS 握手: " << host_);

    int ret = SSL_connect(ssl_conn_);
    if (ret != 1) {
        int error = SSL_get_error(ssl_conn_, ret);

        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
            // 非阻塞模式下的情况，需要等待
            FALCON_LOG_DEBUG_STREAM("TLS 握手需要等待 I/O");
            // 在实际异步实现中，应该返回并等待 I/O 事件
            // 这里简化处理，稍后重试
            return false;
        }

        // 打印错误信息
        char error_buf[256];
        ERR_error_string_n(ERR_get_error(), error_buf, sizeof(error_buf));
        FALCON_LOG_ERROR_STREAM("TLS 握手失败: " << error_buf);

        return false;
    }

    // 验证证书
    if (options_.verify_ssl) {
        X509* cert = SSL_get_peer_certificate(ssl_conn_);
        if (cert) {
            // 验证主机名
            if (X509_check_host(cert, host_.c_str(), host_.length(), 0, nullptr) != 1) {
                FALCON_LOG_WARN_STREAM("证书主机名验证失败: " << host_);
            }
            X509_free(cert);
        } else {
            FALCON_LOG_WARN_STREAM("未收到服务器证书");
        }

        long verify_result = SSL_get_verify_result(ssl_conn_);
        if (verify_result != X509_V_OK) {
            FALCON_LOG_WARN_STREAM("证书验证失败: " << X509_verify_cert_error_string(verify_result));
        }
    }

    // 获取使用的密码套件
    const char* cipher = SSL_get_cipher(ssl_conn_);
    FALCON_LOG_INFO_STREAM("TLS 握手成功，使用加密套件: " << (cipher ? cipher : "unknown"));

    return true;
}
#endif

// 当未定义 FALCON_ENABLE_OPENSSL 时，提供存根实现
#ifndef FALCON_ENABLE_OPENSSL
bool HttpInitiateConnectionCommand::setup_tls() {
    FALCON_LOG_ERROR_STREAM("HTTPS 支持需要 OpenSSL");
    return false;
}
#endif

bool HttpInitiateConnectionCommand::prepare_http_request() {
    http_request_ = std::make_shared<HttpRequest>();
    http_request_->set_method("GET");
    http_request_->set_url(path_);

    http_request_->set_header("Host", host_);
    http_request_->set_header("User-Agent", options_.user_agent);
    http_request_->set_header("Accept", "*/*");
    http_request_->set_header("Connection", "close");

    // 多连接分段：非首段连接携带 Range 请求头
    if (has_range_) {
        const Bytes range_end = range_offset_ + range_length_ - 1;
        http_request_->set_header(
            "Range",
            "bytes=" + std::to_string(range_offset_) + "-" + std::to_string(range_end));
        FALCON_LOG_DEBUG_STREAM("分段 " << range_segment_id_ << " 请求范围: bytes="
                                << range_offset_ << "-" << range_end);
    }

    if (!options_.referer.empty()) {
        http_request_->set_header("Referer", options_.referer);
    }

    for (const auto& [k, v] : options_.headers) {
        http_request_->set_header(k, v);
    }

    request_data_ = http_request_->to_string();
    request_sent_ = 0;
    FALCON_LOG_DEBUG_STREAM("准备 HTTP 请求: " << host_ << ":" << port_ << " " << path_);
    return !request_data_.empty();
}

AbstractCommand::ExecutionResult HttpInitiateConnectionCommand::send_http_request(DownloadEngineV2* engine) {
    if (!engine) {
        return ExecutionResult::ERROR_OCCURRED;
    }

    if (request_data_.empty()) {
        if (!prepare_http_request()) {
            FALCON_LOG_ERROR_STREAM("准备 HTTP 请求失败");
            return ExecutionResult::ERROR_OCCURRED;
        }
    }

    while (request_sent_ < request_data_.size()) {
        const char* data = request_data_.data() + request_sent_;
        const std::size_t remaining = request_data_.size() - request_sent_;

        ssize_t n = 0;
#ifdef FALCON_ENABLE_OPENSSL
        if (use_https_ && ssl_conn_) {
            // 使用 SSL_write 发送 HTTPS 数据
            n = SSL_write(ssl_conn_, data, static_cast<int>(remaining));
            if (n <= 0) {
                int ssl_error = SSL_get_error(ssl_conn_, static_cast<int>(n));
                if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ) {
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
                    return ExecutionResult::WAIT_FOR_SOCKET;
                }
                FALCON_LOG_ERROR_STREAM("SSL_write() 失败: " << ssl_error);
                return ExecutionResult::ERROR_OCCURRED;
            }
        } else {
#endif
            // 使用普通 send 发送 HTTP 数据
            n = send(socket_fd_, data, static_cast<int>(remaining), 0);
            if (n < 0) {
                if (sock_would_block(sock_errno())) {
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
                    return ExecutionResult::WAIT_FOR_SOCKET;
                }
                FALCON_LOG_ERROR_STREAM("send() 失败: " << sock_err_str(sock_errno()));
                return ExecutionResult::ERROR_OCCURRED;
            }
#ifdef FALCON_ENABLE_OPENSSL
        }
#endif

        request_sent_ += static_cast<std::size_t>(n);
    }

    connection_state_ = HttpConnectionState::REQUEST_SENT;

    // 发送完成，进入响应阶段
    // 注意：如果是 HTTPS，需要传递 SSL 连接对象；
    // 分段连接需携带 URL 与分段信息，供响应命令按该段范围调度下载命令
    auto response_cmd = std::make_unique<HttpResponseCommand>(get_task_id(),
                                                             socket_fd_,
                                                             http_request_,
                                                             options_,
#ifdef FALCON_ENABLE_OPENSSL
                                                             ssl_conn_,
#endif
                                                             use_https_,
                                                             url_,
                                                             range_segment_id_,
                                                             range_offset_,
                                                             range_length_);
    response_cmd->set_retry_count(retry_count_);
    schedule_next(engine, std::move(response_cmd));
    return ExecutionResult::OK;
}

void HttpInitiateConnectionCommand::set_range(SegmentId segment_id,
                                              Bytes offset,
                                              Bytes length) {
    has_range_ = true;
    range_segment_id_ = segment_id;
    range_offset_ = offset;
    range_length_ = length;
}

void HttpInitiateConnectionCommand::notify_segment_failure(DownloadEngineV2* engine,
                                                           const std::string& reason) {
    if (!engine) {
        return;
    }
    if (has_range_) {
        // 分段连接：段失败收口（多连接组由段失败聚合终态）
        auto* man = engine->request_group_man();
        auto* group = man ? man->find_group(get_task_id()) : nullptr;
        if (!group) {
            return;
        }
        group->finish_segment(false);
        group->set_error_message(reason);
        group->set_status(RequestGroupStatus::FAILED);
        if (auto task = group->download_task()) {
            if (task->error_message().empty()) {
                task->set_error(reason);
            }
            task->set_status(TaskStatus::Failed);
        }
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return;
    }

    // 初始连接：先尝试连接级重试（重试期间任务组保持 ACTIVE），
    // 重试耗尽才收口终态——修复此前初始连接失败组无终态的悬空缺陷
    //（任务悬空 Downloading、all_completed 永不成立、run() 无法退出）
    if (auto retry_cmd = make_connection_retry(get_task_id(), url_, options_,
                                               retry_count_)) {
        schedule_next(engine, std::move(retry_cmd));
    } else {
        fail_group_terminal(engine, get_task_id(), reason);
    }
    close_socket_fd(socket_fd_);
    socket_fd_ = -1;
}

//==============================================================================
// HttpResponseCommand 实现
//==============================================================================

HttpResponseCommand::HttpResponseCommand(
    TaskId task_id,
    int socket_fd,
    std::shared_ptr<HttpRequest> request,
    const DownloadOptions& options
#ifdef FALCON_ENABLE_OPENSSL
    , void* ssl_conn
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
    , ssl_conn_(ssl_conn)
#endif
{
}

HttpResponseCommand::~HttpResponseCommand() = default;

bool HttpResponseCommand::execute(DownloadEngineV2* engine) {
    if (!engine) {
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    auto* group_man = engine->request_group_man();
    auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
    auto task = group ? group->download_task() : nullptr;

    auto fail = [&](const std::string& msg, bool retryable = false) {
        // 连接级失败（对端重置/立即断开等）：单连接任务调度延迟重试，
        // 任务组保持 ACTIVE 等待重试链。语义性失败（HTTP 状态错误、
        // 重定向不支持等）重试无价值，直接收口
        if (retryable) {
            if (auto retry_cmd = make_connection_retry(get_task_id(), source_url_,
                                                       options_, retry_count_)) {
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
        }
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    };

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
        return fail("HTTP redirect not supported in V2 socket pipeline");
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

    // 总量设置：仅初始连接执行（分段响应的 Content-Length 是段长而非文件长）
    if (task && content_length_ > 0 && segment_id_ == 0) {
        task->update_progress(task->downloaded_bytes(), content_length_, 0);
    }

    if (!determine_download_strategy(engine)) {
        return fail("Failed to schedule HTTP download command");
    }

    return handle_result(ExecutionResult::OK);
}

AbstractCommand::ExecutionResult HttpResponseCommand::receive_response_headers(DownloadEngineV2* engine) {
    char buffer[4096];
    for (;;) {
        ssize_t n = 0;
#ifdef FALCON_ENABLE_OPENSSL
        if (use_https_ && ssl_conn_) {
            // 使用 SSL_read 接收 HTTPS 数据
            n = SSL_read(static_cast<SSL*>(ssl_conn_), buffer, sizeof(buffer));
            if (n <= 0) {
                int ssl_error = SSL_get_error(static_cast<SSL*>(ssl_conn_), static_cast<int>(n));
                if (ssl_error == SSL_ERROR_WANT_READ || ssl_error == SSL_ERROR_WANT_WRITE) {
                    if (engine) {
                        engine->register_socket_event(
                            socket_fd_, static_cast<int>(net::IOEvent::READ), id());
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
                        engine->register_socket_event(
                            socket_fd_, static_cast<int>(net::IOEvent::READ), id());
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
    } else if (key == "location") {
        redirect_url_ = value;
        is_redirect_ = (status_code_ >= 300 && status_code_ < 400);
    } else if (key == "accept-ranges") {
        accepts_range_ = (value == "bytes");
        supports_resume_ = accepts_range_;
    }

    return true;
}

bool HttpResponseCommand::handle_redirect() {
    FALCON_LOG_INFO_STREAM("重定向到: " << redirect_url_);

    // 解析重定向 URL
    std::string redirect_url = redirect_url_;

    // 处理相对路径重定向
    if (redirect_url.find("http://") != 0 && redirect_url.find("https://") != 0) {
        // 相对路径，基于当前 URL 构建
        std::string base_url = http_request_->url();
        size_t path_pos = base_url.find('/', base_url.find("://") + 3);
        if (path_pos != std::string::npos) {
            redirect_url = base_url.substr(0, path_pos);
            if (redirect_url_[0] != '/') {
                redirect_url += '/';
            }
            redirect_url += redirect_url_;
        }
    }

    // 关闭当前连接
    close_socket_fd(socket_fd_);
    socket_fd_ = -1;

    // 创建新的下载选项（保持原有选项）
    DownloadOptions new_options = options_;

    // 创建新的连接命令跟随重定向
    auto new_command = std::make_unique<HttpInitiateConnectionCommand>(
        get_task_id(), redirect_url, new_options);

    // 计划执行新命令
    // 注意：这里需要通过某种方式将新命令添加到引擎的命令队列
    // 在当前的实现中，我们可以通过 schedule_next 来实现
    // 但需要确保引擎能够正确处理重定向链

    FALCON_LOG_INFO_STREAM("已创建重定向命令: " << redirect_url);
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

    FALCON_LOG_INFO_STREAM("启用多连接分段下载: " << plan.size() << " 个分段, 文件大小 "
                          << content_length_);
    group->begin_multi_segment(plan.size());
    group->set_total_size(content_length_);

    // 段 0：复用当前连接（服务器对该连接的 200/206 响应体按计划长度截取）
    schedule_next(engine,
                  std::make_unique<HttpDownloadCommand>(get_task_id(),
                                                        socket_fd_,
                                                        http_response_,
                                                        /*segment_id=*/0,
                                                        /*offset=*/0,
                                                        plan[0].length,
                                                        initial_body_));

    // 段 1..N-1：每段独立连接 + Range 请求
    for (std::size_t i = 1; i < plan.size(); ++i) {
        auto conn = std::make_unique<HttpInitiateConnectionCommand>(
            get_task_id(), source_url_, options_);
        conn->set_range(static_cast<SegmentId>(i), plan[i].offset, plan[i].length);
        schedule_next(engine, std::move(conn));
    }
    return true;
}

bool HttpResponseCommand::determine_download_strategy(DownloadEngineV2* engine) {
    if (!engine) return false;

    // 分段连接（段 1..N-1）的响应：按该段范围调度下载命令，
    // 不再产生新的分段/连接
    if (segment_id_ > 0) {
        schedule_next(engine,
                      std::make_unique<HttpDownloadCommand>(get_task_id(),
                                                            socket_fd_,
                                                            http_response_,
                                                            segment_id_,
                                                            range_offset_,
                                                            range_length_,
                                                            initial_body_));
        return true;
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
                                                        initial_body_));
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

    http_response_ = std::make_shared<HttpResponse>();
    http_response_->set_status_code(status_code_);
    for (const auto& [k, v] : headers_) {
        http_response_->add_header(k, v);
    }

    return true;
}

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
    std::string initial_data)
    : AbstractCommand(task_id)
    , socket_fd_(socket_fd)
    , http_response_(std::move(response))
    , segment_id_(segment_id)
    , offset_(offset)
    , length_(length)
    , current_offset_(offset)
    , initial_data_(std::move(initial_data))
    , last_update_(std::chrono::steady_clock::now())
{
}

HttpDownloadCommand::~HttpDownloadCommand() = default;

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

    auto task = group->download_task();
    if (!task) {
        group->set_error_message("V2 HttpDownload 缺少 DownloadTask");
        group->set_status(RequestGroupStatus::FAILED);
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    if (!file_opened_) {
        const auto& out_path = task->output_path();
        if (segment_id_ == 0) {
            // 首段/单连接模式：创建（截断）文件
            output_.open(out_path, std::ios::binary | std::ios::trunc);
        } else {
            // 多连接分段：文件由首段创建，本段按偏移定位写入
            //（各 ofstream 独立维护写位置，不会互相干扰）
            output_.open(out_path, std::ios::binary | std::ios::in | std::ios::out);
        }
        if (!output_) {
            task->set_error("Failed to open output file: " + out_path);
            task->set_status(TaskStatus::Failed);
            group->set_error_message(task->error_message());
            group->set_status(RequestGroupStatus::FAILED);
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }
        file_opened_ = true;

        if (segment_id_ == 0) {
            task->mark_started();
            task->set_status(TaskStatus::Downloading);
            if (length_ > 0 && !group->is_multi_segment()) {
                task->update_progress(0, length_, 0);
            }
        }
    }

    if (!initial_written_ && !initial_data_.empty()) {
        if (!write_to_segment(initial_data_.data(), initial_data_.size(), engine)) {
            task->set_error("Failed to write initial body bytes");
            fail_group_on_segment_error(*group, task);
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }
        initial_data_.clear();
        initial_written_ = true;
    } else {
        initial_written_ = true;
    }

    if (download_complete_) {
        output_.close();
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        complete_group_if_all_segments_done(*group, task, true);
        return handle_result(ExecutionResult::OK);
    }

    auto res = receive_data(engine);
    if (res == ExecutionResult::ERROR_OCCURRED) {
        output_.close();
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        if (task->error_message().empty()) {
            task->set_error("Socket recv failed");
        }
        fail_group_on_segment_error(*group, task);
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    if (download_complete_) {
        output_.close();
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        complete_group_if_all_segments_done(*group, task, true);
        return handle_result(ExecutionResult::OK);
    }

    if (res == ExecutionResult::WAIT_FOR_SOCKET) {
        return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
    }

    update_progress();
    return false;
}

void HttpDownloadCommand::complete_group_if_all_segments_done(
    RequestGroup& group, const DownloadTask::Ptr& task, bool success) {
    // 单段模式：本段结束即任务完成（保持既有行为）
    if (!group.is_multi_segment()) {
        if (success) {
            task->set_status(TaskStatus::Completed);
            group.set_status(RequestGroupStatus::COMPLETED);
        }
        return;
    }

    // 多分段模式：仅当全部分段结束且无失败时才置完成；
    // 失败已在 fail_group_on_segment_error 中立即置失败
    if (group.finish_segment(success) && !group.has_segment_failure()) {
        task->set_status(TaskStatus::Completed);
        group.set_status(RequestGroupStatus::COMPLETED);
    }
}

void HttpDownloadCommand::fail_group_on_segment_error(RequestGroup& group,
                                                      const DownloadTask::Ptr& task) {
    if (group.is_multi_segment()) {
        group.finish_segment(false);
    }
    group.set_error_message(task->error_message());
    group.set_status(RequestGroupStatus::FAILED);
    task->set_status(TaskStatus::Failed);
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
        ssize_t n = recv(socket_fd_, buffer, static_cast<int>(chunk), 0);
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                if (engine) {
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::READ), id());
                }
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR_STREAM("recv() 失败: " << sock_err_str(sock_errno()));
            return ExecutionResult::ERROR_OCCURRED;
        }

        if (n == 0) {
            // 对端关闭连接
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

    // 定位写入：非首段按段偏移寻址（每个 ofstream 独立维护写位置）
    if (segment_id_ != 0) {
        output_.seekp(static_cast<std::streamoff>(offset_ + downloaded_bytes_));
        if (!output_) {
            return false;
        }
    }

    output_.write(data, static_cast<std::streamsize>(allowed));
    if (!output_) {
        return false;
    }

    downloaded_bytes_ += static_cast<Bytes>(allowed);
    bytes_since_last_update_ += static_cast<Bytes>(allowed);

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
        }
    }

    FALCON_LOG_DEBUG_STREAM("写入 " << allowed << " 字节到分段 " << segment_id_);
    return true;
}

bool HttpDownloadCommand::handle_chunked_encoding(const char* data, std::size_t size, DownloadEngineV2* engine) {
    // 将新数据追加到缓冲区
    chunk_buffer_.append(data, size);

    const char* ptr = chunk_buffer_.data();
    std::size_t remaining = chunk_buffer_.size();

    while (remaining > 0) {
        switch (chunk_state_) {
            case ChunkParseState::READ_SIZE: {
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
                if (crlf_offset + 1 >= remaining || crlf[1] != '\n') {
                    chunk_size_str_.append(ptr, crlf_offset + 1);
                    ptr += crlf_offset + 1;
                    remaining -= crlf_offset + 1;
                    continue;
                }

                // 解析块大小
                chunk_size_str_.append(ptr, crlf_offset);
                try {
                    chunk_remaining_ = std::stoul(chunk_size_str_, nullptr, 16);
                } catch (const std::exception&) {
                    FALCON_LOG_ERROR_STREAM("无效的分块大小: " << chunk_size_str_);
                    return false;
                }

                ptr += crlf_offset + 2;  // 跳过 CRLF
                remaining -= crlf_offset + 2;
                chunk_size_str_.clear();

                if (chunk_remaining_ == 0) {
                    // 最后一个块，进入读取尾部状态
                    chunk_state_ = ChunkParseState::READ_TRAILER;
                    chunk_end_ = true;
                } else {
                    chunk_state_ = ChunkParseState::READ_DATA;
                }
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

//==============================================================================
// HttpRetryCommand 实现
//==============================================================================

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

bool HttpRetryCommand::execute(DownloadEngineV2* engine) {
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
        schedule_next(engine, std::move(next_cmd));
    }

    return handle_result(ExecutionResult::OK);
}

} // namespace falcon
