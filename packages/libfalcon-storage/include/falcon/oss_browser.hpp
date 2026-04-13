/**
 * @file oss_browser.hpp
 * @brief 阿里云OSS资源浏览器接口
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/resource_browser.hpp>
#include <memory>

namespace falcon {

/**
 * @brief OSS配置信息
 */
struct OSSConfig {
    std::string access_key_id;
    std::string access_key_secret;
    std::string endpoint;         // 如: oss-cn-beijing.aliyuncs.com
    std::string region;           // 如: cn-beijing
    std::string security_token;   // STS临时令牌（可选）
};

/**
 * @brief OSS URL解析结果
 */
struct OSSUrl {
    std::string bucket;
    std::string key;
    std::string endpoint;
    std::string region;
};

/**
 * @brief OSS URL解析器
 */
class OSSUrlParser {
public:
    /**
     * @brief 解析OSS URL
     * @param url OSS URL (oss://bucket/key 或 oss://bucket.endpoint/key)
     * @return 解析结果
     */
    static OSSUrl parse(const std::string& url);
};

/**
 * @brief 阿里云OSS浏览器实现类
 */
class OSSBrowser : public IResourceBrowser {
public:
    OSSBrowser();
    ~OSSBrowser() override;

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