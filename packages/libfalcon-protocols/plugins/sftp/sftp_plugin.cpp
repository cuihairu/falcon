/// @file sftp_plugin.cpp
/// SFTP 协议处理器（libssh2 数据面）。
///
/// 阻塞会话模型：socket 保持非阻塞 + libssh2 阻塞模式
/// （libssh2_session_set_blocking(1)）+ libssh2_session_set_timeout ——
/// libssh2 阻塞包装在 EAGAIN 时经内部 wait_socket(poll) 等待并受
/// session 超时约束，timeout_seconds==0 不设（沿用「关看门狗」契约，
/// 语义同 http_commands 的 LOW_SPEED 对）。
/// 暂停/取消语义与 FtpHandler 同约定：pause()/cancel() 只置任务状态，
/// download 读循环逐块检查状态后正常 return（保留 .falcon.tmp 断点，
/// 绝不抛异常——worker 的 catch 会把 Paused 覆写成 Failed）。
/// 读循环每 32KB 检查一次任务状态：活跃传输中暂停在毫秒级生效；
/// 停滞传输中暂停延迟受 session 超时约束（与 curl 的 progress 回调
/// 中止同量级）。

#include "sftp_handler.hpp"

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace falcon::protocols {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;

void close_socket(socket_t s) { ::closesocket(s); }

int sock_errno() { return WSAGetLastError(); }

bool sock_would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }

/// 引擎内多线程共享进程级 Winsock 状态（dht_node 同款 call_once 模式）
void ensure_winsock() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA data;
        ::WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;

void close_socket(socket_t s) { ::close(s); }

int sock_errno() { return errno; }

bool sock_would_block(int e) { return e == EINPROGRESS || e == EWOULDBLOCK; }
#endif

void set_non_blocking(socket_t s, bool non_blocking) {
#ifdef _WIN32
    u_long mode = non_blocking ? 1u : 0u;
    ::ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = ::fcntl(s, F_GETFL, 0);
    if (flags < 0) return;
    flags = non_blocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    ::fcntl(s, F_SETFL, flags);
#endif
}

/// 有界等待 socket 可写/可读（毫秒）；返回 >0 就绪 / 0 超时 / <0 出错
int wait_socket(socket_t s, short events, int timeout_ms) {
    pollfd pfd {};
    pfd.fd = static_cast<decltype(pfd.fd)>(s);
    pfd.events = events;
#ifdef _WIN32
    return ::WSAPoll(&pfd, 1, timeout_ms);
#else
    return ::poll(&pfd, 1, timeout_ms);
#endif
}

std::string home_dir() {
#ifdef _WIN32
    const char* h = std::getenv("USERPROFILE");
#else
    const char* h = std::getenv("HOME");
#endif
    return (h && *h) ? h : std::string();
}

/// 展开开头的 "~/"（无法解析 HOME 时原样返回）
std::string expand_home_path(const std::string& path) {
    if (path.rfind("~/", 0) != 0) return path;
    std::string home = home_dir();
    return home.empty() ? path : home + path.substr(1);
}

std::string default_username() {
#ifdef _WIN32
    const char* u = std::getenv("USERNAME");
#else
    const char* u = std::getenv("USER");
#endif
    return (u && *u) ? u : std::string();
}

bool is_regular_file(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) &&
           std::filesystem::is_regular_file(path, ec);
}

std::uint64_t file_size_or_zero(const std::string& path) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) return 0;
    auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<std::uint64_t>(size);
}

