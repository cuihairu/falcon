/**
 * @file http_commands.hpp
 * @brief HTTP 协议相关命令实现
 * @author Falcon Team
 * @date 2025-12-24
 *
 * 设计参考: aria2/src/HttpInitiateConnectionCommand.h,
 *          aria2/src/HttpResponseCommand.h,
 *          aria2/src/HttpDownloadCommand.h
 */

#pragma once

#include <falcon/commands/command.hpp>
#include <falcon/download_options.hpp>
#include <falcon/http/http_request.hpp>
#include <falcon/net/socket_pool.hpp>
#include <memory>
#include <string>
#include <map>
#include <fstream>

namespace falcon {

// 前向声明
class DownloadEngineV2;

namespace net {
class PooledSocket;
}

/**
 * @brief HTTP 连接状态
 */
enum class HttpConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    REQUEST_SENT,
    RECEIVING,
    COMPLETE
};

/**
 * @brief HTTP 连接初始化命令
 *
 * 职责:
 * 1. 解析 URL（主机名、端口、路径）
 * 2. 建立 TCP 连接
 * 3. 可选: 建立 TLS 连接 (HTTPS)
 * 4. 准备 HTTP 请求数据
 *
 * 对应 aria2 的 HttpInitiateConnectionCommand
 * @see https://github.com/aria2/aria2/blob/master/src/HttpInitiateConnectionCommand.h
 */
class HttpInitiateConnectionCommand : public AbstractCommand {
public:
    /**
     * @brief 构造函数
     *
     * @param task_id 关联的任务 ID
     * @param url 目标 URL
     * @param options 下载选项
     */
    HttpInitiateConnectionCommand(TaskId task_id,
                                   std::string url,
                                   const DownloadOptions& options);

    ~HttpInitiateConnectionCommand() override;

    /**
     * @brief 执行连接初始化
     *
     * 执行流程:
     * 1. 解析 URL
     * 2. DNS 解析（如果需要）
     * 3. 从 Socket Pool 获取或创建连接
     * 4. 连接服务器（如需要）
     * 5. HTTPS: TLS 握手
     * 6. 准备 HTTP 请求头
     *
     * @param engine 下载引擎
     * @return true 连接成功完成
     * @return false 连接中，等待 Socket 可写
     */
    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override {
        return "HttpInitiateConnection";
    }

    /**
     * @brief 获取创建的 Socket 文件描述符
     */
    int socket_fd() const noexcept { return socket_fd_; }

    /**
     * @brief 获取 HTTP 请求对象
     */
    std::shared_ptr<HttpRequest> http_request() const noexcept {
        return http_request_;
    }

    /**
     * @brief 获取连接状态
     */
    HttpConnectionState connection_state() const noexcept {
        return connection_state_;
    }

private:
    bool resolve_host(const std::string& host, std::string& ip);
    bool create_socket();
    bool connect_socket();
    bool setup_tls();
    bool prepare_http_request();
    ExecutionResult send_http_request(DownloadEngineV2* engine);

    std::string url_;
    DownloadOptions options_;
    int socket_fd_ = -1;
    HttpConnectionState connection_state_ = HttpConnectionState::DISCONNECTED;
    std::shared_ptr<HttpRequest> http_request_;
    std::shared_ptr<net::PooledSocket> pooled_socket_;  // Socket Pool 中的连接
    std::string host_;
    std::string path_ = "/";
    uint16_t port_ = 80;
    bool use_https_ = false;

    std::string resolved_ip_;
    bool connect_in_progress_ = false;

    std::string request_data_;
    std::size_t request_sent_ = 0;
};

/**
 * @brief HTTP 响应处理命令
 *
 * 职责:
 * 1. 接收 HTTP 响应头
 * 2. 解析状态码、响应头
 * 3. 处理重定向
 * 4. 处理 Range 响应
 * 5. 判断是否需要分块下载
 *
 * 对应 aria2 的 HttpResponseCommand
 * @see https://github.com/aria2/aria2/blob/master/src/HttpResponseCommand.h
 */
class HttpResponseCommand : public AbstractCommand {
public:
    /**
     * @brief 构造函数
     *
     * @param task_id 关联的任务 ID
     * @param socket_fd Socket 文件描述符
     * @param request HTTP 请求对象
     * @param options 下载选项
     */
    HttpResponseCommand(TaskId task_id,
                        int socket_fd,
                        std::shared_ptr<HttpRequest> request,
                        const DownloadOptions& options);

    ~HttpResponseCommand() override;

    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override {
        return "HttpResponse";
    }

    /**
     * @brief 获取 HTTP 响应对象
     */
    std::shared_ptr<HttpResponse> http_response() const noexcept {
        return http_response_;
    }

    /**
     * @brief 检查是否需要重定向
     */
    bool is_redirect() const noexcept {
        return is_redirect_;
    }

    /**
     * @brief 获取重定向 URL
     */
    const std::string& redirect_url() const noexcept {
        return redirect_url_;
    }

    /**
     * @brief 检查是否支持断点续传
     */
    bool supports_resume() const noexcept {
        return supports_resume_;
    }

