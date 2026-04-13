/**
 * @file qqdl_plugin.cpp
 * @brief QQ旋风 QQDL 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "qqdl_plugin.hpp"
#include <falcon/http_plugin.hpp>
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <regex>
#include <sstream>
#include <openssl/md5.h>
#include <iomanip>

namespace falcon {
namespace plugins {

QQDLPlugin::QQDLPlugin() {
    FALCON_LOG_INFO("QQDL plugin initialized");
}

std::vector<std::string> QQDLPlugin::getSupportedSchemes() const {
    return {"qqlink", "qqdl"};
}

bool QQDLPlugin::canHandle(const std::string& url) const {
    return url.find("qqlink://") == 0 || url.find("qqdl://") == 0;
}

std::unique_ptr<IDownloadTask> QQDLPlugin::createTask(const std::string& url,
                                                     const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating QQDL task for: {}", url);

    // 解析QQ旋风链接
    std::string originalUrl;
    try {
        originalUrl = parseQQUrl(url);
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse QQDL URL: {}", e.what());
        throw InvalidURLException("Invalid QQDL URL: " + url);
    }

    FALCON_LOG_DEBUG("Resolved QQDL URL to: {}", originalUrl);

    // 创建 HTTP 任务来下载原始链接
    auto httpPlugin = std::make_unique<HttpPlugin>();
    if (!httpPlugin->canHandle(originalUrl)) {
        throw UnsupportedProtocolException("Resolved URL not supported: " + originalUrl);
    }

    return httpPlugin->createTask(originalUrl, options);
}

std::string QQDLPlugin::parseQQUrl(const std::string& qqUrl) {
    std::regex qqRegex(R"(^(qqlink|qqdl)://(.+)$)");
    std::smatch match;

    if (!std::regex_match(qqUrl, match, qqRegex)) {
        throw InvalidURLException("Invalid QQDL URL format");
    }

    std::string encoded = match[2].str();
    return decodeQQUrl(encoded);
}

std::string QQDLPlugin::decodeQQUrl(const std::string& encoded) {
    // QQ旋风链接通常有以下几种格式：
    // 1. qqlink://[Base64编码的URL]
    // 2. qqlink://[GID]|[文件信息]
    // 3. qqdl://[特殊编码]

    // 检查是否包含GID分隔符
    if (encoded.find('|') != std::string::npos) {
        // GID格式解析
        std::istringstream iss(encoded);
        std::string gid;
        std::string fileInfo;

        std::getline(iss, gid, '|');
        std::getline(iss, fileInfo);

        if (!isValidGid(gid)) {
            throw InvalidURLException("Invalid GID format");
        }

        // 解析文件信息
        DownloadInfo info = parseDownloadInfo(fileInfo);
        return info.url;
    } else {
        // Base64编码格式
        try {
            std::string decoded = base64_decode(encoded);

            // 检查是否是有效的URL
            if (decoded.find("http://") == 0 || decoded.find("https://") == 0 ||
                decoded.find("ftp://") == 0) {
                return decoded;
            }

            // 可能需要进一步解析
            DownloadInfo info = parseDownloadInfo(decoded);
            return info.url;

        } catch (const std::exception& e) {
            throw InvalidURLException("Failed to decode QQDL URL: " + std::string(e.what()));
        }
    }
}

bool QQDLPlugin::isValidGid(const std::string& gid) {
    // QQ旋风的GID通常是32位的十六进制字符串
    if (gid.length() != 32) {
        return false;
    }

    return std::all_of(gid.begin(), gid.end(), [](char c) {
        return std::isxdigit(c);
    });
}

QQDLPlugin::DownloadInfo QQDLPlugin::parseDownloadInfo(const std::string& info) {
    DownloadInfo result;

    // QQ旋风可能使用特殊格式存储下载信息
    // 例如：URL|filename|filesize|cid
    std::istringstream iss(info);
    std::string token;

    std::vector<std::string> tokens;
    while (std::getline(iss, token, '|')) {
        tokens.push_back(token);
    }

    if (tokens.empty()) {
        throw InvalidURLException("Empty download info");
    }

    // 第一个token通常是URL
    result.url = tokens[0];

    // 解析其他信息
    for (size_t i = 1; i < tokens.size(); ++i) {
        if (i == 1 && !tokens[i].empty()) {
            result.filename = tokens[i];
        } else if (i == 2 && !tokens[i].empty()) {
            result.filesize = tokens[i];
        } else if (i == 3 && !tokens[i].empty()) {
            result.cid = tokens[i];
        }
    }

    // 如果URL看起来不像标准URL，可能是需要转换的格式
    if (result.url.find("://") == std::string::npos) {
        // 可能需要进行特殊解析
        throw UnsupportedProtocolException("Complex QQDL format requires additional parsing");
    }

    return result;
}

// Base64解码辅助函数
std::string QQDLPlugin::base64_decode(const std::string& encoded) {
    // 与 Thunder 插件相同的实现
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