/**
 * @file http_commands.cpp
 * @brief HTTP 协议命令实现
 * @author Falcon Team
 * @date 2025-12-24
 */

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/detail/injection.hpp>
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
#include <filesystem>
#include <system_error>
#include <thread>
#include <vector>
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

#ifdef _WIN32
constexpr int kSendFlags = 0;
#elif defined(MSG_NOSIGNAL)
// 对端已 RST 的 socket 上写数据默认触发 SIGPIPE——默认处置是杀死整个
// 进程，daemon 不能因一个断连的服务器而死；MSG_NOSIGNAL 把它降级为
// EPIPE 错误，走正常失败收口
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;  // macOS：无 MSG_NOSIGNAL，SO_NOSIGPIPE 在
                               // socket 级设置（见 create_socket）
#endif

/// 段级换源的无状态镜像轮转：下一镜像 = uris_ 中 failed_url 的下一个
///（取模）。failed_url 不在列表（重定向后 URL）时回落主镜像 uris_[0]。
/// 无需游标成员，不触碰 try_next_uri 的钳位语义
std::string mirror_after(const RequestGroup& group,
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
std::unique_ptr<Command> make_connection_retry(DownloadEngineV2* engine,
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
void fail_group_terminal(DownloadEngineV2* engine, TaskId task_id,
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
// 断点续传范围辅助
//==============================================================================

bool apply_group_resume_range(HttpInitiateConnectionCommand& cmd,
                              const RequestGroup& group) {
    if (!group.has_resume_state()) {
        // 条件下载（conditional-get）：非续传初始连接携带组级
        // If-Modified-Since。续传连接不走此路（Range + If-Range 与
        // 条件头互斥——304 语义只在整文件 GET 上解释）。本函数同时
        // 服务于初始命令创建与连接级重试重建，两个站点一次覆盖
        const std::string& ims = group.if_modified_since();
        if (!ims.empty()) {
            cmd.set_if_modified_since(ims);
        }
        return false;
    }
    const ResumeControl plan = group.resume_plan();
    // 初始连接承载第一个"未完成且已有落盘进度"的段（进度为 0 的段从
    // 头下载即可，无需 Range；全部无进度 = 全新 GET）
    for (std::size_t i = 0; i < plan.segments.size(); ++i) {
        const auto& seg = plan.segments[i];
        if (seg.downloaded > 0 && seg.downloaded < seg.length) {
            cmd.set_range(static_cast<SegmentId>(i),
                          seg.offset + seg.downloaded,
                          seg.length - seg.downloaded);
            const std::string if_range = group.resume_if_range();
            if (!if_range.empty()) {
                cmd.set_if_range(if_range);
            }
            return true;
        }
    }
    return false;
}

//==============================================================================
// HttpInitiateConnectionCommand 实现
//==============================================================================

namespace {

/// 标准 Base64（RFC 4648，带填充）——Proxy-Authorization: Basic 用
std::string base64_encode(const std::string& in) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);

    // 字节先经 unsigned char 转 unsigned int（保 8 位语义、无符号扩
    // 展），移位全程在无符号域进行——uchar 参与移位会先提升为 int，
    // 再并入 unsigned 会触发符号转换告警
    const auto u8 = [&in](std::size_t k) -> unsigned int {
        return static_cast<unsigned char>(in[k]);
    };

    std::size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned int v = (u8(i) << 16) | (u8(i + 1) << 8) | u8(i + 2);
        out += kTable[(v >> 18) & 0x3fu];
        out += kTable[(v >> 12) & 0x3fu];
        out += kTable[(v >> 6) & 0x3fu];
        out += kTable[v & 0x3fu];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned int v = u8(i) << 16;
        out += kTable[(v >> 18) & 0x3fu];
        out += kTable[(v >> 12) & 0x3fu];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned int v = (u8(i) << 16) | (u8(i + 1) << 8);
        out += kTable[(v >> 18) & 0x3fu];
        out += kTable[(v >> 12) & 0x3fu];
        out += kTable[(v >> 6) & 0x3fu];
        out += '=';
    }
    return out;
}

/// 端口串严格解析（全数字且 1-65535；任何不合法都判 Unsupported）
bool parse_proxy_port(const std::string& text, uint16_t& port) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    unsigned value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    if (value == 0 || value > 65535) {
        return false;
    }
    port = static_cast<uint16_t>(value);
    return true;
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

}  // namespace

