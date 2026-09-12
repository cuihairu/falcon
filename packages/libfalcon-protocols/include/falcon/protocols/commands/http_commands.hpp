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

#include <falcon/protocols/commands/command.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/protocols/http/http_request.hpp>
#include <falcon/protocols/net/socket_pool.hpp>
#include <memory>
#include <string>
#include <map>
#include <fstream>

#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/ssl.h>
#endif

namespace falcon {

// 前向声明
class DownloadEngineV2;
class RequestGroup;
class DownloadTask;

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
 * @brief HTTP 分段范围（多连接分段下载）
 */
struct HttpSegmentRange {
    Bytes offset = 0;  ///< 段起始偏移（含）
    Bytes length = 0;  ///< 段长度
};

/**
 * @brief 计算多连接分段下载的分段计划
 *
 * 参考 SegmentDownloader::initialize_segments 的等分策略：
 * - 段数不超过 max_connections，且不超过 content_length / min_segment_size
 *   （保证每段平均长度不低于最小分段大小）
 * - 不足一个最小分段的剩余部分并入最后一段
 *
 * @param content_length 文件总大小（>0）
 * @param min_segment_size 最小分段大小
 * @param max_connections 最大连接数
 * @return 分段列表（至少 1 个元素，恰好覆盖 [0, content_length)）
 */
std::vector<HttpSegmentRange> compute_http_segment_ranges(
    Bytes content_length,
    Bytes min_segment_size,
    std::size_t max_connections);

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

    /**
     * @brief 将本连接标记为多连接分段的第 segment_id 段
     *
     * 设置后 prepare_http_request() 会附加 Range 请求头，
     * 后续响应命令按 (segment_id, offset, length) 调度该段的下载命令。
     *
     * @param segment_id 分段编号（>0）
     * @param offset 段起始偏移
     * @param length 段长度
     */
    void set_range(SegmentId segment_id, Bytes offset, Bytes length);

    /**
     * @brief 是否为分段连接
     */
    bool has_range() const noexcept { return has_range_; }

    /**
     * @brief 获取分段编号
     */
    SegmentId range_segment_id() const noexcept { return range_segment_id_; }

    /**
     * @brief 获取段起始偏移
     */
    Bytes range_offset() const noexcept { return range_offset_; }

    /**
     * @brief 获取段长度
     */
    Bytes range_length() const noexcept { return range_length_; }

    /**
     * @brief 设置连接级重试计数
     *
     * 重试链重新进入连接阶段时携带（HttpRetryCommand 调度时传入），
     * 用于 max_retries 判定；0 = 首次尝试
     */
    void set_retry_count(int count) noexcept { retry_count_ = count; }

    /**
     * @brief 获取连接级重试计数
     */
    int retry_count() const noexcept { return retry_count_; }

private:
    bool resolve_host(const std::string& host, std::string& ip);
    bool create_socket();
    bool connect_socket();
    bool setup_tls();  // 始终声明，实现根据 FALCON_ENABLE_OPENSSL 条件编译
    bool prepare_http_request();
    ExecutionResult send_http_request(DownloadEngineV2* engine);
    void notify_segment_failure(DownloadEngineV2* engine, const std::string& reason);

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

    // 连接级重试计数（max_retries 语义：首连 + max_retries 次重试）
    int retry_count_ = 0;

    // 多连接分段信息（初始连接无 Range；段 1..N-1 的连接带 Range）
    bool has_range_ = false;
    SegmentId range_segment_id_ = 0;
    Bytes range_offset_ = 0;
    Bytes range_length_ = 0;

    std::string resolved_ip_;
    bool connect_in_progress_ = false;

    std::string request_data_;
    std::size_t request_sent_ = 0;

#ifdef FALCON_ENABLE_OPENSSL
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_conn_ = nullptr;
#endif
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
     * @param ssl_conn SSL 连接（HTTPS 时使用）
     * @param use_https 是否使用 HTTPS
     * @param source_url 原始完整 URL（多连接分段时为段 1..N-1 创建新连接用；
     *                   空串表示不启用多连接）
     * @param segment_id 分段编号（0 = 初始连接）
     * @param range_offset 分段起始偏移（segment_id > 0 时有效）
     * @param range_length 分段长度（segment_id > 0 时有效）
     */
    HttpResponseCommand(TaskId task_id,
                        int socket_fd,
                        std::shared_ptr<HttpRequest> request,
                        const DownloadOptions& options,
#ifdef FALCON_ENABLE_OPENSSL
                        void* ssl_conn = nullptr,
#endif
                        bool use_https = false,
                        std::string source_url = {},
                        SegmentId segment_id = 0,
                        Bytes range_offset = 0,
                        Bytes range_length = 0);

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
     * @brief 获取 HTTP 状态码
     */
    int status_code() const noexcept {
        return status_code_;
    }

