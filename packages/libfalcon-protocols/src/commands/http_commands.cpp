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
using falcon::protocols::detail::fail_group_terminal;
using falcon::protocols::detail::base64_encode;

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

/// 判断文本是否为指定地址族的 IP 字面量（IPv6 字面量在 URL 中带括号，
/// 此处收剥括号后的裸地址）
bool is_ip_literal(int family, const std::string& text) {
    if (family == AF_INET6) {
        in6_addr v6 = {};
        return inet_pton(AF_INET6, text.c_str(), &v6) == 1;
    }
    in_addr v4 = {};
    return inet_pton(AF_INET, text.c_str(), &v4) == 1;
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

/// socks5 CONNECT 请求的目标地址块（ATYP + ADDR + 大端 PORT）：
/// IPv4/IPv6 字面量走 ATYP 0x01/0x04；其余按域名 ATYP 0x03（socks5h
/// 语义：目标主机名交由代理解析）。域名超 255 字节（长度域单字节）
/// 返回 false
bool append_socks_address(std::string& out, const std::string& host,
                          uint16_t port) {
    in_addr v4 = {};
    in6_addr v6 = {};
    if (inet_pton(AF_INET, host.c_str(), &v4) == 1) {
        out += '\x01';
        out.append(reinterpret_cast<const char*>(&v4), sizeof(v4));
    } else if (inet_pton(AF_INET6, host.c_str(), &v6) == 1) {
        out += '\x04';
        out.append(reinterpret_cast<const char*>(&v6), sizeof(v6));
    } else {
        if (host.empty() || host.size() > 255) {
            return false;
        }
        out += '\x03';
        out += static_cast<char>(static_cast<unsigned char>(host.size()));
        out += host;
    }
    out += static_cast<char>((port >> 8) & 0xffu);
    out += static_cast<char>(port & 0xffu);
    return true;
}

/// CONNECT 应答 ATYP 合法性（收满 4 字节后可判定；不足 4 字节不算
/// 非法，交由收齐判定继续等待）
bool socks_atyp_valid(const std::string& data) {
    if (data.size() < 4) {
        return true;
    }
    const auto atyp = static_cast<unsigned char>(data[3]);
    return atyp == 0x01 || atyp == 0x03 || atyp == 0x04;
}

/// CONNECT 应答收齐所需总长度（4 + 地址 + 2；域名 ATYP 需先收满 5
/// 字节读出地址长度）。返回 0 = 还需更多字节
std::size_t socks_connect_reply_len(const std::string& data) {
    if (data.size() < 4) {
        return 0;
    }
    const auto atyp = static_cast<unsigned char>(data[3]);
    switch (atyp) {
        case 0x01:
            return 4 + 4 + 2;
        case 0x04:
            return 4 + 16 + 2;
        case 0x03:
            if (data.size() < 5) {
                return 0;
            }
            return 5 + static_cast<std::size_t>(
                            static_cast<unsigned char>(data[4])) +
                   2;
        default:
            return 0;  // 非法 ATYP（socks_atyp_valid 先行拦截）
    }
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
    bool is_socks5 = false;

    if (rest.rfind("http://", 0) == 0) {
        rest = rest.substr(7);
    } else if (rest.rfind("socks5h://", 0) == 0 || rest.rfind("socks5://", 0) == 0) {
        // socks5 原生数据面（socks5h 语义：目标主机名交代理解析）；
        // socks4 仍 Unsupported 回退 V1 curl
        is_socks5 = true;
        default_port = 1080;
        rest = rest.substr(rest.rfind("socks5h://", 0) == 0 ? 10 : 9);
    } else if (rest.rfind("https://", 0) == 0 || rest.rfind("socks", 0) == 0) {
        // TLS 代理与 socks4 不支持——适配层回退 V1 curl
        return unsupported();
    } else if (rest.find("://") != std::string::npos) {
        return unsupported();  // 未知 scheme
    }  // 无 scheme：按明文 HTTP 代理（curl 对无 scheme 代理同语义）

    // proxy_type 显式声明的兜底（字段语义：覆盖 URL scheme）：
    // socks5 → 原生 socks5 数据面；其余 socks 变体（socks4a/socks4）回退
    const std::string proxy_type_lc = ascii_lower(options.proxy_type);
    if (proxy_type_lc.find("socks5") != std::string::npos) {
        is_socks5 = true;
        default_port = 1080;
    } else if (proxy_type_lc.find("socks") != std::string::npos) {
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
    cfg.kind = is_socks5 ? HttpProxyKind::Socks5 : HttpProxyKind::HttpProxy;
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
    // 解析 URL（最小实现：http://host[:port]/path?query）。IPv6 字面量
    // 按 RFC 3986 §3.2.2 为 [...] 括号包裹（括号内有 ':'，端口在 ] 之后
    // ——对括号形态直接 rfind(':') 会把 host 切成 "["、端口解析成错值）
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

    if (!authority.empty() && authority[0] == '[') {
        is_ipv6_literal_ = true;
        const auto close = authority.find(']');
        if (close == std::string::npos) {
            // 畸形（无闭合 ]）：host 剥去 '[' 后在连接层按不可解析收口
            host_ = authority.substr(1);
        } else {
            host_ = authority.substr(1, close - 1);
            if (close + 1 < authority.size() && authority[close + 1] == ':') {
                port_ = static_cast<uint16_t>(
                    std::stoi(authority.substr(close + 2)));
            }
        }
    } else {
        const auto port_pos = authority.rfind(':');
        if (port_pos != std::string::npos && port_pos + 1 < authority.size()) {
            host_ = authority.substr(0, port_pos);
            port_ = static_cast<uint16_t>(std::stoi(authority.substr(port_pos + 1)));
        } else {
            host_ = authority;
        }
    }

    connection_state_ = HttpConnectionState::DISCONNECTED;

    // 代理配置 parse 一次（Unsupported 的拒绝收口在 request_group 门禁；
    // 此处 None 之外均为已验证的 HttpProxy）
    proxy_cfg_ = parse_http_proxy(options);
    if (proxy_cfg_.kind == HttpProxyKind::HttpProxy) {
        FALCON_LOG_INFO_STREAM("使用 HTTP 代理: " << proxy_cfg_.host << ":"
                                                  << proxy_cfg_.port);
    } else if (proxy_cfg_.kind == HttpProxyKind::Socks5) {
        FALCON_LOG_INFO_STREAM("使用 socks5 代理: " << proxy_cfg_.host << ":"
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
                // 步骤 1: 解析连接端点（字面量家族判定或 DNS 解析；失败
                // 与连接失败同收口——重试链重建连接时再次解析）
                if (!resolve_connect_endpoint()) {
                    FALCON_LOG_ERROR_STREAM("连接失败: " << host_);
                    notify_segment_failure(engine, "Failed to connect: " + host_);
                    return handle_result(ExecutionResult::ERROR_OCCURRED);
                }

                // 步骤 2: 创建 Socket（按端点地址族——IPv6 目标此前恒以
                // AF_INET 创建，连接必败）
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
                    // 注册失败（fd 上限/ENOMEM 等）不能挂起等永远不会来
                    // 的事件——立即按连接失败收口
                    if (!engine->register_socket_event(
                            socket_fd_, static_cast<int>(net::IOEvent::WRITE),
                            id())) {
                        notify_segment_failure(engine,
                                               "Failed to register connect event");
                        return handle_result(ExecutionResult::ERROR_OCCURRED);
                    }
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
                    // HTTPS 经代理：先建 CONNECT 隧道（或 socks5 握
                    // 手）再 TLS 握手（隧道内字节端到端加密，代理
                    // 只见 host:port）；直连直接进 TLS 握手
                    if (proxy_cfg_.kind == HttpProxyKind::HttpProxy) {
                        connection_state_ =
                            HttpConnectionState::PROXY_TUNNEL_SEND;
                    } else if (proxy_cfg_.kind == HttpProxyKind::Socks5) {
                        connection_state_ =
                            HttpConnectionState::SOCKS_HANDSHAKE_SEND;
                    } else {
                        connection_state_ =
                            HttpConnectionState::TLS_HANDSHAKING;
                    }
                } else if (proxy_cfg_.kind == HttpProxyKind::Socks5) {
                    // socks5 明文目标同样先握手：隧道内发 origin-form
                    // 请求（代理负责解析主机名，见 CONNECT 请求 ATYP）
                    connection_state_ =
                        HttpConnectionState::SOCKS_HANDSHAKE_SEND;
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

            case HttpConnectionState::SOCKS_HANDSHAKE_SEND:
                if (connection_state_ == HttpConnectionState::SOCKS_HANDSHAKE_SEND) {
                    // 发送 socks5 握手报文（greeting / RFC 1929 子协商 /
                    // CONNECT）：未发完注册 WRITE 挂起，发完置
                    // SOCKS_HANDSHAKE_RECV 注册 READ 挂起等应答；阶段
                    // 转换由 socks_stage_ 记账（收到子协商应答后置回
                    // SOCKS_HANDSHAKE_SEND）
                    return handle_result(send_socks_handshake(engine));
                }
                [[fallthrough]];

            case HttpConnectionState::SOCKS_HANDSHAKE_RECV:
                if (connection_state_ == HttpConnectionState::SOCKS_HANDSHAKE_RECV) {
                    const auto socks_res = receive_socks_handshake_reply(engine);
                    if (socks_res != ExecutionResult::OK) {
                        return handle_result(socks_res);
                    }
                    // 收到方法选择 0x02 时子协商需再发报文——回 SOCKS_
                    // HANDSHAKE_SEND case 发送（不落下方请求发送是
                    // 误发）；其余路径（CONNECT 应答）已置 TLS_
                    // HANDSHAKING 或 CONNECTED 落下方同轮推进
                    if (connection_state_ == HttpConnectionState::SOCKS_HANDSHAKE_SEND) {
                        return handle_result(send_socks_handshake(engine));
                    }
                }
                [[fallthrough]];

            case HttpConnectionState::CONNECTED:
                // 请求发送 WANT_WRITE 挂起后的重入：连接/隧道/握手均
                // 已完成且状态停在 CONNECTED（初始进入时 CONNECTING 置
                // CONNECTED 后径直落到下方请求发送；socks5 明文握手完
                // 成也置 CONNECTED），重入必须直接续推请求发送——落到
                // default 按失败收口是假失败（真实网络拥塞下内核发送
                // 缓冲满即可触发）
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
    std::string& ip,
    int* family)
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
    if (family) {
        *family = chosen->ai_family;
    }
    freeaddrinfo(result);

    FALCON_LOG_INFO_STREAM(host << " 解析为 " << ip);
    return true;
}

bool HttpInitiateConnectionCommand::resolve_connect_endpoint() {
    // 代理生效时（明文 HTTP 代理或 socks5）连接对象是代理主机——两者均
    // 不本地解析目标主机名（明文代理经 absolute-form 把目标交给代理解析，
    // socks5 经 CONNECT ATYP 0x03 交给代理解析）；v6 字面量代理已被
    // parse_http_proxy 拒绝，代理主机名可解析出 v6，家族判定同路径
    const bool use_proxy = proxy_cfg_.kind == HttpProxyKind::HttpProxy ||
                           proxy_cfg_.kind == HttpProxyKind::Socks5;
    const std::string& connect_host = use_proxy ? proxy_cfg_.host : host_;

    // 字面量快路径（IPv6 字面量 host_ 已剥括号，两者同为裸地址判定）
    in_addr v4 = {};
    if (inet_pton(AF_INET, connect_host.c_str(), &v4) == 1) {
        connect_family_ = AF_INET;
        return true;
    }
    in6_addr v6 = {};
    if (inet_pton(AF_INET6, connect_host.c_str(), &v6) == 1) {
        connect_family_ = AF_INET6;
        return true;
    }

    // 主机名解析（resolved_ip_ 跨连接级重试缓存，家族随缓存）
    if (resolved_ip_.empty()) {
        std::string ip;
        int family = AF_INET;
        if (!resolve_host(connect_host, ip, &family)) {
            FALCON_LOG_ERROR_STREAM("解析主机失败: " << connect_host);
            return false;
        }
        resolved_ip_ = ip;
        resolved_family_ = family;
    }
    connect_family_ = resolved_family_;
    return true;
}

bool HttpInitiateConnectionCommand::create_socket() {
    socket_fd_ = ::falcon::detail::inject_failure(
                     ::falcon::detail::InjectPoint::HttpSocketCreate)
                     ? -1
                     : socket(connect_family_, SOCK_STREAM, 0);
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
    // 代理生效时（明文 HTTP 代理或 socks5）连接代理服务器（absolute-form
    // 请求、CONNECT 隧道或 socks5 握手均在代理连接上承载）；目标主机名
    // 的解析延迟到隧道建立之后（TLS 握手也只与目标主机相关）。端点解析
    // 已在 resolve_connect_endpoint 完成（execute 的连接步骤 1），此处
    // 只按家族组装地址并 connect
    const bool use_proxy = proxy_cfg_.kind == HttpProxyKind::HttpProxy ||
                           proxy_cfg_.kind == HttpProxyKind::Socks5;
    const std::string& connect_host = use_proxy ? proxy_cfg_.host : host_;
    const uint16_t connect_port = use_proxy ? proxy_cfg_.port : port_;

    // 连接地址：字面量直接用，主机名用解析缓存
    std::string addr_text = connect_host;
    if (!is_ip_literal(connect_family_, addr_text) && !resolved_ip_.empty()) {
        addr_text = resolved_ip_;
    }

    struct sockaddr_storage addr_ss = {};
    socklen_t addr_len = 0;
    if (connect_family_ == AF_INET6) {
        auto* addr6 = reinterpret_cast<struct sockaddr_in6*>(&addr_ss);
        addr6->sin6_family = AF_INET6;
        addr6->sin6_port = htons(connect_port);
        if (inet_pton(AF_INET6, addr_text.c_str(), &addr6->sin6_addr) <= 0) {
            FALCON_LOG_ERROR_STREAM("inet_pton 失败: " << addr_text);
            return false;
        }
        addr_len = sizeof(*addr6);
    } else {
        auto* addr4 = reinterpret_cast<struct sockaddr_in*>(&addr_ss);
        addr4->sin_family = AF_INET;
        addr4->sin_port = htons(connect_port);
        if (inet_pton(AF_INET, addr_text.c_str(), &addr4->sin_addr) <= 0) {
            FALCON_LOG_ERROR_STREAM("inet_pton 失败: " << addr_text);
            return false;
        }
        addr_len = sizeof(*addr4);
    }

    connect_in_progress_ = false;

    // 回环上非阻塞 connect 恒报 in-progress（失败在 getsockopt 阶段
    // 才暴露），立即硬失败本地不可构造——注入点确定性命中收口分支
    int ret = ::falcon::detail::inject_failure(
                  ::falcon::detail::InjectPoint::HttpConnectHardFail)
                  ? -1
                  : connect(socket_fd_,
                            reinterpret_cast<struct sockaddr*>(&addr_ss),
                            addr_len);
    if (ret < 0) {
        const int err = ::falcon::detail::inject_failure(
                            ::falcon::detail::InjectPoint::HttpConnectHardFail)
                            ? ENETUNREACH
                            : sock_errno();
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
            const SSL_METHOD* method =
                ::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::TlsMethodFail)
                    ? nullptr
                    : TLS_client_method();
            if (!method) {
                FALCON_LOG_ERROR_STREAM("无法获取 TLS 方法: " << ERR_error_string(ERR_get_error(), nullptr));
                return TlsHandshakeResult::FAILED;
            }

            ssl_ctx_ = ::falcon::detail::inject_failure(
                           ::falcon::detail::InjectPoint::TlsCtxNewFail)
                           ? nullptr
                           : SSL_CTX_new(method);
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

            // 双向 TLS（mTLS）：客户端证书 + 私钥（PEM，aria2
            // --certificate/--private-key 同语义）。任一加载失败都在
            // 握手前失败收口——配置错误绝不退化成匿名连接；单给其一
            // 时 check_private_key 失败同样收口（两字段可指向同一
            // 复合 PEM 文件）
            if (!options_.client_certificate.empty() ||
                !options_.client_private_key.empty()) {
                const bool loaded =
                    (!options_.client_certificate.empty() &&
                     SSL_CTX_use_certificate_chain_file(
                         ssl_ctx_, options_.client_certificate.c_str()) == 1) &&
                    (!options_.client_private_key.empty() &&
                     SSL_CTX_use_PrivateKey_file(
                         ssl_ctx_, options_.client_private_key.c_str(),
                         SSL_FILETYPE_PEM) == 1) &&
                    SSL_CTX_check_private_key(ssl_ctx_) == 1;
                if (!loaded) {
                    FALCON_LOG_ERROR_STREAM(
                        "客户端证书加载失败: "
                        << ERR_error_string(ERR_get_error(), nullptr));
                    ERR_clear_error();
                    return TlsHandshakeResult::FAILED;
                }
            }

            // 加载默认证书
            if (!SSL_CTX_set_default_verify_paths(ssl_ctx_)) {
                FALCON_LOG_WARN_STREAM("无法加载默认 CA 证书，继续使用系统证书");
            }
        }

        // 创建 SSL 连接（共享句柄：命令链上的响应/下载命令继续使用）
        ssl_conn_ = HttpTlsSessionPtr(
            ::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::TlsSslNewFail)
                ? nullptr
                : SSL_new(ssl_ctx_),
            HttpTlsSessionDeleter{});
        if (!ssl_conn_) {
            FALCON_LOG_ERROR_STREAM("无法创建 SSL 连接: " << ERR_error_string(ERR_get_error(), nullptr));
            return TlsHandshakeResult::FAILED;
        }

        // 绑定 Socket 到 SSL
        if (::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::TlsSetFdFail) ||
            SSL_set_fd(ssl_conn_.get(), static_cast<int>(socket_fd_)) != 1) {
            FALCON_LOG_ERROR_STREAM("无法绑定 Socket 到 SSL: " << ERR_error_string(ERR_get_error(), nullptr));
            return TlsHandshakeResult::FAILED;
        }

        // IP 直连（v4/v6 字面量）与 DNS 主机名在此分流：SNI 与证书
        // 校验的绑定 API 都不一样（见下）
        const bool ip_target =
            is_ip_literal(is_ipv6_literal_ ? AF_INET6 : AF_INET, host_);

        // SNI：RFC 6066 规定 HostName 不得是 IP 字面量（服务器应忽略
        // 该扩展），curl 同语义——IP 直连不发 SNI；DNS 主机名照发
        if (!ip_target) {
            SSL_set_tlsext_host_name(ssl_conn_.get(), host_.c_str());
        }

        // 证书校验绑定期望主机名：verify 开启时 SSL_get_verify_result
        // 的结论因此同时覆盖证书链与主机名——替代此前的 WARN-only
        // 主机名检查（告警后照常收数据，校验形同虚设）。
        // SSL_set1_host 只做 DNS 匹配——IP 字面量实测恒 hostname
        // mismatch（X509_check_host 不比对 IP SAN），IP 直连须走
        // X509_VERIFY_PARAM_set1_ip_asc 的 IP SAN 匹配路径
        if (options_.verify_ssl) {
            const bool bound_ok =
                ::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::TlsSetHostFail)
                    ? false
                    : (ip_target
                           ? X509_VERIFY_PARAM_set1_ip_asc(
                                 SSL_get0_param(ssl_conn_.get()),
                                 host_.c_str()) == 1
                           : SSL_set1_host(ssl_conn_.get(), host_.c_str()) == 1);
            if (!bound_ok) {
                FALCON_LOG_ERROR_STREAM("无法设置证书主机名校验: " << host_);
                return TlsHandshakeResult::FAILED;
            }
        }

        FALCON_LOG_INFO_STREAM("开始 TLS 握手: " << host_);
        tls_started_ = true;
    }

    // 执行/继续 TLS 握手（非阻塞 socket：WANT_* 属进行中而非失败）
    const int ret = SSL_connect(ssl_conn_.get());
    if (ret != 1) {
        const int error =
            ::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::TlsHandshakeWantWrite)
                ? SSL_ERROR_WANT_WRITE
                : SSL_get_error(ssl_conn_.get(), ret);

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
        // 注册失败无法等待重入，跳出 switch 落到下方握手失败收口
        if (engine->register_socket_event(
                socket_fd_, static_cast<int>(net::IOEvent::READ), id())) {
            return ExecutionResult::WAIT_FOR_SOCKET;
        }
        break;
    case TlsHandshakeResult::WANT_WRITE:
        connection_state_ = HttpConnectionState::TLS_HANDSHAKING;
        if (engine->register_socket_event(
                socket_fd_, static_cast<int>(net::IOEvent::WRITE), id())) {
            return ExecutionResult::WAIT_FOR_SOCKET;
        }
        break;
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
        proxy_request_ = "CONNECT " + host_authority() + ":" + std::to_string(port_) +
                         " HTTP/1.1\r\n"
                         "Host: " + host_authority() + ":" + std::to_string(port_) + "\r\n";
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
                ::falcon::detail::InjectPoint::HttpSendWantWrite)) {
            // 模拟内核发送缓冲满：置 WOULDBLOCK 走 WANT_WRITE 重入
            WSASetLastError(WSAEWOULDBLOCK);
            n = -1;
        } else if (::falcon::detail::inject_failure(
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
                ::falcon::detail::InjectPoint::HttpSendWantWrite)) {
            // 模拟内核发送缓冲满：置 WOULDBLOCK 走 WANT_WRITE 重入
            errno = EAGAIN;
            n = -1;
        } else if (::falcon::detail::inject_failure(
                       ::falcon::detail::InjectPoint::ProxyConnectSendFail)) {
            errno = ECONNRESET;
            n = -1;
        } else {
            n = send(socket_fd_, data, remaining, kSendFlags);
        }
