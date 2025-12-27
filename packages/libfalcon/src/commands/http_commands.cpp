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

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>
#include <iostream>

namespace falcon {

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
    // 解析 URL
    // TODO: 实现完整的 URL 解析
    if (url_.find("https://") == 0) {
        use_https_ = true;
        port_ = 443;
        // 移除协议前缀
        url_ = url_.substr(8);
    } else if (url_.find("http://") == 0) {
        use_https_ = false;
        port_ = 80;
        url_ = url_.substr(7);
    }

    // 提取主机名和路径
    auto path_pos = url_.find('/');
    if (path_pos != std::string::npos) {
        host_ = url_.substr(0, path_pos);
        // 路径保存在 url_ 的剩余部分
    } else {
        host_ = url_;
        url_ = "/";
    }

    // 提取端口（如果指定）
    auto port_pos = host_.find(':');
    if (port_pos != std::string::npos) {
        port_ = static_cast<uint16_t>(std::stoi(host_.substr(port_pos + 1)));
        host_ = host_.substr(0, port_pos);
    }

    connection_state_ = HttpConnectionState::DISCONNECTED;
}

HttpInitiateConnectionCommand::~HttpInitiateConnectionCommand() {
    // Socket 的清理由连接池负责
}

bool HttpInitiateConnectionCommand::execute(DownloadEngineV2* engine) {
    try {
        // 获取 Socket Pool
        auto* socket_pool = engine ? engine->socket_pool() : nullptr;

        switch (connection_state_) {
            case HttpConnectionState::DISCONNECTED:
                // 尝试从 Socket Pool 获取连接
                if (socket_pool) {
                    net::SocketKey key;
                    key.host = host_;
                    key.port = port_;

                    auto pooled_socket = socket_pool->acquire(key);
                    if (pooled_socket && pooled_socket->is_valid()) {
                        // 复用现有连接
                        socket_fd_ = pooled_socket->fd();
                        pooled_socket_ = pooled_socket;
                        FALCON_LOG_DEBUG("复用 Socket 连接: " << host_ << ":" << port_);

                        // 直接准备 HTTP 请求
                        if (!prepare_http_request()) {
                            FALCON_LOG_ERROR("准备 HTTP 请求失败");
                            return handle_result(ExecutionResult::ERROR_OCCURRED);
                        }

                        connection_state_ = HttpConnectionState::CONNECTED;
                        return handle_result(ExecutionResult::OK);
                    }
                }

                // 没有可用连接，创建新连接
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

                connection_state_ = HttpConnectionState::CONNECTING;
                mark_active();
                return false;  // 等待连接完成

            case HttpConnectionState::CONNECTING:
                // 检查连接是否完成（使用 getsockopt SO_ERROR）
                {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    if (getsockopt(socket_fd_, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                    if (error != 0) {
                        FALCON_LOG_ERROR("连接失败: " << strerror(error));
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
                }

                // 步骤 4: HTTPS TLS 握手
                if (use_https_ && !setup_tls()) {
                    FALCON_LOG_ERROR("TLS 握手失败");
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                // 步骤 5: 准备 HTTP 请求
                if (!prepare_http_request()) {
                    FALCON_LOG_ERROR("准备 HTTP 请求失败");
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                connection_state_ = HttpConnectionState::CONNECTED;
                return handle_result(ExecutionResult::OK);

            default:
                return handle_result(ExecutionResult::ERROR_OCCURRED);
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("HTTP 连接异常: " << e.what());
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

    // 使用第一个结果
    char addr_str[INET6_ADDRSTRLEN] = {};
    if (result->ai_family == AF_INET) {
        auto* ipv4 = reinterpret_cast<struct sockaddr_in*>(result->ai_addr);
        inet_ntop(AF_INET, &ipv4->sin_addr, addr_str, sizeof(addr_str));
    } else if (result->ai_family == AF_INET6) {
        auto* ipv6 = reinterpret_cast<struct sockaddr_in6*>(result->ai_addr);
        inet_ntop(AF_INET6, &ipv6->sin6_addr, addr_str, sizeof(addr_str));
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
    int flags = fcntl(socket_fd_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
        return false;
    }

    FALCON_LOG_DEBUG("创建 Socket: fd=" << socket_fd_);
    return true;
}

bool HttpInitiateConnectionCommand::connect_socket() {
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);

    if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0) {
        FALCON_LOG_ERROR("inet_pton 失败: " << host_);
        return false;
    }

    int ret = connect(socket_fd_,
                     reinterpret_cast<struct sockaddr*>(&addr),
                     sizeof(addr));
    if (ret < 0 && errno != EINPROGRESS) {
        FALCON_LOG_ERROR("connect() 失败: " << strerror(errno));
        return false;
    }

    FALCON_LOG_INFO("正在连接 " << host_ << ":" << port_);
    return true;
}

bool HttpInitiateConnectionCommand::setup_tls() {
    // TODO: 实现 OpenSSL TLS 握手
    // 这是一个占位实现
    FALCON_LOG_WARN("TLS 支持待实现");
    return true;
}

bool HttpInitiateConnectionCommand::prepare_http_request() {
    // TODO: 创建 HttpRequest 对象并填充请求头
    // 这是一个占位实现
    FALCON_LOG_DEBUG("准备 HTTP 请求: " << url_);
    return true;
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

bool HttpResponseCommand::execute(DownloadEngineV2* /*engine*/) {
    // 接收响应头
    if (!headers_received_) {
        if (!receive_response_headers()) {
            // 需要更多数据
            mark_active();
            return false;
        }
    }

    // 解析响应头
    if (!parse_headers()) {
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }

    // 处理重定向
    if (is_redirect_) {
        handle_redirect();
        return handle_result(ExecutionResult::OK);
    }

    // 确定下载策略
    determine_download_strategy();

    return handle_result(ExecutionResult::OK);
}

bool HttpResponseCommand::receive_response_headers() {
    char buffer[4096];
    ssize_t n = recv(socket_fd_, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;  // 需要更多数据
        }
        FALCON_LOG_ERROR("recv() 失败: " << strerror(errno));
        return false;
    }

    buffer[n] = '\0';
    response_buffer_.append(buffer);

    // 检查是否接收完头部（\r\n\r\n 分隔符）
    if (response_buffer_.find("\r\n\r\n") != std::string::npos) {
        headers_received_ = true;
    }

    return headers_received_;
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

    std::string key = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    // 去除前后空格
    while (!value.empty() && value[0] == ' ') value.erase(0, 1);
    while (!value.empty() && value.back() == ' ') value.pop_back();

    headers_[key] = value;

    // 解析关键头部
    if (key == "Content-Length") {
        content_length_ = std::stoull(value);
    } else if (key == "Location") {
        redirect_url_ = value;
        is_redirect_ = (status_code_ >= 300 && status_code_ < 400);
    } else if (key == "Accept-Ranges") {
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

bool HttpResponseCommand::determine_download_strategy() {
    if (accepts_range_ && content_length_ > options_.min_segment_size) {
        FALCON_LOG_INFO("支持分段下载，启用多线程模式");
        // TODO: 创建多个 HttpDownloadCommand
    } else {
        FALCON_LOG_INFO("单线程下载模式");
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
        if (!parse_header_line(line)) return false;
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
    Bytes length)
    : AbstractCommand(task_id)
    , socket_fd_(socket_fd)
    , http_response_(std::move(response))
    , segment_id_(segment_id)
    , offset_(offset)
    , length_(length)
    , current_offset_(offset)
    , last_update_(std::chrono::steady_clock::now())
{
}

HttpDownloadCommand::~HttpDownloadCommand() = default;

bool HttpDownloadCommand::execute(DownloadEngineV2* /*engine*/) {
    if (download_complete_) {
        return handle_result(ExecutionResult::OK);
    }

    if (!receive_data()) {
        mark_active();
        return false;  // 需要更多数据
    }

    if (check_completion()) {
        download_complete_ = true;
        return handle_result(ExecutionResult::OK);
    }

    update_progress();
    return false;  // 继续下载
}

bool HttpDownloadCommand::receive_data() {
    char buffer[65536];  // 64KB 缓冲区
    ssize_t n = recv(socket_fd_, buffer, sizeof(buffer), 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;  // 暂无数据，稍后重试
        }
        FALCON_LOG_ERROR("recv() 失败: " << strerror(errno));
        return false;
    }

    if (n == 0) {
        // 连接关闭
        download_complete_ = true;
        return true;
    }

    downloaded_bytes_ += static_cast<Bytes>(n);
    bytes_since_last_update_ += static_cast<Bytes>(n);

    if (chunked_encoding_) {
        return handle_chunked_encoding();
    } else {
        return write_to_segment(buffer, static_cast<std::size_t>(n));
    }
}

bool HttpDownloadCommand::write_to_segment(const char* /*data*/, std::size_t size) {
    // TODO: 写入到分段下载器
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
