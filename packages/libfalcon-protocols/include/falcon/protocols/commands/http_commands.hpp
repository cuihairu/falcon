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
#include <vector>
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
    TLS_HANDSHAKING,  ///< TLS 握手进行中（WANT_* 挂起等 socket 事件重入续推）
    /// 向 HTTP 代理发送 CONNECT 请求中（非阻塞 send 挂起等重入续推）
    PROXY_TUNNEL_SEND,
    /// 等待/接收代理对 CONNECT 的最终应答（收满 \r\n\r\n 判定）
    PROXY_TUNNEL_RECV,
    REQUEST_SENT,
    RECEIVING,
    COMPLETE
};

/**
 * @brief 代理配置解析结论
 *
 * V2 数据面仅支持明文 HTTP 代理（absolute-form 请求行 + CONNECT
 * 隧道）；socks 系列与 TLS 代理明确判 Unsupported——M2 适配层据此
 * 回退 V1 curl（libcurl 自带 socks 支持）
 */
enum class HttpProxyKind {
    None,         ///< 无代理（proxy 配置为空）
    HttpProxy,    ///< 明文 HTTP 代理
    Unsupported   ///< socks / https 代理 / 未知 scheme / 无法解析
};

struct HttpProxyConfig {
    HttpProxyKind kind = HttpProxyKind::None;
    std::string host;
    uint16_t port = 0;
    std::string username;  ///< 空 = 无认证
    std::string password;
};

/**
 * @brief 解析 DownloadOptions 代理配置（纯函数，便于单测）
 *
 * 接受 `http://[user:pass@]host[:port]` 与无 scheme 的
 * `[user:pass@]host[:port]`（按明文 HTTP 代理，默认端口 80）；
 * userinfo 凭据优先，缺省回落 proxy_username/proxy_password 字段。
 */
HttpProxyConfig parse_http_proxy(const DownloadOptions& options);

/**
 * @brief TLS 握手推进结果
 *
 * 非阻塞 socket 上 WANT_READ/WANT_WRITE 属握手进行中而非失败——
 * 调用方应置 TLS_HANDSHAKING 状态注册对应 socket 事件等待重入
 */
enum class TlsHandshakeResult {
    OK,          ///< 握手完成（证书校验通过或未启用校验）
    WANT_READ,   ///< 握手挂起，等待 socket 可读后续推
    WANT_WRITE,  ///< 握手挂起，等待 socket 可写后续推
    FAILED       ///< 握手失败（含证书校验失败，必须断连）
};

#ifdef FALCON_ENABLE_OPENSSL
/**
 * @brief TLS 会话共享句柄（SSL_free 删除器）
 *
 * 初始连接完成握手后持有，沿命令链（响应 → 下载）共享：下载命令
 * 必须经 SSL_read 解密收响应体，对 TLS 连接做明文 recv 只能读到
 * 密文。共享所有权取代裸指针传递——初始连接命令在调度响应命令后
 * 随即出队销毁，裸指针传递即悬垂
 */
struct HttpTlsSessionDeleter {
    void operator()(SSL* ssl) const noexcept {
        if (ssl) {
            SSL_free(ssl);
        }
    }
};
using HttpTlsSessionPtr = std::shared_ptr<SSL>;
#endif

class RequestGroup;
class HttpInitiateConnectionCommand;

/**
 * @brief 按任务组的续传状态为初始连接设置断点范围（断点续传）
 *
 * 选择第一个未完成且已有落盘进度的分段，请求从断点继续（附带
 * If-Range 验证头）；全部段无进度时不设置 Range（普通全新 GET）。
 * 初始连接始终承载该段——跨会话恢复时段 0 可能已完成，此时承载
 * 第一个未完成段
 *
 * @return true 设置了续传范围
 */