HttpProxyConfig parse_http_proxy(const DownloadOptions& options) {
    const std::string& raw = options.proxy;
    if (raw.empty()) {
        return {};  // None
    }

    auto unsupported = [] {
        HttpProxyConfig cfg;
        cfg.kind = HttpProxyKind::Unsupported;
        return cfg;
    };

    std::string rest = raw;
    uint16_t default_port = 80;

    if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    } else if (rest.rfind("https://", 0) == 0 || rest.rfind("socks", 0) == 0) {
        // TLS 代理与 socks 系列不支持——适配层回退 V1 curl
        return unsupported();
    } else if (rest.find("://") != std::string::npos) {
        return unsupported();  // 未知 scheme
    }  // 无 scheme：按明文 HTTP 代理（curl 对无 scheme 代理同语义）

    // proxy_type 显式声明 socks 的兜底（字段语义：覆盖 URL scheme）
    if (ascii_lower(options.proxy_type).find("socks") != std::string::npos) {
        return unsupported();
    }

    // userinfo：user:pass@ 优先于独立凭据字段
    std::string username;
    std::string password;
    const auto at = rest.find('@');
    if (at != std::string::npos) {
        const std::string userinfo = rest.substr(0, at);
        rest = rest.substr(at + 1);
        const auto colon = userinfo.find(':');
        username = colon == std::string::npos ? userinfo
                                              : userinfo.substr(0, colon);
        if (colon != std::string::npos) {
            password = userinfo.substr(colon + 1);
        }
    }

    // authority：host[:port]（剥路径；V2 数据面为 AF_INET，IPv6 字面量
    // 代理明确判 Unsupported）
    if (!rest.empty() && rest[0] == '/') {
        return unsupported();
    }
    const auto slash = rest.find('/');
    const std::string authority =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    if (authority.empty() || authority.find('[') != std::string::npos) {
        return unsupported();
    }

    std::string host = authority;
    uint16_t port = default_port;
    const auto colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        if (!parse_proxy_port(authority.substr(colon + 1), port)) {
            return unsupported();
        }
    }

    HttpProxyConfig cfg;
    cfg.kind = HttpProxyKind::HttpProxy;
    cfg.host = host;
    cfg.port = port;
    cfg.username = username.empty() ? options.proxy_username : username;
    cfg.password = password.empty() ? options.proxy_password : password;
    return cfg;
}

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

    // 代理配置 parse 一次（Unsupported 的拒绝收口在 request_group 门禁；
    // 此处 None 之外均为已验证的 HttpProxy）
    proxy_cfg_ = parse_http_proxy(options);
    if (proxy_cfg_.kind == HttpProxyKind::HttpProxy) {
        FALCON_LOG_INFO_STREAM("使用 HTTP 代理: " << proxy_cfg_.host << ":"
                                                  << proxy_cfg_.port);
    }
}

HttpInitiateConnectionCommand::~HttpInitiateConnectionCommand() {
#ifdef FALCON_ENABLE_OPENSSL
    // SSL 连接由 ssl_conn_（共享句柄）自动释放：命令链上的响应/下载
    // 命令可能仍在使用会话，此处不能抢先 SSL_free
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
#endif
    // Socket 的清理由连接池负责
}

const char* HttpInitiateConnectionCommand::name() const {
    return "HttpInitiateConnection";
}

