/**
 * @file thunder_plugin.cpp
 * @brief 迅雷 Thunder 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "thunder_plugin.hpp"
#include <falcon/http_plugin.hpp>
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <regex>
#include <openssl/base64.h>
#include <sstream>
#include <iomanip>

namespace falcon {
namespace plugins {

ThunderPlugin::ThunderPlugin() {
    FALCON_LOG_INFO("Thunder plugin initialized");
}

std::vector<std::string> ThunderPlugin::getSupportedSchemes() const {
    return {"thunder", "thunderxl"};
}

bool ThunderPlugin::canHandle(const std::string& url) const {
    return url.find("thunder://") == 0 || url.find("thunderxl://") == 0;
}

std::unique_ptr<IDownloadTask> ThunderPlugin::createTask(const std::string& url,
                                                       const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating thunder task for: {}", url);

    // 解析迅雷链接
    std::string originalUrl;
    try {
        originalUrl = parseThunderUrl(url);
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse thunder URL: {}", e.what());
        throw InvalidURLException("Invalid thunder URL: " + url);
    }

    FALCON_LOG_DEBUG("Resolved thunder URL to: {}", originalUrl);

    // 创建 HTTP 任务来下载原始链接
    auto httpPlugin = std::make_unique<HttpPlugin>();
    if (!httpPlugin->canHandle(originalUrl)) {
        throw UnsupportedProtocolException("Resolved URL not supported: " + originalUrl);
    }

    return httpPlugin->createTask(originalUrl, options);
}

std::string ThunderPlugin::parseThunderUrl(const std::string& thunderUrl) {
    std::regex thunderRegex(R"(^(thunder|thunderxl)://(.+)$)");
    std::smatch match;

    if (!std::regex_match(thunderUrl, match, thunderRegex)) {
        throw InvalidURLException("Invalid thunder URL format");
    }

    std::string protocol = match[1].str();
    std::string encoded = match[2].str();

    if (protocol == "thunder") {
        return decodeClassicThunder(encoded);
    } else if (protocol == "thunderxl") {
        return decodeThunderXL(encoded);
    }

    throw InvalidURLException("Unknown thunder protocol: " + protocol);
}

std::string ThunderPlugin::decodeClassicThunder(const std::string& encoded) {
    // 经典迅雷链接格式：thunder://[Base64编码后的URL]
    // Base64内容通常是：AA[原始URL]ZZ

    // Base64解码
    std::string decoded;
    try {
        decoded = base64_decode(encoded);
    } catch (const std::exception& e) {
        throw InvalidURLException("Failed to decode Base64: " + std::string(e.what()));
    }

    // 检查并移除 AA 和 ZZ 标记
    if (decoded.size() < 2) {
        throw InvalidURLException("Invalid decoded data length");
    }

    // 移除开头的 AA
    if (decoded.substr(0, 2) == "AA") {
        decoded = decoded.substr(2);
    }

    // 移除结尾的 ZZ
    if (decoded.size() >= 2 && decoded.substr(decoded.size() - 2) == "ZZ") {
        decoded = decoded.substr(0, decoded.size() - 2);
    }

    if (decoded.empty()) {
        throw InvalidURLException("Empty URL after decoding");
    }

    if (!isValidUrl(decoded)) {
        throw InvalidURLException("Invalid decoded URL: " + decoded);
    }

    return decoded;
}

std::string ThunderPlugin::decodeThunderXL(const std::string& encoded) {
    // 迅雷离线链接格式更复杂，可能包含额外的信息
    // 这里简化处理，实际实现需要根据迅雷的离线协议文档

    try {
        std::string decoded = base64_decode(encoded);

        // XL链接可能有特殊格式，这里做基础解析
        // 实际应用中需要更复杂的处理逻辑
        if (decoded.find("http://") == 0 || decoded.find("https://") == 0 ||
            decoded.find("ftp://") == 0 || decoded.find("magnet:") == 0) {
            return decoded;
        }

        // 如果不是直接链接，可能是需要进一步解析的格式
        // 这里抛出异常提示用户
        throw UnsupportedProtocolException("Thunder XL complex format not fully supported");

    } catch (const std::exception& e) {
        throw InvalidURLException("Failed to decode thunder XL URL: " + std::string(e.what()));
    }
}

bool ThunderPlugin::isValidUrl(const std::string& url) {
    // 简单的URL验证
    std::regex urlRegex(R"(^(https?|ftp|magnet|ed2k)://[^\s/$.?#].[^\s]*$)");
    return std::regex_match(url, urlRegex);
}

std::string ThunderPlugin::getLinkType(const std::string& url) {
    if (url.find("thunder://") == 0) {
        return "classic";
    } else if (url.find("thunderxl://") == 0) {
        return "xl";
    }
    return "unknown";
}

// Base64解码辅助函数
std::string ThunderPlugin::base64_decode(const std::string& encoded) {
    // 计算解码后的长度
    size_t len = encoded.size();
    size_t padding = 0;

    if (len > 0 && encoded[len - 1] == '=') {
        padding++;
        if (len > 1 && encoded[len - 2] == '=') {
            padding++;
        }
    }

    size_t decoded_len = (len * 3) / 4 - padding;
    std::string decoded;
    decoded.resize(decoded_len);

    // 使用 OpenSSL 进行 Base64 解码
    BIO* bio = BIO_new_mem_buf(encoded.c_str(), -1);
    BIO* b64 = BIO_new(BIO_f_base64());

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    int actual_len = BIO_read(bio, &decoded[0], encoded.size());

    BIO_free_all(bio);

    if (actual_len <= 0) {
        throw std::runtime_error("Base64 decode failed");
    }

    decoded.resize(actual_len);
    return decoded;
}

} // namespace plugins
} // namespace falcon