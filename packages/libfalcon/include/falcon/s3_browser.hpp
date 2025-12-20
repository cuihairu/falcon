/**
 * @file s3_browser.hpp
 * @brief S3资源浏览器接口
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/resource_browser.hpp>
#include <memory>

namespace falcon {

/**
 * @brief S3配置信息
 */
struct S3Config {
    std::string access_key_id;
    std::string secret_access_key;
    std::string region = "us-east-1";
    std::string endpoint;
};

/**
 * @brief S3 URL解析结果
 */
struct S3Url {
    std::string bucket;
    std::string key;
    std::string region;
    std::string endpoint;
};

/**
 * @brief S3 URL解析器
 */
class S3UrlParser {
public:
    /**
     * @brief 解析S3 URL
     * @param url S3 URL (s3://bucket/key)
     * @return 解析结果
     */
    static S3Url parse(const std::string& url);
};

/**
 * @brief S3浏览器实现类
 */
class S3Browser : public IResourceBrowser {
public:
    S3Browser();
    ~S3Browser() override;

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