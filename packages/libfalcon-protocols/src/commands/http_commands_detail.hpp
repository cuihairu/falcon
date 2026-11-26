/**
 * @file http_commands_detail.hpp
 * @brief HTTP 命令 TU 间共享的内部辅助（socket 收口/错误码/镜像轮转/连接级重试构造/任务组终态收口/HTTP 401 认证挑战解析与授权头构造）
 * @author Falcon Team
 * @date 2026-11-22
 */

#pragma once

#include <falcon/protocols/commands/http_commands.hpp>
#include <falcon/protocols/download_engine_v2.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/logger.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>      // _open/_write/_close（file-allocation 直写预分配）
#include <fcntl.h>   // _O_WRONLY/_O_BINARY
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

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#endif

namespace falcon {
namespace protocols {
namespace detail {

inline void close_socket_fd(int fd) {
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
inline int sock_errno() { return WSAGetLastError(); }
inline std::string sock_err_str(int err) { return "WSA error " + std::to_string(err); }
inline bool sock_would_block(int err) { return err == WSAEWOULDBLOCK; }
#else
inline int sock_errno() { return errno; }
inline std::string sock_err_str(int err) { return strerror(err); }
inline bool sock_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
#endif

/// 段级换源的无状态镜像轮转：下一镜像 = uris_ 中 failed_url 的下一个
///（取模）。failed_url 不在列表（重定向后 URL）时回落主镜像 uris_[0]。
/// 无需游标成员，不触碰 try_next_uri 的钳位语义
inline std::string mirror_after(const RequestGroup& group,
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
inline std::unique_ptr<Command> make_connection_retry(DownloadEngineV2* engine,
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
inline void fail_group_terminal(DownloadEngineV2* engine, TaskId task_id,
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

// ============================== HTTP 401 认证 ==============================
// RFC 7617 Basic + RFC 2617/7616 Digest（qop="auth"；MD5 / MD5-sess /
// SHA-256 / SHA-256-sess）。仅挑战驱动（无抢占式发送）；NTLM /
// Negotiate 等不受支持的方案由调用侧按失败收口（aria2 同语义）

/// 标准 Base64（RFC 4648，带填充）——Basic 授权与代理认证共用
inline std::string base64_encode(const std::string& in) {
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

/// 小写副本（独立命名：各 TU 匿名命名空间已有 to_lower/ascii_lower
/// 同类辅助，using 引入同名实体会构成二义）
inline std::string to_lower_copy(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return out;
}

inline std::string trim_copy(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = s.find_last_not_of(" \t");
    return s.substr(begin, end - begin + 1);
}

/// WWW-Authenticate 挑战的解析结果（scheme 统一小写；参数键统一
/// 小写，值保留原始字节——algorithm 的逐字回显需要原始大小写）
struct AuthChallenge {
    std::string scheme;  ///< "basic" / "digest" / "ntlm" ...（小写）
    std::map<std::string, std::string> params;
};

/// 解析 WWW-Authenticate 头值，如
///   Digest realm="t@h", qop="auth,auth-int", nonce="...", opaque="..."
/// scheme 取首个空白分隔 token；参数段在引号外按逗号切分（引号内的
/// 逗号/转义对不属于分隔符）；值若为 "..." 形态则剥引号并还原
/// quoted-pair 转义
inline AuthChallenge parse_www_authenticate(const std::string& value) {
    AuthChallenge challenge;
    const auto sp = value.find_first_of(" \t");
    if (sp == std::string::npos) {
        challenge.scheme = to_lower_copy(trim_copy(value));
        return challenge;
    }
    challenge.scheme = to_lower_copy(trim_copy(value.substr(0, sp)));

    // 逐字符扫描参数区（从 scheme 之后起扫——否则首个参数键会被
    // 拼成 "digest realm" 这类带 scheme 前缀的怪物）
    bool in_quotes = false;
    std::vector<std::string> segments;
    std::string current;
    for (std::size_t i = sp + 1; i < value.size(); ++i) {
        const char c = value[i];
        if (in_quotes) {
            current += c;
            if (c == '\\' && i + 1 < value.size()) {
                current += value[++i];  // 转义对整体并入
            } else if (c == '"') {
                in_quotes = false;
            }
            continue;
        }
        if (c == '"') {
            in_quotes = true;
            current += c;
        } else if (c == ',') {
            segments.push_back(current);
            current.clear();
        } else {
            current += c;
        }
    }
    segments.push_back(current);

    for (const auto& seg : segments) {
        const std::string piece = trim_copy(seg);
        if (piece.empty()) continue;
        const auto eq = piece.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = to_lower_copy(trim_copy(piece.substr(0, eq)));
        std::string val = trim_copy(piece.substr(eq + 1));
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
            std::string unescaped;
            for (std::size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '\\' && i + 1 < val.size()) {
                    unescaped += val[++i];
                } else {
                    unescaped += val[i];
                }
            }
            val = std::move(unescaped);
        }
        if (!name.empty()) challenge.params[name] = std::move(val);
    }
    return challenge;
}

/// quoted-string 值转义：反斜杠与双引号各前置一个反斜杠
inline std::string escape_quoted_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

/// Basic 授权头值（RFC 7617）：base64("user:pass")
inline std::string build_basic_authorization(const std::string& username,
                                             const std::string& password) {
    return "Basic " + base64_encode(username + ":" + password);
}

/// Digest 客户端随机数（cnonce）：16 个小写 hex 字符
inline std::string generate_cnonce() {
    static const char* kHex = "0123456789abcdef";
    std::random_device rd;
    std::string out;
    out.reserve(16);
    for (int i = 0; i < 8; ++i) {
        const unsigned int v = rd() & 0xffu;
        out += kHex[(v >> 4) & 0xfu];
        out += kHex[v & 0xfu];
    }
    return out;
}

/// Digest 的 uri 参数 = 请求目标本身（absolute-form 代理路径剥去
/// scheme://authority 取 path；origin-form 原样；空目标回退 "/"）
inline std::string digest_request_uri(const std::string& request_target) {
    const auto scheme_end = request_target.find("://");
    if (scheme_end == std::string::npos) {
        return request_target.empty() ? "/" : request_target;
    }
    const auto path_start = request_target.find('/', scheme_end + 3);
    if (path_start == std::string::npos) return "/";
    return request_target.substr(path_start);
}

#ifdef FALCON_ENABLE_OPENSSL
/// EVP 摘要 → 小写 hex（"MD5"/"SHA-256" 等经 OpenSSL 名称解析）
inline bool evp_digest_hex(const char* algo, const std::string& data,
                           std::string& out_hex) {
    out_hex.clear();
    const EVP_MD* md = EVP_get_digestbyname(algo);
    if (!md) return false;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (EVP_Digest(data.data(), data.size(), digest, &digest_len, md,
                   nullptr) != 1) {
        return false;
    }
    static const char* kHex = "0123456789abcdef";
    out_hex.reserve(digest_len * 2);
    for (unsigned int i = 0; i < digest_len; ++i) {
        out_hex += kHex[(digest[i] >> 4) & 0xfu];
        out_hex += kHex[digest[i] & 0xfu];
    }
    return true;
}

/// 构造 Digest 授权头值（RFC 2617/7616）。返回空串 = 挑战不受支持：
/// - scheme 非 digest / 无 nonce（调用侧通常已分流，此处防御）
/// - algorithm 缺省按 MD5；支持 MD5 / MD5-sess / SHA-256 / SHA-256-sess
///   （大小写不敏感）；其余（如 SHA-512-256）不支持
/// - qop 菜单不含 "auth" 不支持（auth-int 需要响应体哈希，与流式
///   下载不兼容）
/// algorithm 参数原样回显挑战值（RFC 7616 要求与挑战逐字一致）
inline std::string build_digest_authorization(const AuthChallenge& challenge,
                                              const std::string& username,
                                              const std::string& password,
                                              const std::string& method,
                                              const std::string& uri,
                                              const std::string& cnonce,
                                              unsigned int nc = 1) {
    if (challenge.scheme != "digest") return {};
    const auto nonce_it = challenge.params.find("nonce");
    if (nonce_it == challenge.params.end() || nonce_it->second.empty()) {
        return {};
    }
    const std::string& nonce = nonce_it->second;
    const auto realm_it = challenge.params.find("realm");
    const std::string realm = realm_it != challenge.params.end()
                                  ? realm_it->second
                                  : std::string();

    // 算法分派（大小写不敏感；回显用挑战的逐字原值）
    const char* algo = "MD5";
    bool sess = false;
    const auto algo_it = challenge.params.find("algorithm");
    if (algo_it != challenge.params.end()) {
        const std::string a = to_lower_copy(algo_it->second);
        if (a == "md5") {
            algo = "MD5";
        } else if (a == "md5-sess") {
            algo = "MD5";
            sess = true;
        } else if (a == "sha-256") {
            algo = "SHA-256";
        } else if (a == "sha-256-sess") {
            algo = "SHA-256";
            sess = true;
        } else {
            return {};
        }
    }

    // qop 菜单（逗号分隔；引号已被 parse_www_authenticate 剥除）：
    // 仅支持 "auth"
    std::string qop_value;
    const auto qop_it = challenge.params.find("qop");
    if (qop_it != challenge.params.end()) {
        bool has_auth = false;
        std::size_t start = 0;
        while (start <= qop_it->second.size()) {
            const auto comma = qop_it->second.find(',', start);
            const std::string token = trim_copy(
                qop_it->second.substr(start, comma == std::string::npos
                                                ? std::string::npos
                                                : comma - start));
            if (token == "auth") {
                has_auth = true;
                break;
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (!has_auth) return {};
        qop_value = "auth";
    }

    auto digest_hex = [&](const char* algo_name,
                          const std::string& data) {
        std::string hex;
        return evp_digest_hex(algo_name, data, hex) ? hex : std::string();
    };

    // HA1 = H(user:realm:pass)；-sess 变体再套一层 nonce+cnonce
    std::string ha1 = digest_hex(algo, username + ":" + realm + ":" + password);
    if (ha1.empty()) return {};
    if (sess) {
        ha1 = digest_hex(algo, ha1 + ":" + nonce + ":" + cnonce);
        if (ha1.empty()) return {};
    }
    // HA2 = H(method:uri)
    const std::string ha2 = digest_hex(algo, method + ":" + uri);
    if (ha2.empty()) return {};

    char nc_hex[9];
    std::snprintf(nc_hex, sizeof(nc_hex), "%08x", nc);

    std::string response;
    if (qop_value.empty()) {
        // 旧式（RFC 2069 兼容形态）：response = H(HA1:nonce:HA2)
        response = digest_hex(algo, ha1 + ":" + nonce + ":" + ha2);
    } else {
        // RFC 2617 §3.2.2.1：nonce:nc:cnonce:qop（cnonce 在 qop 之前）
        response = digest_hex(algo, ha1 + ":" + nonce + ":" + nc_hex + ":" +
                                        cnonce + ":" + qop_value + ":" + ha2);
    }
    if (response.empty()) return {};

    // 头组装（RFC 2617 §3.5）；qop/nc/cnonce 仅 qop 路径携带
    std::string header = "Digest username=\"" + escape_quoted_string(username) +
                         "\", realm=\"" + escape_quoted_string(realm) +
                         "\", nonce=\"" + escape_quoted_string(nonce) +
                         "\", uri=\"" + escape_quoted_string(uri) +
                         "\", response=\"" + response + "\"";
    if (algo_it != challenge.params.end()) {
        header += ", algorithm=" + algo_it->second;
    }
    const auto opaque_it = challenge.params.find("opaque");
    if (opaque_it != challenge.params.end()) {
        header += ", opaque=\"" + escape_quoted_string(opaque_it->second) +
                  "\"";
    }
    if (!qop_value.empty()) {
        header += ", qop=" + qop_value + ", nc=" + nc_hex + ", cnonce=\"" +
                  escape_quoted_string(cnonce) + "\"";
    }
    return header;
}

// ========================= TLS-in-TLS 泵（HTTPS 代理） =========================
// HTTPS 代理隧道内的目标 TLS 会话（origin）挂内存 BIO——socket 载荷是
// 代理侧密文，不能 SSL_set_fd。密文搬运全走本泵：
//   origin wbio → outbox → transport SSL_write
//   transport SSL_read → origin rbio
//
// outbox 由调用方持有（命令成员）且跨事件重入保留——transport 非阻塞
// SSL_write 合法返回 WANT_WRITE 时，已从 origin wbio 读出的密文不能丢
// （无状态栈缓冲泵在此丢字节）；WANT_* 重入后续发 outbox 残量。
// SSL_read 期间 origin 可能产生新的待发密文（TLS 1.3 KeyUpdate 等
// 后握手消息），同样经 outbox 排出
enum class TlsInTlsPump {
    kProgress,  // 本轮搬运完成且有净推进（搬运的密文原样发出）
    kIdle,      // 本轮净搬运零字节且 transport 无可收——握手推进点（motor
                // 循环据此再驱动一次 SSL_connect；调用方其余消费点视同 kProgress
                // 继续推进）
    kWantRead,  // transport 读方向暂不可用（outbox 残量等待，注册 READ）
    kWantWrite, // transport 写方向暂不可用（outbox 残量等待，注册 WRITE）
    kEof,       // 代理侧 TLS 关闭（close_notify/断连）——隧道已死
    kError      // 硬失败
};

inline TlsInTlsPump pump_tls_in_tls(SSL* origin, SSL* transport,
                                    std::string& outbox) {
    static constexpr int kChunkSize = 16384;
    bool moved = false;

    for (;;) {
        // 1) origin 待发密文全量排入 outbox（BIO_pending 驱动）
        for (;;) {
            if (BIO_pending(SSL_get_wbio(origin)) == 0) break;
            char buf[kChunkSize];
            const int n = BIO_read(SSL_get_wbio(origin), buf, kChunkSize);
            if (n <= 0) return TlsInTlsPump::kError;
            outbox.append(buf, static_cast<std::size_t>(n));
            moved = true;
        }

        // 2) outbox → transport（部分写循环；WANT_* 时残量留在 outbox
        //    等待重入续发——这正是 outbox 必须由调用方持有的原因）
        while (!outbox.empty()) {
            const int n = SSL_write(transport, outbox.data(),
                                    static_cast<int>(outbox.size()));
            if (n > 0) {
                outbox.erase(0, static_cast<std::size_t>(n));
                moved = true;
                continue;
            }
            const int err = SSL_get_error(transport, n);
            if (err == SSL_ERROR_WANT_WRITE) {
                return TlsInTlsPump::kWantWrite;
            }
            if (err == SSL_ERROR_WANT_READ) {
                return TlsInTlsPump::kWantRead;
            }
            if (err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL ||
                err == SSL_ERROR_SSL) {
                return TlsInTlsPump::kEof;
            }
            return TlsInTlsPump::kError;
        }

        // 3) transport 入站密文 → origin rbio（喂给 origin 解密）
        for (;;) {
            char buf[kChunkSize];
            const int n = SSL_read(transport, buf, kChunkSize);
            if (n > 0) {
                moved = true;
                if (BIO_write(SSL_get_rbio(origin), buf, n) != n) {
                    return TlsInTlsPump::kError;
                }
                continue;
            }
            const int err = SSL_get_error(transport, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                break;  // 无更多入站（WANT_WRITE 极罕见——重协商，下轮再泵）
            }
            if (err == SSL_ERROR_ZERO_RETURN || err == SSL_ERROR_SYSCALL ||
                err == SSL_ERROR_SSL) {
                // 隧道侧 EOF/断连不等于立即失败：按记录逐条解密，此前 BIO_write
                // 进 origin 的密文全部有效（失败的记录 OpenSSL 自行丢弃）。
                // 本轮已搬运、或 origin 仍有未消费密文/已解密数据时按推进返回
                // ——调用方先读 origin；若 origin 之后真被饿死（响应被截断），
                // 下一轮泵 rbio 已空、无新数据 → 走 kEof 收口
                if (moved || BIO_pending(SSL_get_rbio(origin)) > 0 ||
                    SSL_pending(origin) > 0) {
                    return TlsInTlsPump::kProgress;
                }
                return TlsInTlsPump::kEof;
            }
            return TlsInTlsPump::kError;
        }

        // 4) origin 解密入站后可能产生新的待发（后握手消息）——回到
        //    步骤 1 排出；排净则按本轮是否有净推进返回。kProgress/kIdle
        //    的区分供握手推进点（advance_tls_handshake 隧道分支）区分
        //    「继续驱动 motor」与「无事可做，按 origin 状态等待/收口」
        if (BIO_pending(SSL_get_wbio(origin)) == 0) {
            return moved ? TlsInTlsPump::kProgress : TlsInTlsPump::kIdle;
        }
    }
}
#endif  // FALCON_ENABLE_OPENSSL

} // namespace detail
} // namespace protocols
} // namespace falcon