/// RFC 3986 percent-decode（'+' 不作空格——SFTP 路径语义）
std::string percent_decode(const std::string& s) {
    auto hex_value = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_value(s[i + 1]);
            int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>(hi * 16 + lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

/// RFC 4648 标准 base64（known_hosts 键 blob 比对用）
std::string base64_encode(const unsigned char* data, std::size_t len) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (std::size_t i = 0; i < len; i += 3) {
        const bool has_two = i + 1 < len;
        const bool has_three = i + 2 < len;
        unsigned int value = static_cast<unsigned int>(data[i]) << 16;
        if (has_two) value |= static_cast<unsigned int>(data[i + 1]) << 8;
        if (has_three) value |= static_cast<unsigned int>(data[i + 2]);
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(has_two ? kAlphabet[(value >> 6) & 0x3F] : '=');
        out.push_back(has_three ? kAlphabet[value & 0x3F] : '=');
    }
    return out;
}

std::string remote_basename(const std::string& path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string trim_copy(const std::string& s) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(begin, end - begin);
}

/// libssh2 主机键类型 → known_hosts 键类型名；未知返回空串
const char* hostkey_type_name(int type) {
    switch (type) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return "ssh-rsa";
        case LIBSSH2_HOSTKEY_TYPE_DSS: return "ssh-dss";
#if LIBSSH2_VERSION_NUM >= 0x010900
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ecdsa-sha2-nistp256";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ecdsa-sha2-nistp384";
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ecdsa-sha2-nistp521";
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return "ssh-ed25519";
#endif
        default: return "";
    }
}

std::string session_error(LIBSSH2_SESSION* session, const std::string& what) {
    char* msg = nullptr;
    int len = 0;
    libssh2_session_last_error(session, &msg, &len, 0);
    std::string detail =
        (msg && len > 0) ? std::string(msg, static_cast<std::size_t>(len))
                         : std::string("unknown error");
    return what + ": " + detail;
}

std::uint16_t parse_port(const std::string& text, const std::string& url) {
    if (text.empty()) return 22;
    if (text.find_first_not_of("0123456789") != std::string::npos) {
        throw InvalidURLException("SFTP URL has invalid port '" + text +
                                  "': " + url);
    }
    unsigned long value = 0;
    try {
        value = std::stoul(text);
    } catch (const std::exception&) {
        throw InvalidURLException("SFTP URL has invalid port '" + text +
                                  "': " + url);
    }
    if (value == 0 || value > 65535) {
        throw InvalidURLException("SFTP URL port out of range '" + text +
                                  "': " + url);
    }
    return static_cast<std::uint16_t>(value);
}

/// known_hosts hosts 字段（逗号分隔）是否覆盖 endpoint 的 host:port。
/// 只做明文精确匹配：host == 主机名，或非 22 端口的 [host]:port 形态
/// （OpenSSH 写法）。通配符/哈希形态不支持。
bool host_entry_covers(const std::string& hosts_token,
                       const std::string& host, std::uint16_t port) {
    std::istringstream iss(hosts_token);
    std::string entry;
    const std::string bracketed =
        "[" + host + "]:" + std::to_string(port);
    while (std::getline(iss, entry, ',')) {
        if (entry == host) return true;
        if (port != 22 && entry == bracketed) return true;
    }
    return false;
}

/// libssh2 全局初始化守卫（libssh2_init/exit 自 1.2.5 起引用计数，
/// 每连接一对 init/exit 是安全用法）
struct Libssh2GlobalGuard {
    Libssh2GlobalGuard() {
        if (libssh2_init(0) != 0) {
            throw NetworkException("SFTP: libssh2 initialization failed");
        }
    }
    ~Libssh2GlobalGuard() { libssh2_exit(); }
    Libssh2GlobalGuard(const Libssh2GlobalGuard&) = delete;
    Libssh2GlobalGuard& operator=(const Libssh2GlobalGuard&) = delete;
};

/// 连接资源 RAII：socket / SSH 会话 / SFTP 子系统按序清理
struct SftpConnection {
    socket_t fd = kInvalidSocket;
    LIBSSH2_SESSION* session = nullptr;
    LIBSSH2_SFTP* sftp = nullptr;

    ~SftpConnection() {
        if (sftp) libssh2_sftp_shutdown(sftp);
        if (session) {
            libssh2_session_disconnect(session, "falcon: closing connection");
            libssh2_session_free(session);
        }
        if (fd != kInvalidSocket) close_socket(fd);
    }
    SftpConnection() = default;
    SftpConnection(const SftpConnection&) = delete;
    SftpConnection& operator=(const SftpConnection&) = delete;
};

