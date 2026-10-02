/**
 * @file webdav_browser.hpp
 * @brief WebDAV资源浏览器接口
 * @author Falcon Team
 * @date 2026-10-02
 *
 * 覆盖原生 WebDAV 服务与 ALIST 网盘聚合（ALIST 对外即 WebDAV 端点），
 * 支持矩阵中的坚果云/InfiniCloud 及 Google Drive/OneDrive/Dropbox 等
 * 经 ALIST 挂载的路径均由此浏览器承接。数据面下载上传由 protocols 层
 * webdav 插件负责，本类只做浏览与元数据管理（PROPFIND/MKCOL/DELETE/
 * MOVE/COPY）。
 */

#pragma once

#include <falcon/storage/resource_browser.hpp>
#include <memory>

namespace falcon {

/**
 * @brief WebDAV 配置信息
 */
struct WebDavConfig {
    std::string username;   ///< Basic/Digest 凭据用户名（空 = 匿名）
    std::string password;
};

/**
 * @brief WebDAV URL 解析结果
 */
struct WebDavUrl {
    std::string scheme = "http";  ///< 传输协议（dav/webdav→http，davs/webdavs→https）
    std::string host;
    std::string port;             ///< 空表示默认端口
    std::string base_path;        ///< 端点路径前缀（如 ALIST 的 /dav）
    std::string username;         ///< URL 内嵌凭据（可被 options 覆盖）
    std::string password;
};

/**
 * @brief WebDAV URL 解析器
 */
class WebDavUrlParser {
public:
    /**
     * @brief 解析 WebDAV URL
     * @param url 形如 webdav://[user:pass@]host[:port]/base_path
     *            （协议名 dav/davs/webdavs 同样接受）
     * @return 解析结果；不认识的 scheme 时 scheme 为空
     */
    static WebDavUrl parse(const std::string& url);
};

/**
 * @brief WebDAV浏览器实现类
 */
class WebDavBrowser : public IResourceBrowser {
public:
    WebDavBrowser();
    ~WebDavBrowser() override;

    // IResourceBrowser 接口实现
    std::string get_name() const override;
    std::vector<std::string> get_supported_protocols() const override;
    bool can_handle(const std::string& url) const override;
    bool connect(const std::string& url,
                 const std::map<std::string, std::string>& options = {}) override;
    void disconnect() override;
    std::vector<RemoteResource> list_directory(
        const std::string& path,
        const ListOptions& options = {}
    ) override;
    RemoteResource get_resource_info(const std::string& path) override;
    bool create_directory(const std::string& path, bool recursive = false) override;
    bool remove(const std::string& path, bool recursive = false) override;
    bool rename(const std::string& old_path,
                const std::string& new_path) override;
    bool copy(const std::string& source_path,
              const std::string& dest_path) override;
    bool exists(const std::string& path) override;
    std::string get_current_directory() override;
    bool change_directory(const std::string& path) override;
    std::string get_root_path() override;
    std::map<std::string, uint64_t> get_quota_info() override;

private:
    class Impl;
    std::unique_ptr<Impl> p_impl_;
};

} // namespace falcon
