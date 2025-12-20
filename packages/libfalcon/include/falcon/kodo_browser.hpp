/**
 * @file kodo_browser.hpp
 * @brief 七牛云Kodo资源浏览器接口
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/resource_browser.hpp>
#include <memory>

namespace falcon {

/**
 * @brief Kodo配置信息
 */
struct KodoConfig {
    std::string access_key;
    std::string secret_key;
    std::string bucket;          // 存储空间名称
    std::string domain;          // 自定义域名（可选）
    bool use_https = true;       // 是否使用HTTPS
};

/**
 * @brief Kodo URL解析结果
 */
struct KodoUrl {
    std::string bucket;
    std::string key;
    std::string domain;
};

/**
 * @brief Kodo URL解析器
 */
class KodoUrlParser {
public:
    /**
     * @brief 解析Kodo URL
     * @param url Kodo URL (kodo://bucket/key 或 qiniu://bucket/key)
     * @return 解析结果
     */
    static KodoUrl parse(const std::string& url);
};

/**
 * @brief 七牛云Kodo浏览器实现类
 */
class KodoBrowser : public IResourceBrowser {
public:
    KodoBrowser();
    ~KodoBrowser() override;

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