/// 非阻塞 connect + poll 超时（连接阶段有界，即使 timeout_seconds==0
/// 也给 30s 上界——TCP SYN 重传的内核默认值本就有界，这里显式钉住）
socket_t connect_tcp(const std::string& host, std::uint16_t port,
                     std::size_t timeout_seconds) {
#ifdef _WIN32
    ensure_winsock();
#endif
    const std::string port_string = std::to_string(port);
    addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), port_string.c_str(), &hints,
                                 &result);
    if (rc != 0 || !result) {
        throw NetworkException("SFTP: failed to resolve host " + host);
    }

    const int connect_timeout_ms =
        static_cast<int>((timeout_seconds > 0 ? timeout_seconds : 30) * 1000);
    std::string last_error;
    socket_t sock = kInvalidSocket;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == kInvalidSocket) {
            last_error = "socket() failed";
            continue;
        }
        set_non_blocking(sock, true);
#ifdef _WIN32
        const int cr = ::connect(
            sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));
#else
        const int cr =
            ::connect(sock, ai->ai_addr, ai->ai_addrlen); // socklen_t
#endif
        if (cr == 0) {
            freeaddrinfo(result);
            return sock;
        }
        const int err = sock_errno();
        if (!sock_would_block(err)) {
            last_error = "connect() failed: " + std::to_string(err);
            close_socket(sock);
            continue;
        }
        const int ready = wait_socket(sock, POLLOUT, connect_timeout_ms);
        if (ready <= 0) {
            last_error = ready == 0 ? "connect timed out"
                                    : "poll() failed during connect";
            close_socket(sock);
            continue;
        }
        int so_error = 0;
        socklen_t len = sizeof(so_error);
        if (::getsockopt(sock, SOL_SOCKET, SO_ERROR,
                         reinterpret_cast<char*>(&so_error), &len) != 0 ||
            so_error != 0) {
            last_error = "connect failed: " +
                         std::to_string(so_error != 0 ? so_error
                                                      : sock_errno());
            close_socket(sock);
            continue;
        }
        freeaddrinfo(result);
        return sock; // 保持非阻塞：libssh2 阻塞模式经 wait_socket 有界等待
    }
    freeaddrinfo(result);
    throw NetworkException("SFTP: failed to connect to " + host + ":" +
                           port_string + " (" + last_error + ")");
}

/// 主机键校验：known_hosts 命中且不匹配 → 硬失败；未知 → WARN
/// accept-new；|1| 哈希条目无法离线比对（HMAC 键即盐，可解，但为不把
/// 密码学比对写错，先按跳过处理——被跳过的条目不构成 mismatch 证据）。
void verify_host_key(LIBSSH2_SESSION* session,
                     const detail::SftpEndpoint& endpoint) {
    std::size_t key_len = 0;
    int key_type = 0;
    const char* key_blob = libssh2_session_hostkey(session, &key_len, &key_type);
    if (!key_blob || key_len == 0) {
        throw NetworkException("SFTP: server did not present a host key");
    }
    const char* type_name = hostkey_type_name(key_type);
    if (!type_name || *type_name == '\0') {
        FALCON_LOG_WARN_STREAM("[sftp] 未知主机键类型(" << key_type
                                << "),跳过 known_hosts 校验: "
                                << endpoint.host);
        return;
    }
    const std::string key_b64 =
        base64_encode(reinterpret_cast<const unsigned char*>(key_blob),
                      key_len);

    const std::string hosts_path = home_dir() + "/.ssh/known_hosts";
    std::ifstream in(hosts_path);
    if (!in.is_open()) {
        FALCON_LOG_WARN_STREAM("[sftp] known_hosts 不存在,接受新主机"
                                " (accept-new): " << endpoint.host);
        return;
    }

    bool mismatch = false;
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        std::istringstream iss(trimmed);
        std::string hosts_token;
        std::string keytype_token;
        std::string keyblob_token;
        iss >> hosts_token >> keytype_token >> keyblob_token;
        if (keyblob_token.empty()) continue;
        if (hosts_token.rfind("|1|", 0) == 0) continue; // 哈希条目跳过
        if (!host_entry_covers(hosts_token, endpoint.host, endpoint.port)) {
            continue;
        }
        if (keytype_token == type_name && keyblob_token == key_b64) {
            return; // 校验通过
        }
        mismatch = true;
    }

    if (mismatch) {
        throw NetworkException(
            "SFTP: host key mismatch for " + endpoint.host +
            " (possible man-in-the-middle). If the server key legitimately "
            "changed, remove the old entry from " + hosts_path);
    }
    FALCON_LOG_WARN_STREAM("[sftp] 未知主机,接受并继续 (accept-new): "
                            << endpoint.host);
}

