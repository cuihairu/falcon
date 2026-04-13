/**
 * @file upyun_browser.hpp
 * @brief 又拍云USS资源浏览器接口
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/resource_browser.hpp>
#include <memory>

namespace falcon {

/**
 * @brief 又拍云配置信息
 */
struct UpyunConfig {
    std::string username;       // 操作员名称
    std::string password;       // 操作员密码
    std::string bucket;         // 服务名称
    std::string domain;         // 自定义域名（可选）
    std::string api_domain;     // API域名（可选，默认为v0.api.upyun.com）
};

/**
 * @brief 又拍云URL解析结果
 */
struct UpyunUrl {
    std::string bucket;
    std::string key;
    std::string domain;
};

/**
 * @brief 又拍云URL解析器
 */
class UpyunUrlParser {
public:
    /**
     * @brief 解析又拍云URL
     * @param url 又拍云URL (upyun://bucket/key 或 upyun://service/key)
     * @return 解析结果
     */
    static UpyunUrl parse(const std::string& url);
};

/**
 * @brief 又拍云USS浏览器实现类
 */
class UpyunBrowser : public IResourceBrowser {
public:
    UpyunBrowser();
    ~UpyunBrowser() override;

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
    bool rename(const std::string& old_path, const std::string& new_path) override;
    bool copy(const std::string& source_path, const std::string& dest_path) override;
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