bool apply_group_resume_range(HttpInitiateConnectionCommand& cmd,
                              const RequestGroup& group);

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

    const char* name() const override;  // 实现集中在 .cpp(name() 纯为异常日志服务,inline 会让每 TU 生成实例行)

    /**
     * @brief 获取创建的 Socket 文件描述符
     */
    int socket_fd() const noexcept override { return socket_fd_; }

    bool retry_expired_segment(DownloadEngineV2* engine) override;

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
     * @brief 设置 If-Range 验证头（断点续传内容一致性防护）
     *
     * 携带 Range 的请求附 If-Range 后：服务器资源未变更才回 206（从
     * 断点续传），已变更则回 200 全量——响应命令据此放弃续传按全新
     * 下载处理，绝不把新内容错位写进旧断点。ETag 优先，其次
     * Last-Modified（RFC 7233）
     */
    void set_if_range(std::string value) { if_range_ = std::move(value); }

    /**
     * @brief 标记本连接是段级换源重试的重建连接
     *
     * 带 Range 的初始连接有两种来源：暂停恢复的续传响应（走
     * schedule_resume_download，多段防御触发 abandon）与段 0 的换源
     * 重试重建连接（必须按段路由——落进恢复路径会把全组进度作废）。
     * 两者从 Range 头无法区分（第一个未完成段可能恰是段 0），显式
     * 标记沿命令链传给响应命令
     */
    void set_segment_retry(bool value) noexcept { segment_retry_ = value; }

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

    /**
     * @brief 设置重定向深度
     *
     * 跟随重定向重新发起连接时携带（响应命令调度时传入），用于
     * 重定向链上限判定；0 = 非重定向的原始请求
     */
    void set_redirect_depth(int depth) noexcept { redirect_depth_ = depth; }

    /**
     * @brief 获取重定向深度
     */
    int redirect_depth() const noexcept { return redirect_depth_; }

private:
    bool resolve_host(const std::string& host, std::string& ip);
    bool create_socket();
    bool connect_socket();
    TlsHandshakeResult setup_tls();  // 始终声明，实现根据 FALCON_ENABLE_OPENSSL 条件编译
    ExecutionResult advance_tls_handshake(DownloadEngineV2* engine);
    ExecutionResult send_proxy_connect(DownloadEngineV2* engine);
    ExecutionResult receive_proxy_connect_response(DownloadEngineV2* engine);
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

    // 重定向深度（沿命令链传递，超链防护）
    int redirect_depth_ = 0;

    // 多连接分段信息（初始连接无 Range；段 1..N-1 的连接带 Range；
    // 断点续传时初始连接也可承载带 Range 的续传段）
    bool has_range_ = false;
    SegmentId range_segment_id_ = 0;
    Bytes range_offset_ = 0;
    Bytes range_length_ = 0;

    // 段级换源重试的重建连接标记（沿命令链传给响应命令决定响应路由）
    bool segment_retry_ = false;

    // If-Range 验证值（续传请求附带；非续传请求为空）
    std::string if_range_;

    std::string resolved_ip_;
    bool connect_in_progress_ = false;

    // 代理配置（构造时 parse 一次；None = 直连，语义零变化）
    HttpProxyConfig proxy_cfg_;

    // CONNECT 隧道状态（仅 HTTPS + 代理使用）
    std::string proxy_request_;   ///< 待发 CONNECT 报文
    std::size_t proxy_sent_ = 0;  ///< 已发送字节数
    std::string proxy_response_;  ///< 累积的代理应答（收满 \r\n\r\n 判定）

    std::string request_data_;
    std::size_t request_sent_ = 0;

#ifdef FALCON_ENABLE_OPENSSL
    SSL_CTX* ssl_ctx_ = nullptr;
    HttpTlsSessionPtr ssl_conn_;  // 握手完成后沿命令链（响应 → 下载）共享
    bool tls_started_ = false;    // SSL 对象只建一次，重入在其上续握手
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
     * @param tls_session TLS 会话（HTTPS 时使用；调度下载命令时继续
     *        传递——响应体必须经 SSL_read 解密）
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
                        HttpTlsSessionPtr tls_session = {},