bool HttpInitiateConnectionCommand::execute(DownloadEngineV2* engine) {
    try {
        if (!engine) {
            return handle_result(ExecutionResult::ERROR_OCCURRED);
        }

        // 已暂停：静默退出（连接阶段无落盘状态）
        {
            auto* group_man = engine->request_group_man();
            auto* group = group_man ? group_man->find_group(get_task_id()) : nullptr;
            if (group && group->status() == RequestGroupStatus::PAUSED) {
                if (socket_fd_ >= 0) {
                    close_socket_fd(socket_fd_);
                    socket_fd_ = -1;
                }
                return handle_result(ExecutionResult::OK);
            }
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
                [[fallthrough]];

            case HttpConnectionState::CONNECTING:
                // 检查连接是否完成（使用 getsockopt SO_ERROR）；同步
                // 连接路径刚连上，复核恒为 0
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
                if (use_https_) {
                    // HTTPS 经代理：先建 CONNECT 隧道再握手（隧道内字节
                    // 端到端加密，代理只见 host:port）；明文代理与直连
                    // 均直接发请求
                    if (proxy_cfg_.kind == HttpProxyKind::HttpProxy) {
                        connection_state_ =
                            HttpConnectionState::PROXY_TUNNEL_SEND;
                    } else {
                        connection_state_ =
                            HttpConnectionState::TLS_HANDSHAKING;
                    }
                }
                [[fallthrough]];

            case HttpConnectionState::PROXY_TUNNEL_SEND:
                if (connection_state_ == HttpConnectionState::PROXY_TUNNEL_SEND) {
                    // 发送 CONNECT：未发完注册 WRITE 挂起，发完置
                    // PROXY_TUNNEL_RECV 注册 READ 挂起等代理应答——两个
                    // 挂起均直接返回，不同轮误落到请求发送
                    return handle_result(send_proxy_connect(engine));
                }
                [[fallthrough]];

            case HttpConnectionState::PROXY_TUNNEL_RECV:
                if (connection_state_ == HttpConnectionState::PROXY_TUNNEL_RECV) {
                    // 代理应答未到齐：注册 READ 挂起；到齐且 2xx：置
                    // TLS_HANDSHAKING 落下方 TLS case 同轮推进；非 2xx
                    // 失败收口
                    const auto tunnel_res = receive_proxy_connect_response(engine);
                    if (tunnel_res != ExecutionResult::OK) {
                        return handle_result(tunnel_res);
                    }
                }
                [[fallthrough]];

            case HttpConnectionState::TLS_HANDSHAKING:
                if (connection_state_ == HttpConnectionState::TLS_HANDSHAKING) {
                    // 推进/继续 TLS 握手：WANT_* 挂起等 socket 事件重入
                    // （重入时在既有 SSL 对象上续推），完成后落请求发送；
                    // 失败（含证书校验失败）断连收口
                    const auto tls_res = advance_tls_handshake(engine);
                    if (tls_res != ExecutionResult::OK) {
                        return handle_result(tls_res);
                    }
                }

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
#ifdef SO_NOSIGPIPE
    // macOS 无 MSG_NOSIGNAL：socket 级禁用 SIGPIPE（写对端已关的连接
    // 得 EPIPE 而非进程信号）
    int no_sigpipe = 1;
    setsockopt(socket_fd_, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe,
               sizeof(no_sigpipe));
#endif
#endif

    FALCON_LOG_DEBUG_STREAM("创建 Socket: fd=" << socket_fd_);
    return true;
}

bool HttpInitiateConnectionCommand::connect_socket() {
    // 代理生效时连接代理服务器（absolute-form 请求与 CONNECT 隧道均在
    // 代理连接上承载）；目标主机名的解析延迟到隧道建立之后（TLS 握手
    // 也只与目标主机相关）
    const bool use_proxy = proxy_cfg_.kind == HttpProxyKind::HttpProxy;
    const std::string& connect_host = use_proxy ? proxy_cfg_.host : host_;
    const uint16_t connect_port = use_proxy ? proxy_cfg_.port : port_;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(connect_port);

    connect_in_progress_ = false;

    std::string ip = connect_host;
    if (inet_pton(AF_INET, connect_host.c_str(), &addr.sin_addr) <= 0) {
        if (!resolved_ip_.empty()) {
            ip = resolved_ip_;
        } else {
            if (!resolve_host(connect_host, ip)) {
                FALCON_LOG_ERROR_STREAM("解析主机失败: " << connect_host);
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
    FALCON_LOG_INFO_STREAM((use_proxy ? "正在连接代理 " : "正在连接 ")
                                 << connect_host << ":" << connect_port
                                 << (connect_in_progress_ ? " (in progress)" : " (connected)"));
    return true;
}

#ifdef FALCON_ENABLE_OPENSSL
TlsHandshakeResult HttpInitiateConnectionCommand::setup_tls() {
    // SSL 对象只创建一次：非阻塞握手的 WANT_* 重入在既有对象上续推
    //（每次 SSL_new 会丢弃已完成的握手进度且泄漏旧对象）
    if (!tls_started_) {
        if (!ssl_ctx_) {
            // 创建 SSL_CTX
            const SSL_METHOD* method = TLS_client_method();
            if (!method) {
                FALCON_LOG_ERROR_STREAM("无法获取 TLS 方法: " << ERR_error_string(ERR_get_error(), nullptr));
                return TlsHandshakeResult::FAILED;
            }

            ssl_ctx_ = SSL_CTX_new(method);
            if (!ssl_ctx_) {
                FALCON_LOG_ERROR_STREAM("无法创建 SSL_CTX: " << ERR_error_string(ERR_get_error(), nullptr));
                return TlsHandshakeResult::FAILED;
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

        // 创建 SSL 连接（共享句柄：命令链上的响应/下载命令继续使用）
        ssl_conn_ = HttpTlsSessionPtr(SSL_new(ssl_ctx_), HttpTlsSessionDeleter{});
        if (!ssl_conn_) {
            FALCON_LOG_ERROR_STREAM("无法创建 SSL 连接: " << ERR_error_string(ERR_get_error(), nullptr));
            return TlsHandshakeResult::FAILED;
        }

        // 绑定 Socket 到 SSL
        if (SSL_set_fd(ssl_conn_.get(), static_cast<int>(socket_fd_)) != 1) {
            FALCON_LOG_ERROR_STREAM("无法绑定 Socket 到 SSL: " << ERR_error_string(ERR_get_error(), nullptr));
            return TlsHandshakeResult::FAILED;
        }

        // 设置 SNI 主机名
        SSL_set_tlsext_host_name(ssl_conn_.get(), host_.c_str());

        // 证书校验绑定期望主机名：verify 开启时 SSL_get_verify_result
        // 的结论因此同时覆盖证书链与主机名——替代此前的 WARN-only
        // 主机名检查（告警后照常收数据，校验形同虚设）
        if (options_.verify_ssl && SSL_set1_host(ssl_conn_.get(), host_.c_str()) != 1) {
            FALCON_LOG_ERROR_STREAM("无法设置证书主机名校验: " << host_);
            return TlsHandshakeResult::FAILED;
        }

        FALCON_LOG_INFO_STREAM("开始 TLS 握手: " << host_);
        tls_started_ = true;
    }

    // 执行/继续 TLS 握手（非阻塞 socket：WANT_* 属进行中而非失败）
    const int ret = SSL_connect(ssl_conn_.get());
    if (ret != 1) {
        const int error = SSL_get_error(ssl_conn_.get(), ret);

        if (error == SSL_ERROR_WANT_READ) {
            FALCON_LOG_DEBUG_STREAM("TLS 握手等待可读: " << host_);
            return TlsHandshakeResult::WANT_READ;
        }
        if (error == SSL_ERROR_WANT_WRITE) {
            FALCON_LOG_DEBUG_STREAM("TLS 握手等待可写: " << host_);
            return TlsHandshakeResult::WANT_WRITE;
        }

        // SSL_VERIFY_PEER 下证书校验失败在握手阶段即中止，优先给出
        // 校验结论（通用错误串看不到原因）
        if (options_.verify_ssl) {
            const long verify_result = SSL_get_verify_result(ssl_conn_.get());
            if (verify_result != X509_V_OK) {
                FALCON_LOG_ERROR_STREAM("证书验证失败: "
                                        << X509_verify_cert_error_string(verify_result));
                return TlsHandshakeResult::FAILED;
            }
        }

        char error_buf[256];
        ERR_error_string_n(ERR_get_error(), error_buf, sizeof(error_buf));
        FALCON_LOG_ERROR_STREAM("TLS 握手失败: " << error_buf);
        return TlsHandshakeResult::FAILED;
    }

    // 握手成功的最终校验兜底：VERIFY_PEER 下校验失败走不到这里，此
    // 分支防验证模式与预期不符——校验失败即硬断连，绝不 WARN 后继续
    // 收数据（TLS 形同虚设）
    if (options_.verify_ssl) {
        const long verify_result = SSL_get_verify_result(ssl_conn_.get());
        if (verify_result != X509_V_OK) {
            FALCON_LOG_ERROR_STREAM("证书验证失败: "
                                    << X509_verify_cert_error_string(verify_result));
            return TlsHandshakeResult::FAILED;
        }
        X509* cert = SSL_get_peer_certificate(ssl_conn_.get());
        if (!cert) {
            FALCON_LOG_ERROR_STREAM("未收到服务器证书");
            return TlsHandshakeResult::FAILED;
        }
        X509_free(cert);
    }

    // 获取使用的密码套件
    const char* cipher = SSL_get_cipher(ssl_conn_.get());
    FALCON_LOG_INFO_STREAM("TLS 握手成功，使用加密套件: " << (cipher ? cipher : "unknown"));

    return TlsHandshakeResult::OK;
}
#endif

AbstractCommand::ExecutionResult
HttpInitiateConnectionCommand::advance_tls_handshake(
    DownloadEngineV2* engine) {
#ifndef FALCON_ENABLE_OPENSSL
    // 无 OpenSSL：HTTPS 不可用，明确失败
    FALCON_LOG_ERROR_STREAM("HTTPS 支持需要启用 OpenSSL");
    notify_segment_failure(engine, "HTTPS support requires OpenSSL");
    close_socket_fd(socket_fd_);
    socket_fd_ = -1;
    return ExecutionResult::ERROR_OCCURRED;
#else
    switch (setup_tls()) {
    case TlsHandshakeResult::OK:
        connection_state_ = HttpConnectionState::CONNECTED;
        return ExecutionResult::OK;
    case TlsHandshakeResult::WANT_READ:
        connection_state_ = HttpConnectionState::TLS_HANDSHAKING;
        engine->register_socket_event(
            socket_fd_, static_cast<int>(net::IOEvent::READ), id());
        return ExecutionResult::WAIT_FOR_SOCKET;
    case TlsHandshakeResult::WANT_WRITE:
        connection_state_ = HttpConnectionState::TLS_HANDSHAKING;
        engine->register_socket_event(
            socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
        return ExecutionResult::WAIT_FOR_SOCKET;
    case TlsHandshakeResult::FAILED:
    default:
        break;
    }

    FALCON_LOG_ERROR_STREAM("TLS 握手失败: " << host_);
    notify_segment_failure(engine, "TLS handshake failed: " + host_);
    close_socket_fd(socket_fd_);
    socket_fd_ = -1;
    return ExecutionResult::ERROR_OCCURRED;
#endif
}

AbstractCommand::ExecutionResult
HttpInitiateConnectionCommand::send_proxy_connect(
    DownloadEngineV2* engine) {
    if (proxy_request_.empty()) {
        // RFC 7231 §4.3.6：CONNECT 的 target 是 authority-form
        proxy_request_ = "CONNECT " + host_ + ":" + std::to_string(port_) +
                         " HTTP/1.1\r\n"
                         "Host: " + host_ + ":" + std::to_string(port_) + "\r\n";
        if (!options_.user_agent.empty()) {
            proxy_request_ += "User-Agent: " + options_.user_agent + "\r\n";
        }
        if (!proxy_cfg_.username.empty()) {
            proxy_request_ +=
                "Proxy-Authorization: Basic " +
                base64_encode(proxy_cfg_.username + ":" + proxy_cfg_.password) +
                "\r\n";
        }
        proxy_request_ += "\r\n";
        proxy_sent_ = 0;
    }

    while (proxy_sent_ < proxy_request_.size()) {
        const char* data = proxy_request_.data() + proxy_sent_;
        const std::size_t remaining = proxy_request_.size() - proxy_sent_;
#ifdef _WIN32
        ssize_t n;
        if (::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::ProxyConnectSendFail)) {
            WSASetLastError(WSAECONNRESET);
            n = -1;
        } else {
            n = send(socket_fd_, data, static_cast<int>(remaining),
                     kSendFlags);
        }
#else
        ssize_t n;
        if (::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::ProxyConnectSendFail)) {
            errno = ECONNRESET;
            n = -1;
        } else {
            n = send(socket_fd_, data, remaining, kSendFlags);
        }
#endif
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                engine->register_socket_event(
                    socket_fd_, static_cast<int>(net::IOEvent::WRITE), id());
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR_STREAM("发送 CONNECT 失败: "
                                    << sock_err_str(sock_errno()));
            notify_segment_failure(engine, "Failed to send CONNECT to proxy");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        proxy_sent_ += static_cast<std::size_t>(n);
    }

    // CONNECT 已完整发出：等代理最终应答（非阻塞 socket 上应答不可
    // 能已在缓冲——请求刚写出）
    connection_state_ = HttpConnectionState::PROXY_TUNNEL_RECV;
    engine->register_socket_event(
        socket_fd_, static_cast<int>(net::IOEvent::READ), id());
    return ExecutionResult::WAIT_FOR_SOCKET;
}

AbstractCommand::ExecutionResult
HttpInitiateConnectionCommand::receive_proxy_connect_response(
    DownloadEngineV2* engine) {
    char buf[4096];
    while (proxy_response_.find("\r\n\r\n") == std::string::npos &&
           proxy_response_.size() < 64 * 1024) {
#ifdef _WIN32
        const ssize_t n = recv(socket_fd_, buf, static_cast<int>(sizeof(buf)), 0);
#else
        const ssize_t n = recv(socket_fd_, buf, sizeof(buf), 0);
#endif
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                engine->register_socket_event(
                    socket_fd_, static_cast<int>(net::IOEvent::READ), id());
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR_STREAM("接收代理应答失败: "
                                    << sock_err_str(sock_errno()));
            notify_segment_failure(engine, "Failed to receive proxy response");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (n == 0) {
            FALCON_LOG_ERROR_STREAM("代理在应答完成前关闭连接");
            notify_segment_failure(engine, "Proxy closed connection before response");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        proxy_response_.append(buf, static_cast<std::size_t>(n));
    }

    // 状态行判定：HTTP/1.x 2xx 即隧道建立（RFC 7231：1xx 中间应答不在
    // 最终应答之列，代理必须以 2xx 收尾）
    const auto eol = proxy_response_.find("\r\n");
    const std::string status_line =
        eol == std::string::npos ? proxy_response_
                                 : proxy_response_.substr(0, eol);
    unsigned code = 0;
    const auto sp1 = status_line.find(' ');
    if (sp1 != std::string::npos) {
        const auto sp2 = status_line.find(' ', sp1 + 1);
        const std::string code_text = status_line.substr(
            sp1 + 1,
            sp2 == std::string::npos ? std::string::npos : sp2 - sp1 - 1);
        if (code_text.size() == 3 && code_text.find_first_not_of("0123456789") ==
                                         std::string::npos) {
            code = static_cast<unsigned>((code_text[0] - '0') * 100) +
                   static_cast<unsigned>((code_text[1] - '0') * 10) +
                   static_cast<unsigned>(code_text[2] - '0');
        }
    }

    if (code < 200 || code > 299) {
        FALCON_LOG_ERROR_STREAM("代理拒绝 CONNECT 隧道: " << status_line);
        notify_segment_failure(engine, "Proxy refused CONNECT: " + status_line);
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return ExecutionResult::ERROR_OCCURRED;
    }

    FALCON_LOG_INFO_STREAM("代理隧道已建立: " << host_ << ":" << port_);
    connection_state_ = HttpConnectionState::TLS_HANDSHAKING;
    return ExecutionResult::OK;
}

bool HttpInitiateConnectionCommand::prepare_http_request() {
    http_request_ = std::make_shared<HttpRequest>();
    http_request_->set_method("GET");
    // 明文 HTTP 经代理：请求行用 absolute-form（RFC 7230 §5.3.2），代理
    // 据此转发；HTTPS 经代理已由 CONNECT 隧道承载，隧道内回 origin-form
    // （代理不见内部请求，目标服务器也不认 absolute-form）；直连为
    // origin-form
    const bool plain_via_proxy =
        proxy_cfg_.kind == HttpProxyKind::HttpProxy && !use_https_;
    http_request_->set_url(plain_via_proxy ? url_ : path_);

    http_request_->set_header("Host", host_);
    http_request_->set_header("User-Agent", options_.user_agent);
    http_request_->set_header("Accept", "*/*");
    // 不协商压缩：显式只接受 identity（wget 同语义）。下载器不变式是
    // 输出文件与 URL 响应体逐字节一致（分段拼接、断点续传、metalink
    // 哈希校验等内容寻址流程都依赖它）；协商压缩会改变落盘字节。服务
    // 器无视协商强制压缩时，响应命令按 aria2 同语义原样落盘并记录
    // 日志（parse_headers 的 content-encoding 观测点）
    http_request_->set_header("Accept-Encoding", "identity");
    http_request_->set_header("Connection", "close");

    // 代理认证只随明文代理请求发出（CONNECT 隧道的凭据已在 CONNECT
    // 请求里交给代理，隧道内再发既无意义也向目标泄漏代理凭据）
    if (plain_via_proxy && !proxy_cfg_.username.empty()) {
        http_request_->set_header(
            "Proxy-Authorization",
            "Basic " +
                base64_encode(proxy_cfg_.username + ":" + proxy_cfg_.password));
    }

    // 多连接分段：非首段连接携带 Range 请求头；断点续传时初始连接也
    // 可承载带 Range 的续传段（set_range 对段 0 同样合法）
    if (has_range_) {
        const Bytes range_end = range_offset_ + range_length_ - 1;
        http_request_->set_header(
            "Range",
            "bytes=" + std::to_string(range_offset_) + "-" + std::to_string(range_end));
        FALCON_LOG_DEBUG_STREAM("分段 " << range_segment_id_ << " 请求范围: bytes="
                                << range_offset_ << "-" << range_end);
        // If-Range 内容一致性防护：资源已变更时服务器回 200 全量，
        // 响应命令据此放弃续传（断点数据是旧内容的，不能接新内容）
        if (!if_range_.empty()) {
            http_request_->set_header("If-Range", if_range_);
        }
    } else if (!if_modified_since_.empty()) {
        // 条件下载：仅全新 GET 携带（与 Range 互斥由本分支结构性保证
        // ——304 命中即"本地文件已是最新"，无响应体，响应命令按成功
        // 收口并保留本地文件，aria2 --conditional-get 同语义）
        http_request_->set_header("If-Modified-Since", if_modified_since_);
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
            n = SSL_write(ssl_conn_.get(), data, static_cast<int>(remaining));
            if (n <= 0) {
                int ssl_error = SSL_get_error(ssl_conn_.get(), static_cast<int>(n));
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
#ifdef _WIN32
            n = send(socket_fd_, data, static_cast<int>(remaining),
                     kSendFlags);
#else
            n = send(socket_fd_, data, remaining, kSendFlags);
#endif
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
    // 注意：HTTPS 时传递 TLS 会话共享句柄（响应/下载命令经 SSL_read 解密）；
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
    response_cmd->set_redirect_depth(redirect_depth_);
    if (segment_retry_) {
        response_cmd->set_segment_retry_routing(true);
    }
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
        // 分段连接：先试段级换源重试（预算内换下一镜像），预算耗尽
        // 才走既有失败收口（多连接组由段失败聚合终态）
        auto* man = engine->request_group_man();
        auto* group = man ? man->find_group(get_task_id()) : nullptr;
        if (!group) {
            return;
        }
        // 暂停/移除竞态守卫：控制入口已改组状态时不得把 Paused 改写
        // 成 Failed（参照 fail_group_of_command 的状态守卫）
        const auto st = group->status();
        if (st == RequestGroupStatus::PAUSED ||
            st == RequestGroupStatus::REMOVED) {
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return;
        }
        if (HttpSegmentRetryCommand::schedule_retry(
                engine, get_task_id(), range_segment_id_,
                range_offset_, range_length_, url_)) {
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return;
        }
        group->finish_segment(false);
        group->save_resume_now();  // 固化各段断点（未追踪时空操作）
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
    if (auto retry_cmd = make_connection_retry(engine, get_task_id(), url_,
                                               options_, retry_count_)) {
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
        // 换源；暂停恢复的初始连接不在此列（走连接级重试保留恢复语义）
        const bool in_segment_context =
            segment_id_ > 0 || segment_retry_routing_;
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
    if (redirect_url.rfind("https://", 0) == 0) {
        // V2 socket 链路尚未放行 TLS（M1.1），明确报错而非静默失败
        FALCON_LOG_WARN_STREAM("重定向目标为 https（V2 暂不支持），"
                              "按失败收口: " << redirect_url);
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
        response_ok = content_length_ == plan.total;
    }

    if (!response_ok) {
        FALCON_LOG_INFO_STREAM("续传响应不一致（status=" << status_code_
                              << ", length=" << content_length_
                              << ", 请求起点=" << range_offset_
                              << ", 请求长度=" << range_length_
                              << ", content-range=" << headers_["content-range"]
                              << "），放弃续传: task=" << get_task_id());
        group.abandon_resume();
        // 同进程 pause→resume 后组保持 multi_segment 状态：abandon 后
        // 重启的全新下载若再次分段，残留段计数会让完成判定错乱
        if (group.is_multi_segment()) {
            group.reset_multi_segment_tracking();
        }
        // 本连接的响应体起点与全新请求不符，不能复用——重新发起无
        // Range 的全新下载
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
    std::error_code rename_ec;
    std::filesystem::rename(write_path_, task->output_path(), rename_ec);
    if (rename_ec) {
        task->set_error("下载完成但发布失败（临时文件改名 " + write_path_ +
                        " → " + task->output_path() + "）: " +
                        rename_ec.message());
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
                        engine->register_socket_event(
                            socket_fd_,
                            static_cast<int>(ssl_error == SSL_ERROR_WANT_WRITE
                                                 ? net::IOEvent::WRITE
                                                 : net::IOEvent::READ),
                            id());
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
            apply_group_resume_range(*next_cmd, *group);
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