#endif
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                if (!engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::WRITE),
                        id())) {
                    notify_segment_failure(
                        engine, "Failed to register proxy connect send event");
                    close_socket_fd(socket_fd_);
                    socket_fd_ = -1;
                    return ExecutionResult::ERROR_OCCURRED;
                }
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
    if (!engine->register_socket_event(
            socket_fd_, static_cast<int>(net::IOEvent::READ), id())) {
        notify_segment_failure(engine,
                               "Failed to register proxy response event");
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return ExecutionResult::ERROR_OCCURRED;
    }
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
                if (!engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::READ),
                        id())) {
                    notify_segment_failure(
                        engine, "Failed to register proxy response event");
                    close_socket_fd(socket_fd_);
                    socket_fd_ = -1;
                    return ExecutionResult::ERROR_OCCURRED;
                }
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

AbstractCommand::ExecutionResult
HttpInitiateConnectionCommand::send_socks_handshake(
    DownloadEngineV2* engine) {
    // 按当前 socks 阶段构造握手报文（每阶段首次进入构造一次，阶段
    // 转换时由调用方复位 proxy_request_/proxy_sent_/proxy_response_）
    if (proxy_request_.empty()) {
        const bool has_credentials =
            !proxy_cfg_.username.empty() || !proxy_cfg_.password.empty();
        if (socks_stage_ == SocksStage::Greeting) {
            // 方法协商：VER=5 + NMETHODS + METHODS；有凭据才声明
            // 0x02（用户名密码）——绝不声明无法执行的认证方法
            proxy_request_ += '\x05';
            proxy_request_ += has_credentials ? '\x02' : '\x01';
            proxy_request_ += '\x00';  // 无认证
            if (has_credentials) {
                proxy_request_ += '\x02';  // 用户名/密码（RFC 1929）
            }
        } else if (socks_stage_ == SocksStage::Auth) {
            // RFC 1929 子协商：VER=1 + ULEN + UNAME + PLEN + PASSWD；
            // 用户名/密码各限 255 字节（单字节长度域）——超长在发送前收口
            if (proxy_cfg_.username.size() > 255 ||
                proxy_cfg_.password.size() > 255) {
                FALCON_LOG_ERROR_STREAM(
                    "socks5 代理用户名或密码超过 255 字节，无法编码");
                notify_segment_failure(
                    engine,
                    "socks5 credentials exceed 255 bytes");
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                return ExecutionResult::ERROR_OCCURRED;
            }
            proxy_request_ += '\x01';
            proxy_request_ += static_cast<char>(
                static_cast<unsigned char>(proxy_cfg_.username.size()));
            proxy_request_ += proxy_cfg_.username;
            proxy_request_ += static_cast<char>(
                static_cast<unsigned char>(proxy_cfg_.password.size()));
            proxy_request_ += proxy_cfg_.password;
        } else {
            // CONNECT 请求：VER=5 + CMD=0x01(connect) + RSV + ATYP + 目标
            // 地址 + 端口。目标 = host_/port_；域名按 socks5h 语义交给
            // 代理解析（ATYP 0x03）；v6 字面量（无括号）走 0x04；
            // 其余按 0x03
            // 先编码地址体（失败时 proxy_request_ 仍为空），再加三字节头
            std::string address_body;
            if (!append_socks_address(address_body, host_, port_)) {
                FALCON_LOG_ERROR_STREAM(
                    "socks5 CONNECT 目标地址编码失败: " << host_);
                notify_segment_failure(engine, "Invalid socks5 target address");
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                return ExecutionResult::ERROR_OCCURRED;
            }
            // assign 显式长度——字面量含 \x00，operator=(const char*) 会在 NUL 处截断
            proxy_request_.assign("\x05\x01\x00", 3);
            proxy_request_ += address_body;
        }
        proxy_sent_ = 0;
    }

    while (proxy_sent_ < proxy_request_.size()) {
        const char* data = proxy_request_.data() + proxy_sent_;
        const std::size_t remaining = proxy_request_.size() - proxy_sent_;
#ifdef _WIN32
        ssize_t n;
        if (::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::HttpSendWantWrite)) {
            // 模拟内核发送缓冲满：置 WOULDBLOCK 走 WANT_WRITE 重入
            WSASetLastError(WSAEWOULDBLOCK);
            n = -1;
        } else if (::falcon::detail::inject_failure(
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
                ::falcon::detail::InjectPoint::HttpSendWantWrite)) {
            // 模拟内核发送缓冲满：置 WOULDBLOCK 走 WANT_WRITE 重入
            errno = EAGAIN;
            n = -1;
        } else if (::falcon::detail::inject_failure(
                       ::falcon::detail::InjectPoint::ProxyConnectSendFail)) {
            errno = ECONNRESET;
            n = -1;
        } else {
            n = send(socket_fd_, data, remaining, kSendFlags);
        }
#endif
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                if (!engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::WRITE),
                        id())) {
                    notify_segment_failure(
                        engine, "Failed to register socks handshake event");
                    close_socket_fd(socket_fd_);
                    socket_fd_ = -1;
                    return ExecutionResult::ERROR_OCCURRED;
                }
                return ExecutionResult::WAIT_FOR_SOCKET;
            }
            FALCON_LOG_ERROR_STREAM("发送 socks5 握手报文失败: "
                                    << sock_err_str(sock_errno()));
            notify_segment_failure(engine, "Failed to send socks5 handshake");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        proxy_sent_ += static_cast<std::size_t>(n);
    }

    proxy_response_.clear();
    connection_state_ = HttpConnectionState::SOCKS_HANDSHAKE_RECV;
    if (!engine->register_socket_event(
            socket_fd_, static_cast<int>(net::IOEvent::READ), id())) {
        FALCON_LOG_ERROR_STREAM("socks5 握手后注册读事件失败");
        notify_segment_failure(engine, "Failed to register socks read event");
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return ExecutionResult::ERROR_OCCURRED;
    }
    return ExecutionResult::WAIT_FOR_SOCKET;
}

