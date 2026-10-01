#pragma once

#include <falcon/protocol_handler.hpp>

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

} // namespace detail

} // namespace falcon::protocols
