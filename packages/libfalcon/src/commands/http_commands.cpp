/**
 * @file http_commands.cpp
 * @brief HTTP 协议命令实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/commands/http_commands.hpp>
#include <falcon/download_engine_v2.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// Windows 没有 ssize_t，使用 SSIZE_T 或 ptrdiff_t
typedef SSIZE_T ssize_t;
// Windows 没有 EINPROGRESS，使用 WSAEWOULDBLOCK
#ifndef EINPROGRESS
#define EINPROGRESS WSAEWOULDBLOCK
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <errno.h>

#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>
#include <iostream>
#include <cctype>

namespace falcon {

namespace {
void close_socket_fd(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(fd);
#else
    ::close(fd);
#endif
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
    // Socket 的清理由连接池负责
}

bool HttpInitiateConnectionCommand::execute(DownloadEngineV2* engine) {
    try {
        if (!engine) {
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }

        if (use_https_) {
            FALCON_LOG_ERROR("当前 HTTP 命令链路暂不支持 HTTPS: " << url_);
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }

        switch (connection_state_) {
            case HttpConnectionState::DISCONNECTED:
                // 创建新连接
                // 步骤 1: 创建 Socket
                if (!create_socket()) {
                    FALCON_LOG_ERROR("创建 Socket 失败");
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                // 步骤 2: 开始连接
                if (!connect_socket()) {
                    FALCON_LOG_ERROR("连接失败: " << host_);
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                if (connect_in_progress_) {
                    connection_state_ = HttpConnectionState::CONNECTING;
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
                    return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
                }

                connection_state_ = HttpConnectionState::CONNECTED;
                return handle_result(send_http_request(engine));

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
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                    if (error != 0) {
                        FALCON_LOG_ERROR("连接失败: " << strerror(error));
                        close_socket_fd(socket_fd_);
                        socket_fd_ = -1;
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                }

                connection_state_ = HttpConnectionState::CONNECTED;
                return handle_result(send_http_request(engine));

            default:
                return handle_result(ExecutionResult::ERROR_OCCURRED);
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("HTTP 连接异常: " << e.what());
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
        FALCON_LOG_ERROR("getaddrinfo 失败: " << gai_strerror(ret));
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

    FALCON_LOG_INFO(host << " 解析为 " << ip);
    return true;
}

bool HttpInitiateConnectionCommand::create_socket() {
    socket_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd_ < 0) {
        FALCON_LOG_ERROR("socket() 失败: " << strerror(errno));
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

    FALCON_LOG_DEBUG("创建 Socket: fd=" << socket_fd_);
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
                FALCON_LOG_ERROR("解析主机失败: " << host_);
                return false;
            }
            resolved_ip_ = ip;
        }

        if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
            FALCON_LOG_ERROR("inet_pton 失败: " << ip);
            return false;
        }
    }

    int ret = connect(socket_fd_,
                     reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        FALCON_LOG_ERROR("connect() 失败: " << strerror(errno));
        return false;
    }

    connect_in_progress_ = (ret < 0 && errno == EINPROGRESS);
    FALCON_LOG_INFO("正在连接 " << host_ << ":" << port_
                                 << (connect_in_progress_ ? " (in progress)" : " (connected)"));
    return true;
}

bool HttpInitiateConnectionCommand::setup_tls() {
    // TODO: 实现 OpenSSL TLS 握手
    // 这是一个占位实现
    FALCON_LOG_WARN("TLS 支持待实现");
    return true;
}

bool HttpInitiateConnectionCommand::prepare_http_request() {
    http_request_ = std::make_shared<HttpRequest>();
    http_request_->set_method("GET");
    http_request_->set_url(path_);

    http_request_->set_header("Host", host_);
    http_request_->set_header("User-Agent", options_.user_agent);
    http_request_->set_header("Accept", "*/*");
    http_request_->set_header("Connection", "close");

    if (!options_.referer.empty()) {
        http_request_->set_header("Referer", options_.referer);
    }

    for (const auto& [k, v] : options_.headers) {
        http_request_->set_header(k, v);
    }

    request_data_ = http_request_->to_string();
    request_sent_ = 0;
    FALCON_LOG_DEBUG("准备 HTTP 请求: " << host_ << ":" << port_ << " " << path_);
    return !request_data_.empty();
}

