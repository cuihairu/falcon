/**
 * @file s3_plugin.hpp
 * @brief Amazon S3 兼容存储协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "plugin.hpp"
#include <string>
#include <map>
#include <vector>
#include <chrono>

namespace falcon {

/**
 * @brief S3 配置信息
 */
struct S3Config {
    std::string access_key_id;        // 访问密钥ID
    std::string secret_access_key;    // 秘密访问密钥
    std::string session_token;        // 会话令牌（可选）
    std::string region = "us-east-1"; // 区域
    std::string endpoint;             // 自定义端点（用于MinIO等）
    std::string bucket;               // 存储桶名称
    bool use_ssl = true;              // 使用SSL
    bool verify_ssl = true;           // 验证SSL证书
    std::string host_style = "virtual"; // virtual或path
    int timeout_seconds = 30;         // 超时时间
    int max_retries = 3;              // 最大重试次数
};

/**
 * @brief S3 对象信息
 */
struct S3ObjectInfo {
    std::string key;                  // 对象键
    std::string etag;                 // ETag
    size_t size = 0;                  // 对象大小
    std::string last_modified;        // 最后修改时间
    std::string storage_class;        // 存储类别
    std::map<std::string, std::string> metadata; // 元数据
};

/**
 * @brief S3 预签名URL配置
 */
struct S3PresignedUrlConfig {
    int expires_in_seconds = 3600;   // 过期时间（默认1小时）
    std::string method = "GET";       // HTTP方法
    std::map<std::string, std::string> custom_headers; // 自定义头部
};

/**
 * @brief S3 多部分上传信息
 */
struct S3MultipartUpload {
    std::string upload_id;            // 上传ID
    std::string key;                  // 对象键
    std::vector<std::string> parts;   // 已完成的分片
    size_t part_size = 8 * 1024 * 1024; // 分片大小（默认8MB）
};

/**
 * @brief S3 协议插件
 */
class S3Plugin : public BaseProtocolPlugin {
public:
    S3Plugin();
    ~S3Plugin() override;

    // BaseProtocolPlugin 接口
    bool can_handle(const std::string& url) const override;
    std::string get_protocol_name() const override { return "S3"; }

    // 下载任务
    std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options = {}) override;

    // S3 特定功能
    void set_config(const S3Config& config);
    S3Config get_config() const { return config_; }

    // 列出对象
    std::vector<S3ObjectInfo> list_objects(
        const std::string& prefix = "",
        size_t max_keys = 1000
    );

    // 获取对象信息
    S3ObjectInfo get_object_info(const std::string& key);

    // 生成预签名URL
    std::string generate_presigned_url(
        const std::string& key,
        const S3PresignedUrlConfig& config = {}
    );

    // 多部分上传
    S3MultipartUpload create_multipart_upload(const std::string& key);
    bool upload_part(
        const S3MultipartUpload& upload,
        int part_number,
        const std::string& data
    );
    bool complete_multipart_upload(
        const S3MultipartUpload& upload
    );
    bool abort_multipart_upload(const S3MultipartUpload& upload);

    // 测试连接
    bool test_connection();

private:
    class Impl;
    std::unique_ptr<Impl> p_impl_;
    S3Config config_;
};

/**
 * @brief S3 URL 解析器
 */
class S3UrlParser {
public:
    struct S3Url {
        std::string endpoint;      // 端点
        std::string bucket;        // 存储桶
        std::string key;           // 对象键
        std::string region;        // 区域
        bool use_ssl;              // 是否使用SSL
        bool is_virtual_host;      // 是否使用虚拟主机样式
    };

    /**
     * 解析S3 URL
     * 支持格式：
     * - s3://bucket/key
     * - https://bucket.s3.region.amazonaws.com/key
     * - https://s3.region.amazonaws.com/bucket/key
     * - https://endpoint/bucket/key (MinIO等)
     */
    static S3Url parse(const std::string& url);

    /**
     * 构建S3 URL
     */
    static std::string build(const S3Url& s3_url);

    /**
     * 规范化S3 URL
     */
    static std::string normalize(const std::string& url);
};

/**
 * @brief S3 认证器（AWS Signature Version 4）
 */
class S3Authenticator {
public:
    /**
     * 生成签名
     */
    static std::string sign_request(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const std::string& payload,
        const std::string& access_key,
        const std::string& secret_key,
        const std::string& region,
        const std::string& service,
        const std::chrono::system_clock::time_point& request_time
    );

    /**
     * 生成预签名URL
     */
    static std::string generate_presigned_url(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& query_params,
        const std::map<std::string, std::string>& headers,
        const std::string& access_key,
        const std::string& secret_key,
        const std::string& region,
        const std::string& service,
        int expires_in_seconds,
        const std::chrono::system_clock::time_point& request_time
    );

private:
    /**
     * 计算SHA256哈希
     */
    static std::string sha256(const std::string& data);

    /**
     * 计算HMAC-SHA256
     */
    static std::string hmac_sha256(
        const std::string& key,
        const std::string& data
    );

    /**
     * 十六进制编码
     */
    static std::string hex_encode(const std::vector<uint8_t>& data);

    /**
     * 获取规范请求字符串
     */
    static std::string get_canonical_request(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& query_params,
        const std::map<std::string, std::string>& headers,
        const std::string& payload
    );

    /**
     * 获取待签名字符串
     */
    static std::string get_string_to_sign(
        const std::chrono::system_clock::time_point& request_time,
        const std::string& region,
        const std::string& service,
        const std::string& canonical_request
    );
};

/**
 * @brief S3 存储类别枚举
 */
enum class S3StorageClass {
    Standard,
    ReducedRedundancy,
    StandardIA,
    OnezoneIA,
    IntelligentTiering,
    Glacier,
    GlacierDeepArchive,
    Outposts,
    GlacierInstantRetrieval
};

/**
 * @brief S3 工具函数
 */
class S3Utils {
public:
    /**
     * 解析存储类别
     */
    static S3StorageClass parse_storage_class(const std::string& class_name);

    /**
     * 获取存储类别名称
     */
    static std::string get_storage_class_name(S3StorageClass storage_class);

    /**
     * 格式化S3时间（ISO 8601）
     */
    static std::string format_time(
        const std::chrono::system_clock::time_point& time_point
    );

    /**
     * 解析S3时间
     */
    static std::chrono::system_clock::time_point parse_time(
        const std::string& time_string
    );

    /**
     * 计算范围下载的分片信息
     */
    static std::vector<std::pair<uint64_t, uint64_t>> calculate_parts(
        uint64_t total_size,
        uint64_t part_size = 8 * 1024 * 1024,
        size_t max_concurrency = 10
    );

    /**
     * 编码URL路径（S3特殊编码）
     */
    static std::string encode_s3_path(const std::string& path);
};

} // namespace falcon