/// 认证序：none → publickey（显式私钥优先，回落 ~/.ssh 常规路径）→
/// password（URL userinfo 主，options 兜底）。加密私钥的 passphrase
/// 本批不接（passphrase 恒传 NULL——加密私钥会认证失败落到 password）。
void authenticate(LIBSSH2_SESSION* session,
                  const detail::SftpEndpoint& endpoint,
                  const DownloadOptions& options) {
    char* methods_raw = libssh2_userauth_list(
        session, endpoint.user.c_str(),
        static_cast<unsigned int>(endpoint.user.size()));
    if (!methods_raw) {
        // none 认证被接受时返回 NULL——用 authenticated 确认而非盲信
        if (libssh2_userauth_authenticated(session) != 0) {
            FALCON_LOG_WARN_STREAM("[sftp] 服务器接受 none 认证(匿名): "
                                    << endpoint.user);
            return;
        }
        throw NetworkException(
            session_error(session, "SFTP: failed to query auth methods"));
    }
    const std::string methods(methods_raw);
    const auto supports = [&methods](const char* name) {
        return methods.find(name) != std::string::npos;
    };

    if (supports("publickey")) {
        std::vector<std::string> keys;
        if (!options.client_private_key.empty()) {
            keys.push_back(expand_home_path(options.client_private_key));
        }
        const std::string home = home_dir();
        if (!home.empty()) {
            keys.push_back(home + "/.ssh/id_ed25519");
            keys.push_back(home + "/.ssh/id_rsa");
        }
        for (const auto& key : keys) {
            if (!is_regular_file(key)) continue;
            const std::string public_key = key + ".pub";
            const char* public_key_path =
                is_regular_file(public_key) ? public_key.c_str() : nullptr;
            const int rc = libssh2_userauth_publickey_fromfile(
                session, endpoint.user.c_str(), public_key_path,
                key.c_str(), nullptr);
            if (rc == 0) {
                FALCON_LOG_WARN_STREAM("[sftp] publickey 认证成功: " << key);
                return;
            }
        }
    }

    if (supports("password")) {
        const std::string& password = !endpoint.password.empty()
                                          ? endpoint.password
                                          : options.http_password;
        if (!password.empty()) {
            const int rc = libssh2_userauth_password_ex(
                session, endpoint.user.c_str(),
                static_cast<unsigned int>(endpoint.user.size()),
                password.c_str(),
                static_cast<unsigned int>(password.size()), nullptr);
            if (rc == 0) return;
        }
    }

    throw NetworkException("SFTP: authentication failed for " +
                           endpoint.user + "@" + endpoint.host +
                           " (server accepts: " + methods + ")");
}

std::unique_ptr<SftpConnection>
open_sftp_connection(const detail::SftpEndpoint& endpoint,
                     const DownloadOptions& options) {
    Libssh2GlobalGuard global_guard;
    auto conn = std::make_unique<SftpConnection>();
    conn->fd = connect_tcp(endpoint.host, endpoint.port, options.timeout_seconds);

    conn->session = libssh2_session_init();
    if (!conn->session) {
        throw NetworkException("SFTP: failed to initialize libssh2 session");
    }
    libssh2_session_set_blocking(conn->session, 1);
    if (options.timeout_seconds > 0) {
        libssh2_session_set_timeout(
            conn->session,
            static_cast<long>(options.timeout_seconds) * 1000L);
    }
    const int handshake_rc = libssh2_session_handshake(
        conn->session, static_cast<libssh2_socket_t>(conn->fd));
    if (handshake_rc != 0) {
        throw NetworkException(
            session_error(conn->session, "SFTP: handshake failed"));
    }

    verify_host_key(conn->session, endpoint);
    authenticate(conn->session, endpoint, options);

    conn->sftp = libssh2_sftp_init(conn->session);
    if (!conn->sftp) {
        throw NetworkException(
            session_error(conn->session, "SFTP: subsystem init failed"));
    }
    return conn;
}

} // namespace

