/**
 * @file oss_browser_refactored.cpp
 * @brief 重构后的 OSS 解析器示例
 * @author Falcon Team
 * @date 2025-12-27
 */

#include <falcon/oss_browser.hpp>
#include <falcon/cloud_url_protocols.hpp>

namespace falcon {

// 使用新的通用解析器
OSSUrl OSSUrlParser::parse(const std::string& url) {
    using namespace cloud;

    OSSUrl oss_url;

    if (starts_with_protocol(url, PROTOCOL_OSS)) {
        // 自动计算正确的起始位置！
        size_t bucket_start = PROTOCOL_OSS.size();  // = 6，无需手动数
        size_t bucket_end = url.find('/', bucket_start);

        if (bucket_end == std::string::npos) {
            oss_url.bucket = url.substr(bucket_start);
        } else {
            std::string bucket_part = url.substr(bucket_start, bucket_end - bucket_start);

            // 解析 bucket.endpoint 格式
            size_t dot_pos = bucket_part.find('.');
            if (dot_pos != std::string::npos) {
                oss_url.bucket = bucket_part.substr(0, dot_pos);
                oss_url.endpoint = bucket_part.substr(dot_pos + 1);

                // 从 endpoint 提取 region
                if (oss_url.endpoint.find("oss-") == 0) {
                    size_t region_end = oss_url.endpoint.find(".aliyuncs.com");
                    if (region_end != std::string::npos) {
                        oss_url.region = oss_url.endpoint.substr(4, region_end - 4);
                    }
                }
            } else {
                oss_url.bucket = bucket_part;
                oss_url.endpoint = "oss-" + oss_url.region + ".aliyuncs.com";
            }

            oss_url.key = url.substr(bucket_end + 1);
        }
    }

    return oss_url;
}

} // namespace falcon