#endif
                        bool use_https = false,
                        std::string source_url = {},
                        SegmentId segment_id = 0,
                        Bytes range_offset = 0,
                        Bytes range_length = 0);

    ~HttpResponseCommand() override;

    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override;  // 实现集中在 .cpp(name() 纯为异常日志服务,inline 会让每 TU 生成实例行)

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
     * @brief 获取持有的 Socket 文件描述符（引擎停机排水用）
     */
    int socket_fd() const noexcept override { return socket_fd_; }

    bool retry_expired_segment(DownloadEngineV2* engine) override;

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

    /**
     * @brief 标记响应来自段级换源重试的重建连接（连接阶段携带而来）
     *
     * 段 0 重试连接的响应 segment_id 为 0，与初始恢复连接从 Range 头
     * 无法区分；此标记决定 determine_download_strategy 按段路由而非
     * 落进初始恢复路径（那里的多段防御会 abandon 全组进度）
     */
    void set_segment_retry_routing(bool value) noexcept {
        segment_retry_routing_ = value;
    }

    /**
     * @brief 设置重定向深度（连接阶段携带而来）
     *
     * HttpInitiateConnectionCommand 把自己的深度带进响应命令，
     * 超链判定基于此值
     */
    void set_redirect_depth(int depth) noexcept { redirect_depth_ = depth; }

    /**
     * @brief 获取重定向深度
     */
    int redirect_depth() const noexcept { return redirect_depth_; }

private:
    ExecutionResult receive_response_headers(DownloadEngineV2* engine);
    bool parse_headers();
    bool parse_status_line(const std::string& line);
    bool parse_header_line(const std::string& line);
    bool handle_redirect(DownloadEngineV2* engine);
    bool determine_download_strategy(DownloadEngineV2* engine);
    bool schedule_multi_segment_download(DownloadEngineV2* engine);
    bool validate_segment_response() const;

    /// 断点续传：初始连接响应按组内续传计划调度（校验 206/全量一致性，
    /// 失败即放弃续传转全新下载）
    bool schedule_resume_download(DownloadEngineV2* engine, RequestGroup& group);

    /// 为除 except_segment 外的所有未完成分段建立续传连接
    ///（已完成的分段在 prepare_resumed_multi_segment 中预记账）
    void schedule_remaining_resume_segments(DownloadEngineV2* engine,
                                            RequestGroup& group,
                                            std::size_t except_segment);

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

    // 段级换源重试的重建连接标记（决定段 0 重试响应的路由分支）
    bool segment_retry_routing_ = false;

    // 重定向深度（连接阶段携带而来，超链防护）
    int redirect_depth_ = 0;

    // TLS/HTTPS 支持
    bool use_https_ = false;