    /**
     * @brief 获取文件总大小
     */
    Bytes content_length() const noexcept {
        return content_length_;
    }

    /**
     * @brief 检查是否接受 Range 请求
     */
    bool accepts_range() const noexcept {
        return accepts_range_;
    }

private:
    ExecutionResult receive_response_headers(DownloadEngineV2* engine);
    bool parse_headers();
    bool parse_status_line(const std::string& line);
    bool parse_header_line(const std::string& line);
    bool handle_redirect();
    bool determine_download_strategy(DownloadEngineV2* engine);

    int socket_fd_;
    std::shared_ptr<HttpRequest> http_request_;
    std::shared_ptr<HttpResponse> http_response_;
    const DownloadOptions& options_;  // 引用下载选项

    // 响应解析状态
    std::string response_buffer_;
    std::string initial_body_;
    bool headers_received_ = false;
    int status_code_ = 0;
    std::map<std::string, std::string> headers_;

    // 响应信息
    bool is_redirect_ = false;
    std::string redirect_url_;
    bool supports_resume_ = false;
    Bytes content_length_ = 0;
    bool accepts_range_ = false;
};

/**
 * @brief HTTP 下载数据命令
 *
 * 职责:
 * 1. 接收 HTTP 响应体数据
 * 2. 写入到指定分段
 * 3. 处理分块传输编码
 * 4. 更新下载进度
 * 5. 检测下载完成
 *
 * 对应 aria2 的 HttpDownloadCommand
 * @see https://github.com/aria2/aria2/blob/master/src/DownloadCommand.h
 */
class HttpDownloadCommand : public AbstractCommand {
public:
    /**
     * @brief 构造函数
     *
     * @param task_id 关联的任务 ID
     * @param socket_fd Socket 文件描述符
     * @param response HTTP 响应对象
     * @param segment_id 分段 ID
     * @param offset 分段起始偏移
     * @param length 分段长度（0 表示到文件末尾）
     */
    HttpDownloadCommand(TaskId task_id,
                        int socket_fd,
                        std::shared_ptr<HttpResponse> response,
                        SegmentId segment_id,
                        Bytes offset,
                        Bytes length = 0,
                        std::string initial_data = {});

    ~HttpDownloadCommand() override;

    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override {
        return "HttpDownload";
    }

    /**
     * @brief 获取已下载字节数
     */
    Bytes downloaded_bytes() const noexcept {
        return downloaded_bytes_;
    }

    /**
     * @brief 获取下载速度（字节/秒）
     */
    Speed download_speed() const noexcept {
        return download_speed_;
    }

    /**
     * @brief 检查下载是否完成
     */
    bool is_complete() const noexcept {
        return download_complete_;
    }

private:
    ExecutionResult receive_data(DownloadEngineV2* engine);
    bool write_to_segment(const char* data, std::size_t size, DownloadEngineV2* engine);
    bool handle_chunked_encoding();
    void update_progress();
    bool check_completion();

    int socket_fd_;
    std::shared_ptr<HttpResponse> http_response_;
    SegmentId segment_id_;
    [[maybe_unused]] Bytes offset_;          // 当前分段在文件中的起始偏移
    Bytes length_;          // 分段长度（0 表示到末尾）
    [[maybe_unused]] Bytes current_offset_;  // 当前写入位置

    // 下载状态
    Bytes downloaded_bytes_ = 0;
    Speed download_speed_ = 0;
    bool download_complete_ = false;
    bool file_opened_ = false;
    std::string initial_data_;
    bool initial_written_ = false;
    std::ofstream output_;

    // 分块传输编码状态
    bool chunked_encoding_ = false;
    [[maybe_unused]] std::size_t chunk_remaining_ = 0;
    [[maybe_unused]] bool chunk_end_ = false;

    // 进度计算
    std::chrono::steady_clock::time_point last_update_;
    Bytes bytes_since_last_update_ = 0;
};

/**
 * @brief HTTP 请求重试命令
 *
 * 当下载失败时，根据重试策略决定是否重试
 */
class HttpRetryCommand : public AbstractCommand {
public:
    HttpRetryCommand(TaskId task_id,
                     const std::string& url,
                     const DownloadOptions& options,
                     int retry_count);

    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override {
        return "HttpRetry";
    }

    /**
     * @brief 检查是否应该重试
     */
    bool should_retry() const noexcept {
        return retry_count_ <= max_retries_;
    }

private:
    std::string url_;
    DownloadOptions options_;
    int retry_count_;
    int max_retries_;
    std::chrono::seconds retry_wait_;
};

/**
 * @brief Socket 事件注册辅助类
 *
 * 用于在 DownloadEngine 中注册 Socket I/O 事件
 */
struct SocketEntry {
    int fd;
    TaskId task_id;
    CommandId command_id;
    int events;  // READ=1, WRITE=2, ERROR=4

    SocketEntry(int f, TaskId t, CommandId c, int e)
        : fd(f), task_id(t), command_id(c), events(e) {}
};

} // namespace falcon
