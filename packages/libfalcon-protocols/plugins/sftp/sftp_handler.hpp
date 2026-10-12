#pragma once

#include <falcon/protocol_handler.hpp>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>

namespace falcon::protocols {

/// SFTP (SSH File Transfer Protocol) handler based on libssh2.
///
/// 认证序（与 aria2 --private-key/--http-user/--http-passwd 同语义）：
/// none → publickey（options.client_private_key 显式私钥优先，
/// 回落 ~/.ssh/id_ed25519 → ~/.ssh/id_rsa）→ password（URL userinfo
/// 主，options.http_username/http_password 兜底）。
/// 主机键校验：known_hosts 已知且不匹配 → 硬失败（防 MITM）；未知主机
/// → WARN accept-new（OpenSSH accept-new 同语义）。哈希（|1|）条目无法
/// 离线比对，跳过（跳过列表内条目不构成 mismatch 证据）。
/// 断点续传：REST/偏移语义 —— `.falcon.tmp` 临时文件尺寸为续传起点，
/// libssh2_sftp_seek64 定位后追加。
class SftpHandler : public IProtocolHandler {
public:
    std::string protocol_name() const override;
    std::vector<std::string> supported_schemes() const override;
    bool can_handle(const std::string& url) const override;
    FileInfo get_file_info(const std::string& url,
                           const DownloadOptions& options) override;
    void download(DownloadTask::Ptr task, IEventListener* listener) override;
    void pause(DownloadTask::Ptr task) override;
    void resume(DownloadTask::Ptr task, IEventListener* listener) override;
    void cancel(DownloadTask::Ptr task) override;

    bool supports_resume() const override { return true; }
    int priority() const override { return 50; }
};

/// 工厂（builtin_protocol_handlers 注册锚点消费）
std::unique_ptr<IProtocolHandler> create_sftp_handler();

namespace detail {

/// sftp://[user[:pass]@]host[:port]/path 解析结果。
/// userinfo 经 percent-decode；path 也 percent-decode。
struct SftpEndpoint {
    std::string host;
    std::uint16_t port = 22;
    std::string user;
    std::string password;
    std::string path;
};

/// 解析失败（无 host / 空 path / 非法端口）抛 InvalidURLException。
SftpEndpoint parse_sftp_url(const std::string& url,
                            const DownloadOptions& options);

// ---- 纯函数工具（单元测试直达；实现在 sftp_plugin.cpp）----

/// RFC 3986 percent-decoding（'+' 原样保留，非表单语义；截断/非法转义原样输出）。
std::string percent_decode(const std::string& s);

/// RFC 4648 标准 base64（含 '=' 填充）。
std::string base64_encode(const unsigned char* data, std::size_t len);

/// 远端路径末段（rfind('/')；无 '/' 返回整串）。
std::string remote_basename(const std::string& path);

/// 两侧空白裁剪（std::isspace）。
std::string trim_copy(const std::string& s);

/// 端口解析：空 = 22；非数字 / 越界（0 或 >65535）抛 InvalidURLException。
std::uint16_t parse_port(const std::string& text, const std::string& url);

/// known_hosts hosts 字段（逗号分隔列表）是否覆盖 host:port
///（port==22 仅裸 host 形态命中；"[host]:port" 仅 port!=22 时参与）。
bool host_entry_covers(const std::string& entries, const std::string& host,
                       std::uint16_t port);

/// HOME / USERPROFILE 环境变量（平台相关；缺失或为空返回空串）。
std::string home_dir();

/// "~/..." 展开到家目录（HOME 缺失/为空原样返回；"~user" 非语义形态原样返回）。
std::string expand_home_path(const std::string& path);

/// known_hosts 逐条扫描裁决。
enum class KnownHostsVerdict {
    kMatch,    ///< 覆盖条目且 keytype+keyblob 全等
    kMismatch, ///< 覆盖条目但键不匹配（MITM 嫌疑，硬失败）
    kUnknown   ///< 无覆盖条目（accept-new）
};

/// 扫描 known_hosts 流内容（明文条目；哈希 |1| 条目跳过、注释/空行/
/// 字段不足行跳过），返回覆盖 host:port 的裁决。
KnownHostsVerdict scan_known_hosts(std::istream& in, const std::string& key_type,
                                   const std::string& key_b64,
                                   const std::string& host, std::uint16_t port);

} // namespace detail

} // namespace falcon::protocols