#ifdef FALCON_ENABLE_OPENSSL
    HttpTlsSessionPtr tls_session_;  ///< 与初始连接共享的 TLS 会话
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
    /// Transfer-Encoding: chunked（RFC 7230：优先于 Content-Length，
    /// 总长未知——headers 解析完成后统一置零长度与 Range 标志）
    bool is_chunked_response_ = false;
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
     * @param initial_data 响应头后已随同到达的响应体前缀
     * @param resumed_bytes 断点续传预置字节数：本分段在上一会话已确认
     *        落盘的进度（段 0/单连接续传用——写位置 = offset + 该值，
     *        完成判定/进度分母仍按段全长；其余段续传由 Range 起点天然
     *        承载，保持默认 0）
     * @param truncate_output 打开输出文件时是否截断（全新下载的段 0 用
     *        默认 true 创建/重置文件；断点续传的段 0 必须传 false——
     *        临时文件里存着本段与其他段的已落盘数据，截断即销毁断点）
     * @param tls_session TLS 会话（HTTPS 时必传——响应体必须经
     *        SSL_read 解密，对 TLS 连接做明文 recv 只能读到密文）
     */
    HttpDownloadCommand(TaskId task_id,
                        int socket_fd,
                        std::shared_ptr<HttpResponse> response,
                        SegmentId segment_id,
                        Bytes offset,
                        Bytes length = 0,
                        std::string initial_data = {},
                        Bytes resumed_bytes = 0,
                        bool truncate_output = true,
                        bool chunked = false,
                        std::string source_url = {}
#ifdef FALCON_ENABLE_OPENSSL
                        ,
                        HttpTlsSessionPtr tls_session = {}
#endif
                        );

    ~HttpDownloadCommand() override;

    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override;  // 实现集中在 .cpp(name() 纯为异常日志服务,inline 会让每 TU 生成实例行)

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
     * @brief 获取持有的 Socket 文件描述符（引擎停机排水用）
     */
    int socket_fd() const noexcept override { return socket_fd_; }

    /**
     * @brief 暂停清扫检查点：冲刷残留缓冲并把最终落盘进度上报任务组
     * （析构只冲刷不上报，清扫路径负责固化断点）
     */
    void prepare_sweep(DownloadEngineV2* engine) override;

    bool retry_expired_segment(DownloadEngineV2* engine) override;

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

    /// 冲刷磁盘写缓冲到输出文件（缓冲为空时直接成功）
    /// @return false 表示落盘失败（stream 置错）
    bool flush_write_buffer();

    /// 冲刷写缓冲并关闭输出文件；完成/失败收尾与析构兜底共用
    /// @return false 表示冲刷或关闭失败
    bool finish_output();

    /// 发布下载成果：临时文件改名为最终名（temp_extension 消费点）。
    /// 无临时扩展名或已发布时直接成功
    /// @return false 表示改名失败（错误已写入 task）
    bool publish_output(const DownloadTask::Ptr& task);
    void complete_group_if_all_segments_done(DownloadEngineV2* engine,
                                             RequestGroup& group,
                                             const DownloadTask::Ptr& task,
                                             bool success);

    /**
     * @brief 段错误收口（先试段级换源重试，预算耗尽才终态化）
     *
     * @return true 已终态收口（组 FAILED）；false 重试已接管（组保持
     *         ACTIVE，调用方按非失败收口处理）或组非可失败态
     */
    bool fail_group_on_segment_error(DownloadEngineV2* engine,
                                     RequestGroup& group,
                                     const DownloadTask::Ptr& task);

    /// 已落盘进度的段内绝对值：恢复段连接按剩余量请求（offset_ 是
    /// 断点起点而非段计划起点），下载计数是剩余量——上报控制文件前
    /// 换算成段计划坐标，否则被单调门忽略、断点永不推进
    Bytes flushed_progress_absolute(const RequestGroup& group) const;

    int socket_fd_;
    std::shared_ptr<HttpResponse> http_response_;
    SegmentId segment_id_;
    Bytes offset_;                          // 当前分段在文件中的起始偏移
    Bytes length_;          // 分段长度（0 表示到末尾）
    [[maybe_unused]] Bytes current_offset_;  // 当前写入位置
    /// 本命令数据来源 URL（段级换源重试的轮转基准；空 = 不参与轮转）
    std::string source_url_;

    // 下载状态
    Bytes downloaded_bytes_ = 0;
    Speed download_speed_ = 0;
    bool download_complete_ = false;
    bool file_opened_ = false;
    std::string initial_data_;
    bool initial_written_ = false;
    bool truncate_output_ = true;  // 打开输出文件是否截断（续传的段 0 为 false）
    std::ofstream output_;

    // 磁盘写缓冲（enable_disk_cache/disk_cache_size 消费点）：数据先
    // 攒在内存、攒满 disk_cache_size 一次性落盘，减少小块写 syscall
    // 与多段模式逐块 seekp 的流缓冲冲刷；缓冲容量在文件打开时从引擎
    // 配置取定，0 = 直写。异常路径（超时清理/停机排水直接销毁命令，
    // 不经 execute 收尾分支）由析构兜底冲刷，防缓冲数据静默丢失
    std::vector<char> write_buffer_;
    std::size_t write_buffer_capacity_ = 0;

    // 临时文件路径（temp_extension 消费点）：非空时数据实际写此路径，
    // 组完成时原子改名为最终名；与最终名相同 = 直写，无发布步骤
    std::string write_path_;

    // 分块传输编码状态
    bool chunked_encoding_ = false;
    std::size_t chunk_remaining_ = 0;  // 当前块剩余字节数
    bool chunk_end_ = false;           // 是否到达块结束标记
    std::string chunk_buffer_;         // 分块编码解析缓冲区

