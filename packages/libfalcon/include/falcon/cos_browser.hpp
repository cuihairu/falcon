/**
 * @file cos_browser.hpp
 * @brief 腾讯云COS资源浏览器接口
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/resource_browser.hpp>
#include <memory>

namespace falcon {

/**
 * @brief COS配置信息
 */
struct COSConfig {
    std::string secret_id;
    std::string secret_key;
    std::string region;          // 如: ap-beijing
    std::string bucket;          // 存储桶名称
    std::string app_id;          // APP ID（可选，新版本COS不需要）
    std::string token;           // 临时token（可选）
};

/**
 * @brief COS URL解析结果
 */
struct COSUrl {
    std::string bucket;
    std::string region;
    std::string key;
    std::string app_id;
};

/**
 * @brief COS URL解析器
 */
class COSUrlParser {
public:
    /**
     * @brief 解析COS URL
     * @param url COS URL (cos://bucket/key 或 cos://bucket-region/key)
     * @return 解析结果
     */
    static COSUrl parse(const std::string& url);
};

/**
 * @brief 腾讯云COS浏览器实现类
 */
class COSBrowser : public IResourceBrowser {
public:
    COSBrowser();
    ~COSBrowser() override;

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