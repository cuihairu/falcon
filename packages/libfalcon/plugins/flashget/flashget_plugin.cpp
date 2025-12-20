/**
 * @file flashget_plugin.cpp
 * @brief 快车 FlashGet 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "flashget_plugin.hpp"
#include <falcon/http_plugin.hpp>
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <regex>
#include <sstream>
#include <openssl/base64.h>
#include <iomanip>

namespace falcon {
namespace plugins {

FlashGetPlugin::FlashGetPlugin() {
    FALCON_LOG_INFO("FlashGet plugin initialized");
}

std::vector<std::string> FlashGetPlugin::getSupportedSchemes() const {
    return {"flashget", "fg"};
}

bool FlashGetPlugin::canHandle(const std::string& url) const {
    return url.find("flashget://") == 0 || url.find("fg://") == 0;
}

std::unique_ptr<IDownloadTask> FlashGetPlugin::createTask(const std::string& url,
                                                         const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating FlashGet task for: {}", url);

    // 解析快车链接
    std::string originalUrl;
    try {
        originalUrl = parseFlashGetUrl(url);
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse FlashGet URL: {}", e.what());
        throw InvalidURLException("Invalid FlashGet URL: " + url);
    }

    FALCON_LOG_DEBUG("Resolved FlashGet URL to: {}", originalUrl);

    // 创建 HTTP 任务来下载原始链接
    auto httpPlugin = std::make_unique<HttpPlugin>();
    if (!httpPlugin->canHandle(originalUrl)) {
        throw UnsupportedProtocolException("Resolved URL not supported: " + originalUrl);
    }

    // 设置引用页面（如果有的话）
    DownloadOptions modifiedOptions = options;
    if (url.find("ref=") != std::string::npos) {
        std::regex refRegex(R"(ref=([^&]+))");
        std::smatch match;
        if (std::regex_search(url, match, refRegex)) {
            modifiedOptions.referrer = urlDecode(match[1].str());
        }
    }

    return httpPlugin->createTask(originalUrl, modifiedOptions);
}

std::string FlashGetPlugin::parseFlashGetUrl(const std::string& flashgetUrl) {
    // FlashGet 链接格式：
    // flashget://[URL]
    // flashget://[Base64编码的URL]
    // flashget://[Base64编码的URL]&ref=[Base64编码的引用页面]
    // fg://[URL] (短格式)

    std::regex flashgetRegex(R"(^(flashget|fg)://(.+)$)");
    std::smatch match;

    if (!std::regex_match(flashgetUrl, match, flashgetRegex)) {
        throw InvalidURLException("Invalid FlashGet URL format");
    }

    std::string protocol = match[1].str();
    std::string encoded = match[2].str();

    // 如果是短格式 fg://，直接返回URL
    if (protocol == "fg") {
        return urlDecode(encoded);
    }

    // 处理引用页面参数
    std::string referrer;
    size_t refPos = encoded.find("&ref=");
    if (refPos != std::string::npos) {
        referrer = encoded.substr(refPos + 5);
        encoded = encoded.substr(0, refPos);
    }

    // 解码主URL
    std::string originalUrl = decodeFlashGetUrl(encoded, referrer);

    // 获取镜像链接（可选）
    auto mirrors = parseMirrors(originalUrl);
    if (!mirrors.empty()) {
        FALCON_LOG_DEBUG("Found {} mirror URLs for FlashGet download", mirrors.size());
    }

    return originalUrl;
}

std::string FlashGetPlugin::decodeFlashGetUrl(const std::string& encoded,
                                             const std::string& referrer) {
    // 尝试直接解码（如果URL没有编码）
    if (encoded.find("://") != std::string::npos) {
        return urlDecode(encoded);
    }

    // 尝试Base64解码
    try {
        std::string decoded = base64_decode(encoded);

        // FlashGet的Base64可能有特殊前缀
        if (decoded.find("[FLASHGET]") == 0) {
            decoded = decoded.substr(10); // 移除 [FLASHGET] 前缀
        }

        // URL解码
        return urlDecode(decoded);

    } catch (const std::exception& e) {
        // 如果Base64解码失败，尝试直接URL解码
        try {
            return urlDecode(encoded);
        } catch (...) {
            throw InvalidURLException("Failed to decode FlashGet URL");
        }
    }
}

std::string FlashGetPlugin::urlDecode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            // 解码 %XX 格式
            int hex;
            std::istringstream iss(encoded.substr(i + 1, 2));
            if (iss >> std::hex >> hex) {
                decoded += static_cast<char>(hex);
                i += 2;
            } else {
                decoded += encoded[i];
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

std::vector<std::string> FlashGetPlugin::parseMirrors(const std::string& url) {
    // FlashGet支持镜像列表
    // 这里是简化实现，实际FlashGet使用更复杂的镜像检测机制
    std::vector<std::string> mirrors;

    // 示例：从URL提取可能的镜像
    if (url.find("mirrorlist") != std::string::npos) {
        // 解析镜像列表
        // 实际实现需要根据FlashGet的镜像协议
    }

    return mirrors;
}

// Base64解码辅助函数
std::string FlashGetPlugin::base64_decode(const std::string& encoded) {
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