    /**
     * @brief 检查是否接受 Range 请求
     */
    bool accepts_range() const noexcept {
        return accepts_range_;
    }

    /**
     * @brief 是否为多连接分段中的响应（segment_id > 0）
     */
    bool is_segment_response() const noexcept {
        return segment_id_ > 0;
    }

    /**
     * @brief 设置连接级重试计数（连接阶段携带而来）
     */
    void set_retry_count(int count) noexcept { retry_count_ = count; }

    /**
     * @brief 获取连接级重试计数
     */
    int retry_count() const noexcept { return retry_count_; }

private:
    ExecutionResult receive_response_headers(DownloadEngineV2* engine);
    bool parse_headers();
    bool parse_status_line(const std::string& line);
    bool parse_header_line(const std::string& line);
    bool handle_redirect();
    bool determine_download_strategy(DownloadEngineV2* engine);
    bool schedule_multi_segment_download(DownloadEngineV2* engine);
    bool validate_segment_response() const;

    int socket_fd_;
    std::shared_ptr<HttpRequest> http_request_;
    std::shared_ptr<HttpResponse> http_response_;
    // 必须按值持有：调用方（HttpInitiateConnectionCommand）把自己的
    // options_ 值成员传入，其命令对象在本命令执行前就会被引擎队列销毁，
    // 引用成员会悬垂（heap-use-after-free）
    const DownloadOptions options_;

    // 多连接分段信息
    std::string source_url_;       ///< 原始完整 URL（空 = 不启用多连接）
    SegmentId segment_id_ = 0;     ///< 分段编号（0 = 初始连接）
    Bytes range_offset_ = 0;       ///< 分段起始偏移
    Bytes range_length_ = 0;       ///< 分段长度

    // 连接级重试计数（连接阶段携带而来，用于响应头阶段失败后的
    // max_retries 判定）
    int retry_count_ = 0;

    // TLS/HTTPS 支持
    bool use_https_ = false;
#ifdef FALCON_ENABLE_OPENSSL
    void* ssl_conn_ = nullptr;  // SSL* 指针
#endif

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

    /**
     * @brief 获取分段 ID
     */
    SegmentId segment_id() const noexcept {
        return segment_id_;
    }

    /**
     * @brief 获取分段起始偏移
     */
    Bytes offset() const noexcept {
        return offset_;
    }

    /**
     * @brief 获取分段长度
     */
    Bytes length() const noexcept {
        return length_;
    }

private:
    ExecutionResult receive_data(DownloadEngineV2* engine);
    bool write_to_segment(const char* data, std::size_t size, DownloadEngineV2* engine);
    bool handle_chunked_encoding(const char* data, std::size_t size, DownloadEngineV2* engine);
    void update_progress();
    bool check_completion();
    void complete_group_if_all_segments_done(RequestGroup& group,
                                             const DownloadTask::Ptr& task,
                                             bool success);
    void fail_group_on_segment_error(RequestGroup& group,
                                     const DownloadTask::Ptr& task);

    int socket_fd_;
    std::shared_ptr<HttpResponse> http_response_;
    SegmentId segment_id_;
    Bytes offset_;                          // 当前分段在文件中的起始偏移
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
    std::size_t chunk_remaining_ = 0;  // 当前块剩余字节数
    bool chunk_end_ = false;           // 是否到达块结束标记
    std::string chunk_buffer_;         // 分块编码解析缓冲区
    enum class ChunkParseState {
        READ_SIZE,      // 读取块大小
        READ_DATA,      // 读取块数据
        READ_CR,        // 读取数据后的 CR
        READ_LF,        // 读取数据后的 LF
        READ_TRAILER    // 读取尾部（可选的头部）
    };
    ChunkParseState chunk_state_ = ChunkParseState::READ_SIZE;
    std::string chunk_size_str_;      // 存储块大小字符串

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

    /**
     * @brief 获取当前重试次数
     */
    int retry_count() const noexcept {
        return retry_count_;
    }

private:
    std::string url_;
    DownloadOptions options_;
    int retry_count_;
    int max_retries_;
    std::chrono::seconds retry_wait_;
    // 最早重试时刻：到点前 execute 以 NEED_RETRY 回队轮询，
    // 不阻塞引擎线程（旧实现 sleep_for 会停摆整个事件循环）
    std::chrono::steady_clock::time_point retry_at_;
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