AbstractCommand::ExecutionResult
HttpInitiateConnectionCommand::receive_socks_handshake_reply(
    DownloadEngineV2* engine) {
    // 按当前阶段判定收齐：方法协商/子协商应答恰 2 字节；CONNECT 应答
    // = 4 头部 + 地址（ATYP 0x03 需先收满 5 字节读出地址长度）+ 2 端口
    auto stage_complete = [this]() -> bool {
        if (socks_stage_ == SocksStage::Connect) {
            if (proxy_response_.size() < 4) {
                return false;
            }
            if (!socks_atyp_valid(proxy_response_)) {
                return true;  // 非法 ATYP——收齐头部即按失败收口
            }
            const std::size_t total = socks_connect_reply_len(proxy_response_);
            return total > 0 && proxy_response_.size() >= total;
        }
        return proxy_response_.size() >= 2;
    };

    char buf[4096];
    while (!stage_complete() && proxy_response_.size() < 64 * 1024) {
#ifdef _WIN32
        const ssize_t n = recv(socket_fd_, buf,
                               static_cast<int>(sizeof(buf)), 0);
#else
        const ssize_t n = recv(socket_fd_, buf, sizeof(buf), 0);
#endif
        if (n < 0) {
            if (sock_would_block(sock_errno())) {
                if (engine->register_socket_event(
                        socket_fd_, static_cast<int>(net::IOEvent::READ),
                        id())) {
                    return ExecutionResult::WAIT_FOR_SOCKET;
                }
                FALCON_LOG_ERROR_STREAM("socks5 应答挂起后注册读事件失败");
                notify_segment_failure(
                    engine, "Failed to register socks read event");
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                return ExecutionResult::ERROR_OCCURRED;
            }
            FALCON_LOG_ERROR_STREAM("接收 socks5 应答失败: "
                                    << sock_err_str(sock_errno()));
            notify_segment_failure(engine, "Failed to receive socks5 reply");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (n == 0) {
            FALCON_LOG_ERROR_STREAM("socks5 代理在应答完成前关闭连接");
            notify_segment_failure(
                engine, "socks5 proxy closed connection before reply");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        proxy_response_.append(buf, static_cast<std::size_t>(n));
    }

    if (socks_stage_ == SocksStage::Greeting) {
        const auto ver = static_cast<unsigned char>(proxy_response_[0]);
        const auto method =
            static_cast<unsigned char>(proxy_response_[1]);
        if (ver != 0x05) {
            FALCON_LOG_ERROR_STREAM("socks5 方法协商应答版本错误: "
                                    << ver);
            notify_segment_failure(engine,
                                   "socks5 greeting reply has wrong version");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (method == 0xFF) {
            FALCON_LOG_ERROR_STREAM("socks5 代理不接受任何认证方法");
            notify_segment_failure(
                engine, "socks5 proxy accepts no auth method");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (method == 0x02) {
            if (proxy_cfg_.username.empty() && proxy_cfg_.password.empty()) {
                // 代理要求认证但客户端无凭据——握手报文未声明 0x02，
                // 属于代理越权，按失败收口
                FALCON_LOG_ERROR_STREAM("socks5 代理要求用户名密码认证但无凭据");
                notify_segment_failure(
                    engine,
                    "socks5 proxy demands auth but no credentials");
                close_socket_fd(socket_fd_);
                socket_fd_ = -1;
                return ExecutionResult::ERROR_OCCURRED;
            }
            socks_stage_ = SocksStage::Auth;
            connection_state_ = HttpConnectionState::SOCKS_HANDSHAKE_SEND;
            proxy_request_.clear();
            proxy_sent_ = 0;
            proxy_response_.clear();
            return send_socks_handshake(engine);
        }
        if (method != 0x00) {
            FALCON_LOG_ERROR_STREAM("socks5 代理选择了不支持的认证方法: "
                                    << static_cast<unsigned>(method));
            notify_segment_failure(
                engine, "socks5 proxy selected unsupported auth method");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        socks_stage_ = SocksStage::Connect;
        connection_state_ = HttpConnectionState::SOCKS_HANDSHAKE_SEND;
        proxy_request_.clear();
        proxy_sent_ = 0;
        proxy_response_.clear();
        return send_socks_handshake(engine);
    }

    if (socks_stage_ == SocksStage::Auth) {
        const auto ver = static_cast<unsigned char>(proxy_response_[0]);
        const auto status =
            static_cast<unsigned char>(proxy_response_[1]);
        if (ver != 0x01) {
            FALCON_LOG_ERROR_STREAM("socks5 认证应答版本错误: " << ver);
            notify_segment_failure(engine,
                                   "socks5 auth reply has wrong version");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (status != 0x00) {
            FALCON_LOG_ERROR_STREAM("socks5 用户名密码认证失败: status="
                                    << static_cast<unsigned>(status));
            notify_segment_failure(engine, "socks5 auth failed");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        socks_stage_ = SocksStage::Connect;
        connection_state_ = HttpConnectionState::SOCKS_HANDSHAKE_SEND;
        proxy_request_.clear();
        proxy_sent_ = 0;
        proxy_response_.clear();
        return send_socks_handshake(engine);
    }

    // CONNECT 应答：VER REP RSV ATYP BND.ADDR BND.PORT
    if (proxy_response_.size() < 2) {
        FALCON_LOG_ERROR_STREAM("socks5 CONNECT 应答为空或截断");
        notify_segment_failure(engine, "socks5 CONNECT reply truncated");
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return ExecutionResult::ERROR_OCCURRED;
    }
    // stage_complete 会对非法 ATYP 提前放行等待循环，64KB 上限也可在
    // 应答收齐前退出——两者都不是完整合法应答，绝不进入成功收口
    if (!socks_atyp_valid(proxy_response_) ||
        proxy_response_.size() <
            socks_connect_reply_len(proxy_response_)) {
        FALCON_LOG_ERROR_STREAM("socks5 CONNECT 应答地址块非法或截断");
        notify_segment_failure(engine, "socks5 CONNECT reply truncated");
        close_socket_fd(socket_fd_);
        socket_fd_ = -1;
        return ExecutionResult::ERROR_OCCURRED;
    }
    {
        const auto ver =
            static_cast<unsigned char>(proxy_response_[0]);
        const auto rep =
            static_cast<unsigned char>(proxy_response_[1]);
        if (ver != 0x05) {
            FALCON_LOG_ERROR_STREAM("socks5 CONNECT 应答版本错误: "
                                    << ver);
            notify_segment_failure(engine,
                                   "socks5 CONNECT reply has wrong version");
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
        if (rep != 0x00) {
            FALCON_LOG_ERROR_STREAM("socks5 代理拒绝 CONNECT: REP="
                                    << static_cast<unsigned>(rep));
            notify_segment_failure(
                engine, "socks5 proxy refused CONNECT: REP=" +
                            std::to_string(static_cast<unsigned>(rep)));
            close_socket_fd(socket_fd_);
            socket_fd_ = -1;
            return ExecutionResult::ERROR_OCCURRED;
        }
    }

    FALCON_LOG_INFO_STREAM("socks5 隧道已建立: " << host_ << ":" << port_);
    // https 目标接着 TLS 握手；明文目标回落 CONNECTED（既有状态机
    // fallthrough 到 origin-form 请求发送——socks5 隧道内目标地址
    // 已由 ATYP 字段承载，请求行保持 origin-form）
    connection_state_ =
        use_https_ ? HttpConnectionState::TLS_HANDSHAKING
                   : HttpConnectionState::CONNECTED;
    proxy_request_.clear();
    proxy_response_.clear();
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

    http_request_->set_header("Host", host_authority());
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
            // 使用 SSL_write 发送 HTTPS 数据（注入时未发出任何字节，
            // SSL_get_error 的前置条件"真实失败操作"不成立，故错误码
            // 一并注入：WantWrite 模拟缓冲满走重入，Fail 取 SYSCALL）
            const bool write_want_write =
                ::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::TlsRequestWriteWantWrite);
            const bool write_injected =
                write_want_write ||
                ::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::TlsRequestWriteFail);
            n = write_injected
                    ? -1
                    : SSL_write(ssl_conn_.get(), data,
                                static_cast<int>(remaining));
            if (n <= 0) {
                int ssl_error =
                    write_want_write ? SSL_ERROR_WANT_WRITE
                    : write_injected ? SSL_ERROR_SYSCALL
                                     : SSL_get_error(ssl_conn_.get(),
                                                     static_cast<int>(n));
                if (ssl_error == SSL_ERROR_WANT_WRITE || ssl_error == SSL_ERROR_WANT_READ) {
                    if (!engine->register_socket_event(
                            socket_fd_, static_cast<int>(net::IOEvent::WRITE),
                            id())) {
                        return ExecutionResult::ERROR_OCCURRED;
                    }
                    return ExecutionResult::WAIT_FOR_SOCKET;
                }
                FALCON_LOG_ERROR_STREAM("SSL_write() 失败: " << ssl_error);
                return ExecutionResult::ERROR_OCCURRED;
            }
        } else {
#endif
            // 使用普通 send 发送 HTTP 数据（注入模拟内核发送缓冲满）
#ifdef _WIN32
            if (::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::HttpSendWantWrite)) {
                WSASetLastError(WSAEWOULDBLOCK);
                n = -1;
            } else {
                n = send(socket_fd_, data, static_cast<int>(remaining),
                         kSendFlags);
            }
#else
            if (::falcon::detail::inject_failure(
                    ::falcon::detail::InjectPoint::HttpSendWantWrite)) {
                errno = EAGAIN;
                n = -1;
            } else {
                n = send(socket_fd_, data, remaining, kSendFlags);
            }
#endif
            if (n < 0) {
                if (sock_would_block(sock_errno())) {
                    if (!engine->register_socket_event(
                            socket_fd_, static_cast<int>(net::IOEvent::WRITE),
                            id())) {
                        return ExecutionResult::ERROR_OCCURRED;
                    }
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
    if (resume_initial_) {
        response_cmd->set_resume_initial(true);
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

} // namespace falcon
