/**
 * @file cloud_protocol.hpp
 * @brief 云存储协议枚举定义（替代方案）
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <string_view>
#include <array>
#include <stdexcept>

namespace falcon {

/**
 * @brief 云存储协议枚举
 *
 * 优点：
 * 1. 类型安全
 * 2. 编译期检查
 * 3. 避免拼写错误
 * 4. 自带字符串表示
 */
enum class CloudProtocol : uint8_t {
    S3,      // Amazon S3
    OSS,     // 阿里云 OSS
    COS,     // 腾讯云 COS
    Kodo,    // 七牛云 Kodo
    Qiniu,   // 七牛云 (别名)
    Upyun,   // 又拍云
    Unknown
};

/**
 * @brief 协议元数据
 */
struct ProtocolInfo {
    std::string_view prefix;      // 协议前缀，如 "s3://"
    std::string_view name;        // 协议名称
    std::string_view description; // 描述
};

// 编译期协议表
inline constexpr std::array<ProtocolInfo, 7> PROTOCOL_INFO = {{
    { "s3://",     "Amazon S3",      "Amazon Simple Storage Service" },
    { "oss://",    "Aliyun OSS",     "Alibaba Cloud Object Storage Service" },
    { "cos://",    "Tencent COS",    "Tencent Cloud Object Storage" },
    { "kodo://",   "Qiniu Kodo",     "Qiniu Cloud Storage" },
    { "qiniu://",  "Qiniu",          "Qiniu Cloud Storage (Alias)" },
    { "upyun://",  "Upyun USS",      "Upyun Cloud Storage" },
    { "",          "Unknown",        "Unknown protocol" }
}};

/**
 * @brief 协议工具类
 */
class CloudProtocolUtils {
public:
    /**
     * @brief 获取协议前缀
     */
    static constexpr std::string_view get_prefix(CloudProtocol protocol) {
        return PROTOCOL_INFO[static_cast<size_t>(protocol)].prefix;
    }

    /**
     * @brief 获取协议名称
     */
    static constexpr std::string_view get_name(CloudProtocol protocol) {
        return PROTOCOL_INFO[static_cast<size_t>(protocol)].name;
    }

    /**
     * @brief 从 URL 检测协议
     */
    static CloudProtocol detect_from_url(std::string_view url) {
        for (size_t i = 0; i < PROTOCOL_INFO.size() - 1; ++i) {  // -1 跳过 Unknown
            const auto& info = PROTOCOL_INFO[i];
            if (url.size() >= info.prefix.size() &&
                url.substr(0, info.prefix.size()) == info.prefix) {
                return static_cast<CloudProtocol>(i);
            }
        }
        return CloudProtocol::Unknown;
    }

    /**
     * @brief 解析 bucket 和 key（通用方法）
     */
    static std::pair<std::string, std::string> parse_bucket_and_key(
        std::string_view url,
        CloudProtocol protocol) {

        std::string_view prefix = get_prefix(protocol);

        if (url.size() < prefix.size() ||
            url.substr(0, prefix.size()) != prefix) {
            return {};
        }

        // 自动计算正确的起始位置！
        size_t content_start = prefix.size();
        size_t bucket_end = url.find('/', content_start);

        std::string bucket;
        std::string key;

        if (bucket_end == std::string_view::npos) {
            bucket = url.substr(content_start);
        } else {
            bucket = url.substr(content_start, bucket_end - content_start);
            key = url.substr(bucket_end + 1);
        }

        return {bucket, key};
    }
};

} // namespace falcon