#ifdef FALCON_ENABLE_OPENSSL
    // TLS 会话（HTTPS 下载体必须经 SSL_read 解密；与初始连接共享）
    HttpTlsSessionPtr tls_session_;
#endif
    enum class ChunkParseState {
        READ_SIZE,      // 读取块大小
        READ_DATA,      // 读取块数据
        READ_CR,        // 读取数据后的 CR
        READ_LF,        // 读取数据后的 LF
        READ_TRAILER    // 读取尾部（可选的头部）
    };
    ChunkParseState chunk_state_ = ChunkParseState::READ_SIZE;
    std::string chunk_size_str_;      // 存储块大小字符串
    // 块大小行/尾部区域的 CR 已单独到达、LF 尚未到达（TCP 分片把
    // CRLF 拆开）：置位等待下批数据补判，CR 绝不预消费——预消费会让
    // LF 与后续字节被并进大小行，静默错帧
    bool chunk_cr_pending_ = false;

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

    const char* name() const override;  // 实现集中在 .cpp(name() 纯为异常日志服务,inline 会让每 TU 生成实例行)

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
 * @brief 段级换源重试命令（Metalink 多源分段，阶段2）
 *
 * 多段下载中某段的连接/传输失败时，由调度点（schedule_retry）在
 * 预算内换下一镜像重建该段的 Range 连接。与 HttpRetryCommand 分离：
 * 后者是初始连接语义（耗尽路径整组终态、Range 取第一个未完成段），
 * 改造它会波及单连接路径既有行为
 *
 * 预算判定收口在调度点（increment_segment_retry / options.max_retries），
 * 本命令只做「延迟 → 重建带 Range 的连接」，天然保证 finish_segment
 * 只在预算耗尽时被调用一次
 */
class HttpSegmentRetryCommand : public AbstractCommand {
public:
    HttpSegmentRetryCommand(TaskId task_id,
                            std::string url,
                            const DownloadOptions& options,
                            SegmentId segment_id,
                            Bytes offset,
                            Bytes length,
                            std::string if_range,
                            int retry_count);

    bool execute(DownloadEngineV2* engine) override;

    const char* name() const override;  // 实现集中在 .cpp(name() 纯为异常日志服务,inline 会让每 TU 生成实例行)

    /**
     * @brief 段失败调度点：预算内换源重建该段连接，预算耗尽返回 false
     *
     * 门禁：组存在且非终态/暂停 / 处于多分段模式 / uris 非空。
     * 剩余 Range 从组内控制文件确认进度推导（segment_progress），
     * If-Range 仅当轮转结果仍是主镜像（uris_[0]，ETag 归属者）才附带
     *
     * @return true 重试已调度（调用方按非失败收口处理）；false 预算
     *         耗尽或不可重试（调用方走既有失败收口）
     */
    static bool schedule_retry(DownloadEngineV2* engine,
                               TaskId task_id,
                               SegmentId segment_id,
                               Bytes plan_offset,
                               Bytes plan_length,
                               const std::string& failed_url);

private:
    std::string url_;
    DownloadOptions options_;
    SegmentId segment_id_;
    Bytes offset_;
    Bytes length_;
    std::string if_range_;
    int retry_count_;
    std::chrono::seconds retry_wait_;
    // 最早重试时刻：到点前以 NEED_RETRY 回队轮询（同 HttpRetryCommand）
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
