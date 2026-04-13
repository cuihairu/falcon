/**
 * @file cloud_url_protocols.hpp
 * @brief 云存储 URL 协议常量定义
 * @author Falcon Team
 * @date 2025-12-27
 *
 * 设计理念：
 * 使用 constexpr 常量替代魔法数字，避免手动计算协议前缀长度
 *
 * 使用方法：
 *   using namespace falcon::cloud;
 *   size_t start = PROTOCOL_OSS.size();  // 自动 = 6，无需手动数
 */

#pragma once

#include <string_view>
#include <string>
#include <algorithm>

namespace falcon::cloud {

//==============================================================================
// 云存储协议前缀常量（编译期计算）
//==============================================================================

inline constexpr std::string_view PROTOCOL_S3 = "s3://";
inline constexpr std::string_view PROTOCOL_OSS = "oss://";
inline constexpr std::string_view PROTOCOL_COS = "cos://";
inline constexpr std::string_view PROTOCOL_KODO = "kodo://";
inline constexpr std::string_view PROTOCOL_QINIU = "qiniu://";
inline constexpr std::string_view PROTOCOL_UPYUN = "upyun://";

//==============================================================================
// 辅助函数
//==============================================================================

/**
 * @brief 检查 URL 是否以指定协议开头
 */
inline bool starts_with_protocol(std::string_view url, std::string_view protocol) {
    return url.size() >= protocol.size() &&
           url.substr(0, protocol.size()) == protocol;
}

/**
 * @brief 跳过协议前缀，返回内容部分的起始位置
 * @return 协议前缀的长度，如果 URL 不以该协议开头则返回 std::string_view::npos
 */
inline size_t skip_protocol(std::string_view url, std::string_view protocol) {
    if (starts_with_protocol(url, protocol)) {
        return protocol.size();  // 自动计算正确的偏移量！
    }
    return std::string_view::npos;
}

/**
 * @brief 通用的 bucket/key 解析函数
 * @param url 完整的 URL
 * @param protocol 协议前缀
 * @return pair<bucket, key>
 */
inline std::pair<std::string, std::string> parse_bucket_and_key(
    std::string_view url,
    std::string_view protocol) {

    size_t content_start = skip_protocol(url, protocol);
    if (content_start == std::string_view::npos) {
        return {};
    }

    size_t bucket_end = url.find('/', content_start);

    std::string bucket;
    std::string key;

    if (bucket_end == std::string_view::npos) {
        // 只有 bucket，没有 key
        bucket = url.substr(content_start);
    } else {
        bucket = url.substr(content_start, bucket_end - content_start);
        key = url.substr(bucket_end + 1);
    }

    return {bucket, key};
}

/**
 * @brief 从 URL 中提取 bucket 名称
 */
inline std::string extract_bucket(std::string_view url, std::string_view protocol) {
    auto [bucket, key] = parse_bucket_and_key(url, protocol);
    return bucket;
}

/**
 * @brief 从 URL 中提取 key 路径
 */
inline std::string extract_key(std::string_view url, std::string_view protocol) {
    auto [bucket, key] = parse_bucket_and_key(url, protocol);
    return key;
}

/**
 * @brief 检测 URL 使用的协议
 * @return 协议前缀，如果无法识别返回空字符串
 */
inline std::string_view detect_protocol(std::string_view url) {
    // 按长度降序检查，避免 "qiniu://" 被错误识别为 "kodo://"
    static constexpr std::string_view protocols[] = {
        PROTOCOL_QINIU,   // 8
        PROTOCOL_UPYUN,   // 8
        PROTOCOL_KODO,    // 7
        PROTOCOL_OSS,     // 6
        PROTOCOL_COS,     // 6
        PROTOCOL_S3,      // 5
    };

    for (const auto& protocol : protocols) {
        if (starts_with_protocol(url, protocol)) {
            return protocol;
        }
    }

    return {};
}

//==============================================================================
// 协议验证
//==============================================================================

// 编译期验证：确保所有协议前缀都以 "://" 结尾
static_assert(PROTOCOL_S3.back() == '/' && PROTOCOL_S3[PROTOCOL_S3.size()-2] == '/' && PROTOCOL_S3[PROTOCOL_S3.size()-3] == ':',
              "S3 protocol must end with '://'");
static_assert(PROTOCOL_OSS.back() == '/' && PROTOCOL_OSS[PROTOCOL_OSS.size()-2] == '/' && PROTOCOL_OSS[PROTOCOL_OSS.size()-3] == ':',
              "OSS protocol must end with '://'");
static_assert(PROTOCOL_COS.back() == '/' && PROTOCOL_COS[PROTOCOL_COS.size()-2] == '/' && PROTOCOL_COS[PROTOCOL_COS.size()-3] == ':',
              "COS protocol must end with '://'");
static_assert(PROTOCOL_KODO.back() == '/' && PROTOCOL_KODO[PROTOCOL_KODO.size()-2] == '/' && PROTOCOL_KODO[PROTOCOL_KODO.size()-3] == ':',
              "Kodo protocol must end with '://'");
static_assert(PROTOCOL_QINIU.back() == '/' && PROTOCOL_QINIU[PROTOCOL_QINIU.size()-2] == '/' && PROTOCOL_QINIU[PROTOCOL_QINIU.size()-3] == ':',
              "Qiniu protocol must end with '://'");
static_assert(PROTOCOL_UPYUN.back() == '/' && PROTOCOL_UPYUN[PROTOCOL_UPYUN.size()-2] == '/' && PROTOCOL_UPYUN[PROTOCOL_UPYUN.size()-3] == ':',
              "Upyun protocol must end with '://'");

} // namespace falcon::cloud