AbstractCommand::ExecutionResult HttpInitiateConnectionCommand::send_http_request(DownloadEngineV2* engine) {
    if (!engine) {
        return ExecutionResult::ERROR_OCCURRED;
    }

    if (request_data_.empty()) {
        if (!prepare_http_request()) {
            FALCON_LOG_ERROR("准备 HTTP 请求失败");
            return ExecutionResult::ERROR_OCCURRED;
        }
    }

    while (request_sent_ < request_data_.size()) {
        const char* data = request_data_.data() + request_sent_;
        const std::size_t remaining = request_data_.size() - request_sent_;

        ssize_t n = send(socket_fd_, data, remaining, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                engine->register_socket_event(
                    socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR("send() 失败: " << strerror(errno));
            return ExecutionResult::ERROR_OCCURRED;
        }

        request_sent_ += static_cast<std::size_t>(n);
    }

    connection_state_ = HttpConnectionState::REQUEST_SENT;

    // 发送完成，进入响应阶段
    schedule_next(engine,
                  std::make_unique<HttpResponseCommand>(get_task_id(),
                                                       socket_fd_,
                                                       http_request_,
                                                       options_));
    return ExecutionResult::OK;
}

//==============================================================================
// HttpResponseCommand 实现
//==============================================================================

HttpResponseCommand::HttpResponseCommand(
    TaskId task_id,
    int socket_fd,
    std::shared_ptr<HttpRequest> request,
    const DownloadOptions& options)
    : AbstractCommand(task_id)
    , socket_fd_(socket_fd)
    , http_request_(std::move(request))
    , options_(options)
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

    auto fail = [&](const std::string& msg) {
        if (task) {
            task->set_error(msg);
            task->set_status(TaskStatus::Failed);
        }
        if (group) {
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
            return fail("Failed to receive HTTP response headers");
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

    if (task && content_length_ > 0) {
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
        ssize_t n = recv(socket_fd_, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (engine) {
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::READ), id());
                }
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR("recv() 失败: " << strerror(errno));
            return ExecutionResult::ERROR_OCCURRED;
        }

        if (n == 0) {
            FALCON_LOG_ERROR("接收响应头时连接关闭");
            return ExecutionResult::ERROR_OCCURRED;
        }

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
            FALCON_LOG_ERROR("响应头过大，终止解析");
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
    FALCON_LOG_INFO("HTTP 状态: " << status_code_);

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
    FALCON_LOG_INFO("重定向到: " << redirect_url_);
    // TODO: 创建新的 HttpInitiateConnectionCommand 跟随重定向
    return true;
}

bool HttpResponseCommand::determine_download_strategy(DownloadEngineV2* engine) {
    if (!engine) return false;

    // 单连接下载：后续可按 Accept-Ranges + min_segment_size 切换多连接
    if (accepts_range_ && content_length_ > options_.min_segment_size) {
        FALCON_LOG_INFO("支持分段下载，启用多线程模式");
        // TODO: 创建多个 HttpDownloadCommand
    } else {
        FALCON_LOG_INFO("单线程下载模式");
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
        output_.open(out_path, std::ios::binary | std::ios::trunc);
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

        task->mark_started();
        task->set_status(TaskStatus::Downloading);
        if (length_ > 0) {
            task->update_progress(0, length_, 0);
        }
    }

    if (!initial_written_ && !initial_data_.empty()) {
        if (!write_to_segment(initial_data_.data(), initial_data_.size(), engine)) {
            task->set_error("Failed to write initial body bytes");
            task->set_status(TaskStatus::Failed);
            group->set_error_message(task->error_message());
            group->set_status(RequestGroupStatus::FAILED);
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
        task->set_status(TaskStatus::Completed);
        group->set_status(RequestGroupStatus::COMPLETED);
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
        task->set_status(TaskStatus::Failed);
        group->set_error_message(task->error_message());
        group->set_status(RequestGroupStatus::FAILED);
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    if (download_complete_) {
        output_.close();
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        task->set_status(TaskStatus::Completed);
        group->set_status(RequestGroupStatus::COMPLETED);
        return handle_result(ExecutionResult::OK);
    }

    if (res == ExecutionResult::WAIT_FOR_SOCKET) {
        return handle_result(ExecutionResult::WAIT_FOR_SOCKET);
    }

    update_progress();
    return false;
}

AbstractCommand::ExecutionResult HttpDownloadCommand::receive_data(DownloadEngineV2* engine) {
    char buffer[65536];  // 64KB 缓冲区
    for (int iter = 0; iter < 64; ++iter) {
        ssize_t n = recv(socket_fd_, buffer, sizeof(buffer), 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (engine) {
                    engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::READ), id());
                }
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR("recv() 失败: " << strerror(errno));
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

        if (chunked_encoding_) {
            if (!handle_chunked_encoding()) {
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

    output_.write(data, static_cast<std::streamsize>(size));
    if (!output_) {
        return false;
    }

    downloaded_bytes_ += static_cast<Bytes>(size);
    bytes_since_last_update_ += static_cast<Bytes>(size);

    if (engine) {
        auto* group_man = engine->request_group_man();
        auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
        if (group) {
            group->add_downloaded_bytes(static_cast<Bytes>(size));
            if (auto task = group->download_task()) {
                task->update_progress(downloaded_bytes_, length_, download_speed_);
            }
        }
    }

    FALCON_LOG_DEBUG("写入 " << size << " 字节到分段 " << segment_id_);
    return true;
}

bool HttpDownloadCommand::handle_chunked_encoding() {
    // TODO: 实现分块传输编码解析
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

        FALCON_LOG_DEBUG("下载速度: " << (download_speed_ / 1024) << " KB/s");
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
    , retry_wait_(std::chrono::seconds(5))
{
}

bool HttpRetryCommand::execute(DownloadEngineV2* /*engine*/) {
    if (!should_retry()) {
        FALCON_LOG_ERROR("达到最大重试次数: " << max_retries_);
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    FALCON_LOG_INFO("重试下载 (" << retry_count_ << "/" << max_retries_ << "): " << url_);

    // 等待一段时间后重试
    std::this_thread::sleep_for(retry_wait_);

    // TODO: 创建新的连接命令
    // auto next_cmd = CommandFactory::create_http_init_command(
    //     get_task_id(), url_, options_);
    // schedule_next(engine, std::move(next_cmd));

    return handle_result(ExecutionResult::OK);
}

} // namespace falcon
