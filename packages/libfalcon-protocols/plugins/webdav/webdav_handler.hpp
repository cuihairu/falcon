#pragma once

#include <falcon/protocol_handler.hpp>

#include <cstdint>
#include <memory>
#include <string>

namespace falcon::protocols {

/// WebDAV (RFC 4918) download handler on the libcurl data plane.
///
/// dav:// 重写为 http://、davs:// 重写为 https:// 后交 libcurl（GET 下载
/// 与 HEAD 探测对 WebDAV 服务器即普通 HTTP 语义）。认证经 CURLOPT_
/// USERNAME/PASSWORD + CURLOPT_HTTPAUTH(CURLAUTH_ANY)——Basic 与 Digest
/// 都由 libcurl 按 401 挑战协商，无需区分服务器形态。userinfo 里的
/// 用户名/密码经 percent-decode；**path 保持 percent-encoded 原样交
/// curl**（curl 原样发送、服务器解码——与 SFTP 相反，那里 path 必须
/// 解码成 raw 交 libssh2）。断点续传：CURLOPT_RESUME_FROM_LARGE（curl
/// 自带 Range 防护——服务器对带 Range 的请求回 200 而非 206 时
/// CURLE_RANGE_ERROR），`.falcon.tmp` 尺寸即续传起点。
/// timeout_seconds = 停滞看门狗（LOW_SPEED 语义，0 = 关；绝不映射
/// CURLOPT_TIMEOUT 总时长硬帽——B6 P0 教训）；HEAD 探测保持总帽。
/// 纯 http(s):// 的 WebDAV 文件 URL 无需本插件——GET 本就是普通 HTTP。
class WebdavHandler : public IProtocolHandler {
public:
    WebdavHandler();
    ~WebdavHandler() override;

    WebdavHandler(const WebdavHandler&) = delete;
    WebdavHandler& operator=(const WebdavHandler&) = delete;

    [[nodiscard]] std::string protocol_name() const override { return "webdav"; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {"dav", "davs"};
    }
    [[nodiscard]] bool can_handle(const std::string& url) const override;
    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;
    void download(DownloadTask::Ptr task, IEventListener* listener) override;
    void pause(DownloadTask::Ptr task) override;
    void resume(DownloadTask::Ptr task, IEventListener* listener) override;
    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }
    [[nodiscard]] int priority() const override { return 50; }
};

/// 工厂（builtin_protocol_handlers 注册锚点消费）
std::unique_ptr<IProtocolHandler> create_webdav_handler();

namespace detail {

/// dav://[user[:pass]@]host[:port]/path 解析结果。
/// userinfo 经 percent-decode；path 保持 percent-encoded 原样
/// （交 curl 原样发送）。port==0 表示 scheme 缺省端口（http 80 /
/// https 443，交给 libcurl 默认）。
struct WebdavEndpoint {
    bool secure = false;
    std::string host;
    std::uint16_t port = 0;
    std::string user;
    std::string password;
    std::string path;
};

/// 解析失败（无 host / 空 path / 非法端口）抛 InvalidURLException。
WebdavEndpoint parse_dav_url(const std::string& url,
                             const DownloadOptions& options);

/// endpoint → 剥 userinfo 的 http(s) URL（IPv6 字面量保持 [..] 形态）。
std::string to_http_url(const WebdavEndpoint& endpoint);

} // namespace detail

} // namespace falcon::protocols