namespace detail {

SftpEndpoint parse_sftp_url(const std::string& url,
                            const DownloadOptions& options) {
    const std::string scheme = "sftp://";
    std::string rest = url.substr(scheme.size());

    const std::size_t slash = rest.find('/');
    const std::string authority =
        slash == std::string::npos ? rest : rest.substr(0, slash);
    const std::string path =
        slash == std::string::npos ? std::string() : rest.substr(slash);

    std::string userpart;
    std::string hostpart;
    const std::size_t at = authority.rfind('@');
    if (at != std::string::npos) {
        userpart = authority.substr(0, at);
        hostpart = authority.substr(at + 1);
    } else {
        hostpart = authority;
    }

    std::string host;
    std::uint16_t port = 22;
    if (!hostpart.empty() && hostpart[0] == '[') {
        // RFC 3986：[IPv6-literal] 与可选 ]:port
        const std::size_t close = hostpart.find(']');
        if (close == std::string::npos) {
            throw InvalidURLException("SFTP URL has malformed IPv6 literal: " +
                                      url);
        }
        host = hostpart.substr(1, close - 1);
        if (close + 2 < hostpart.size() && hostpart[close + 1] == ':') {
            port = parse_port(hostpart.substr(close + 2), url);
        }
    } else {
        const std::size_t colon = hostpart.rfind(':');
        if (colon != std::string::npos) {
            port = parse_port(hostpart.substr(colon + 1), url);
            host = hostpart.substr(0, colon);
        } else {
            host = hostpart;
        }
    }
    if (host.empty()) {
        throw InvalidURLException("SFTP URL has no host: " + url);
    }

    std::string user;
    std::string password;
    if (at != std::string::npos) {
        const std::size_t colon = userpart.find(':');
        if (colon == std::string::npos) {
            user = userpart;
        } else {
            user = userpart.substr(0, colon);
            password = userpart.substr(colon + 1);
        }
    }
    if (user.empty()) user = options.http_username;
    if (user.empty()) user = default_username();
    if (path.empty()) {
        throw InvalidURLException("SFTP URL has no remote path: " + url);
    }

    SftpEndpoint endpoint;
    endpoint.host = host;
    endpoint.port = port;
    endpoint.user = percent_decode(user);
    endpoint.password = percent_decode(password);
    endpoint.path = percent_decode(path);
    return endpoint;
}

} // namespace detail

namespace {

/// 进度记账（FtpHandler 同款 200ms 节流；速度 = 本窗口字节差 / 窗口毫秒）
struct ProgressData {
    DownloadTask::Ptr task;
    IEventListener* listener;
    std::uint64_t start_offset;
    std::chrono::steady_clock::time_point last_update;
    std::uint64_t last_reported;
};

void maybe_report_progress(ProgressData& data, std::uint64_t total,
                           std::uint64_t current_bytes) {
    const auto now = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - data.last_update)
                        .count();
    if (ms < 200) return;
    std::uint64_t speed = 0;
    if (ms > 0 && current_bytes > data.last_reported) {
        speed = (current_bytes - data.last_reported) * 1000UL /
                static_cast<std::uint64_t>(ms);
    }
    data.task->update_progress(current_bytes, total, speed);
    data.last_update = now;
    data.last_reported = current_bytes;
    (void)data.listener; // 进度经 task->update_progress 的任务级监听链下发
}

/// 任务级/监听器级限速取严（options.speed_limit 与
/// query_speed_limit 双源），0 = 不限速
std::uint64_t effective_speed_limit(const DownloadOptions& options,
                                    IEventListener* listener,
                                    TaskId task_id) {
    std::uint64_t limit = options.speed_limit;
    if (listener) {
        const auto queried =
            static_cast<std::uint64_t>(listener->query_speed_limit(task_id));
        if (queried > 0 && (limit == 0 || queried < limit)) limit = queried;
    }
    return limit;
}

/// 简单窗口 pacing：目标耗时 = 已收字节 / 限速，超前则补眠
void apply_speed_limit(std::uint64_t limit,
                       std::chrono::steady_clock::time_point start,
                       std::uint64_t session_bytes) {
    if (limit == 0 || session_bytes == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                now - start)
                                .count();
    const auto target_ns = static_cast<long long>(
        static_cast<long double>(session_bytes) /
        static_cast<long double>(limit) * 1.0e9L);
    if (elapsed_ns < target_ns) {
        std::this_thread::sleep_for(
            std::chrono::nanoseconds(target_ns - elapsed_ns));
    }
}

} // namespace

// ============================================================================
// SftpHandler
// ============================================================================

std::string SftpHandler::protocol_name() const { return "sftp"; }

std::vector<std::string> SftpHandler::supported_schemes() const {
    return {"sftp"};
}

bool SftpHandler::can_handle(const std::string& url) const {
    return url.rfind("sftp://", 0) == 0;
}

FileInfo SftpHandler::get_file_info(const std::string& url,
                                    const DownloadOptions& options) {
    const auto endpoint = detail::parse_sftp_url(url, options);
    if (endpoint.user.empty()) {
        throw NetworkException(
            "SFTP: no username (provide in URL or set http_username)");
    }
    const auto conn = open_sftp_connection(endpoint, options);

    LIBSSH2_SFTP_ATTRIBUTES attrs {};
    if (libssh2_sftp_stat(conn->sftp, endpoint.path.c_str(), &attrs) != 0) {
        throw NetworkException(
            session_error(conn->session,
                          "SFTP: remote stat failed for " + endpoint.path));
    }
    if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
        throw NetworkException("SFTP: remote path is a directory: " +
                               endpoint.path);
    }
    FileInfo info;
    info.url = url;
    info.filename = remote_basename(endpoint.path);
    info.total_size = attrs.filesize;
    info.supports_resume = true;
    return info;
}

namespace {

/// 单次尝试：连接 → stat（即文件信息步）→ 续传定位 → 读循环 → 发布。
/// 任何 throw 由 download() 的重试循环收口；暂停/取消正常 return。
void download_once(DownloadTask::Ptr task, IEventListener* listener,
                   const detail::SftpEndpoint& endpoint,
                   const DownloadOptions& options) {
    const auto conn = open_sftp_connection(endpoint, options);

    LIBSSH2_SFTP_ATTRIBUTES attrs {};
    if (libssh2_sftp_stat(conn->sftp, endpoint.path.c_str(), &attrs) != 0) {
        throw NetworkException(
            session_error(conn->session,
                          "SFTP: remote stat failed for " + endpoint.path));
    }
    if (LIBSSH2_SFTP_S_ISDIR(attrs.permissions)) {
        throw NetworkException("SFTP: remote path is a directory: " +
                               endpoint.path);
    }
    const std::uint64_t total = attrs.filesize;
    {
        FileInfo info;
        info.url = task->url();
        info.filename = remote_basename(endpoint.path);
        info.total_size = total;
        info.supports_resume = true;
        task->set_file_info(info);
    }

    const std::string temp_path = task->output_path() + ".falcon.tmp";
    std::uint64_t start_offset = 0;
    if (options.resume_enabled) {
        start_offset = file_size_or_zero(temp_path);
    }
    if (start_offset > total) start_offset = 0; // 远端缩小 → 全新重下
    if (total > 0 && start_offset == total) {
        // 临时文件已完整 → 直接发布（此前尝试已收满但发布前中断）
        if (std::rename(temp_path.c_str(), task->output_path().c_str()) != 0) {
            throw FileIOException("SFTP: failed to publish completed file: " +
                                  task->output_path());
        }
        task->update_progress(total, total, 0);
        task->set_status(TaskStatus::Completed);
        return;
    }

    std::ofstream file;
    std::ios::openmode mode = std::ios::binary;
    mode |= (start_offset > 0 ? std::ios::app : std::ios::trunc);
    file.open(temp_path, mode);
    if (!file.is_open()) {
        throw FileIOException("SFTP: failed to open output file: " +
                              temp_path);
    }

    LIBSSH2_SFTP_HANDLE* handle =
        libssh2_sftp_open(conn->sftp, endpoint.path.c_str(),
                          LIBSSH2_FXF_READ, 0);
    if (!handle) {
        throw NetworkException(session_error(
            conn->session, "SFTP: remote open failed for " + endpoint.path));
    }
    struct HandleCloser {
        LIBSSH2_SFTP_HANDLE* handle;
        ~HandleCloser() {
            if (handle) libssh2_sftp_close(handle);
        }
    } handle_closer {handle};

    if (start_offset > 0) {
        libssh2_sftp_seek64(handle, start_offset);
        FALCON_LOG_WARN_STREAM("[sftp] 断点续传: " << endpoint.path << " 从 "
                                << start_offset << " / " << total
                                << " 字节继续");
    }

    ProgressData progress {task, listener, start_offset,
                           std::chrono::steady_clock::now(), start_offset};
    const auto session_start = std::chrono::steady_clock::now();
    const std::uint64_t speed_limit =
        effective_speed_limit(options, listener, task->id());
    std::uint64_t session_bytes = 0;
    char buffer[32768];

    for (;;) {
        const auto status = task->status();
        if (status == TaskStatus::Paused || status == TaskStatus::Cancelled) {
            return; // 保留 .falcon.tmp 断点，绝不抛（worker 会覆写状态）
        }
        const auto n = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (n == LIBSSH2_ERROR_EAGAIN) continue; // 阻塞模式不应出现，防御
        if (n < 0) {
            throw NetworkException(session_error(
                conn->session, "SFTP: read failed (code " +
                                   std::to_string(n) + ")"));
        }
        if (n == 0) break; // EOF

        file.write(buffer, static_cast<std::streamsize>(n));
        if (!file.good()) {
            throw FileIOException("SFTP: disk write failed for " + temp_path);
        }
        session_bytes += static_cast<std::uint64_t>(n);
        maybe_report_progress(progress, total, start_offset + session_bytes);
        apply_speed_limit(speed_limit, session_start, session_bytes);
    }

    file.flush();
    if (!file.good()) {
        throw FileIOException("SFTP: flush failed for " + temp_path);
    }
    file.close();

    // EOF 但尺寸不符 = 远端截断/变更，绝不发布残缺成品（重试按断点续传）
    if (total > 0 && start_offset + session_bytes != total) {
        throw NetworkException("SFTP: remote file truncated or changed (got " +
                               std::to_string(start_offset + session_bytes) +
                               " of " + std::to_string(total) + " bytes)");
    }

    if (std::rename(temp_path.c_str(), task->output_path().c_str()) != 0) {
        throw FileIOException("SFTP: failed to publish file: " +
                              task->output_path());
    }
    task->update_progress(total, total, 0);
    task->set_status(TaskStatus::Completed);
}

} // namespace

void SftpHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    const DownloadOptions options = task->options();
    const auto endpoint = detail::parse_sftp_url(task->url(), options);
    std::string last_error;

    for (std::size_t attempt = 0; attempt <= options.max_retries; ++attempt) {
        if (task->status() == TaskStatus::Paused ||
            task->status() == TaskStatus::Cancelled) {
            return; // 暂停/取消绝不抛
        }
        try {
            download_once(task, listener, endpoint, options);
            return;
        } catch (const std::exception& e) {
            last_error = e.what();
        }
        if (attempt >= options.max_retries) break;
        if (task->status() == TaskStatus::Paused ||
            task->status() == TaskStatus::Cancelled) {
            return;
        }
        const auto backoff = options.retry_delay_seconds
                             << attempt; // 指数退避（FtpHandler 同款）
        std::this_thread::sleep_for(std::chrono::seconds(backoff));
    }
    throw NetworkException("SFTP download failed after " +
                           std::to_string(options.max_retries + 1) +
                           " attempts: " + last_error);
}

void SftpHandler::pause(DownloadTask::Ptr task) {
    if (!task) return;
    task->set_status(TaskStatus::Paused);
}

void SftpHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    if (!task) return;
    task->set_status(TaskStatus::Downloading);
    download(task, listener);
}

void SftpHandler::cancel(DownloadTask::Ptr task) {
    if (!task) return;
    task->set_status(TaskStatus::Cancelled);
}

std::unique_ptr<IProtocolHandler> create_sftp_handler() {
    return std::make_unique<SftpHandler>();
}

} // namespace falcon::protocols
