/**
 * @file cloud_link_detector.cpp
 * @brief 网盘链接识别（纯 STL 实现，无第三方依赖）
 *
 * 该文件同时编译进 libs/drives 门面与 cloud_storage 插件：
 * 两侧各自持有副本，避免插件 so 反向依赖门面库造成静态库符号解析问题。
 */
#include <falcon/drives/cloud_storage_plugin.hpp>

#include <algorithm>
#include <regex>
#include <string>

namespace falcon {

std::map<CloudPlatform, std::vector<std::regex>> CloudLinkDetector::url_patterns_;

void CloudLinkDetector::init_patterns() {
    if (!url_patterns_.empty()) return;

    struct PlatformPattern {
        CloudPlatform platform;
        const char* pattern;
    };
    static const PlatformPattern kPatterns[] = {
        {CloudPlatform::BaiduNetdisk, R"(https?://pan\.baidu\.com/s/[a-zA-Z0-9_-]+)"},
        {CloudPlatform::BaiduNetdisk, R"(https?://yun\.baidu\.com/s/[a-zA-Z0-9_-]+)"},
        {CloudPlatform::BaiduNetdisk, R"(baidupan://[a-zA-Z0-9]+)"},
        {CloudPlatform::LanzouCloud, R"(https?://([\w-]+\.)*lanzou[a-z]\.(com|net)/[\w]+)"},
        {CloudPlatform::LanzouCloud, R"(https?://([\w-]+\.)*lanzou[a-z]\.(com|net)/i[\w]+)"},
        {CloudPlatform::AlibabaCloud, R"(https?://www\.aliyundrive\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::AlibabaCloud, R"(https?://www\.alipan\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::AlibabaCloud, R"(alipan://[a-zA-Z0-9]+)"},
        {CloudPlatform::TencentWeiyun, R"(https?://share\.weiyun\.com/[a-zA-Z0-9]+)"},
        {CloudPlatform::TencentWeiyun, R"(https?://pan\.qq\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::Cloud115, R"(https?://115\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::Cloud115, R"(https?://anxia\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::Quark, R"(https?://pan\.quark\.cn/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::Quark, R"(quark://[a-zA-Z0-9]+)"},
        {CloudPlatform::TianyiCloud, R"(https?://cloud\.189\.cn/t/[a-zA-Z0-9]+)"},
        {CloudPlatform::PikPak, R"(https?://mypikpak\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::PikPak, R"(https?://share\.pikpak\.com/s/[a-zA-Z0-9]+)"},
        {CloudPlatform::Mega, R"(https?://mega\.co\.nz/#[a-zA-Z0-9_-]+)"},
        {CloudPlatform::Mega, R"(https?://mega\.nz/#[a-zA-Z0-9_-]+)"},
        {CloudPlatform::Mega, R"(mega://[a-zA-Z0-9!@#$%^&*()]+)"},
        {CloudPlatform::GoogleDrive, R"(https?://drive\.google\.com/file/d/[a-zA-Z0-9_-]+)"},
        {CloudPlatform::GoogleDrive, R"(https?://drive\.google\.com/open\?id=[a-zA-Z0-9_-]+)"},
        {CloudPlatform::GoogleDrive, R"(https?://docs\.google\.com/[^\s]+)"},
        {CloudPlatform::OneDrive, R"(https?://1drv\.ms/[a-zA-Z]/[a-zA-Z0-9!_-]+)"},
        {CloudPlatform::OneDrive, R"(https?://onedrive\.live\.com/[^\s]+)"},
        {CloudPlatform::Dropbox, R"(https?://www\.dropbox\.com/s/[a-zA-Z0-9]+/[^\s]+)"},
        {CloudPlatform::Dropbox, R"(https?://dl\.dropboxusercontent\.com/s/[a-zA-Z0-9]+/[^\s]+)"},
        {CloudPlatform::YandexDisk, R"(https?://disk\.yandex\.ru/d/[a-zA-Z0-9]+/[^\s]+)"},
        {CloudPlatform::YandexDisk, R"(https?://yadi\.sk/d/[a-zA-Z0-9]+/[^\s]+)"},
    };

    for (const auto& entry : kPatterns) {
        url_patterns_[entry.platform].emplace_back(entry.pattern);
    }
}

CloudPlatform CloudLinkDetector::detect_platform(const std::string& url) {
    init_patterns();
    const std::string normalized = normalize_url(url);
    for (const auto& [platform, patterns] : url_patterns_) {
        for (const auto& pattern : patterns) {
            if (std::regex_search(url, pattern) || std::regex_search(normalized, pattern)) {
                return platform;
            }
        }
    }
    return CloudPlatform::Unknown;
}

std::string CloudLinkDetector::platform_display_name(CloudPlatform platform) {
    switch (platform) {
        case CloudPlatform::BaiduNetdisk:  return "Baidu Pan";
        case CloudPlatform::LanzouCloud:   return "Lanzou Cloud";
        case CloudPlatform::AlibabaCloud:  return "Aliyun Drive";
        case CloudPlatform::TencentWeiyun: return "Tencent Weiyun";
        case CloudPlatform::Cloud115:      return "115 Cloud";
        case CloudPlatform::Quark:         return "Quark Drive";
        case CloudPlatform::TianyiCloud:   return "Tianyi Cloud";
        case CloudPlatform::PikPak:        return "PikPak";
        case CloudPlatform::Mega:          return "MEGA";
        case CloudPlatform::GoogleDrive:   return "Google Drive";
        case CloudPlatform::OneDrive:      return "OneDrive";
        case CloudPlatform::Dropbox:       return "Dropbox";
        case CloudPlatform::YandexDisk:    return "Yandex Disk";
        default:                           return "Unknown";
    }
}

std::string CloudLinkDetector::extract_file_id(const std::string& url, CloudPlatform platform) {
    std::string id;
    switch (platform) {
        case CloudPlatform::BaiduNetdisk:
        case CloudPlatform::AlibabaCloud:
        case CloudPlatform::Cloud115:
        case CloudPlatform::Quark:
        case CloudPlatform::PikPak:
        case CloudPlatform::Dropbox: {
            std::regex pattern(R"(/s/([a-zA-Z0-9_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::LanzouCloud: {
            std::regex pattern(R"(/([a-zA-Z0-9]+)(?:\?|$))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::TianyiCloud: {
            std::regex pattern(R"(/t/([a-zA-Z0-9]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::TencentWeiyun: {
            std::regex pattern(R"((?:share\.weiyun\.com/|/s/)([a-zA-Z0-9]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::Mega: {
            std::regex pattern(R"(#([a-zA-Z0-9_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::GoogleDrive: {
            std::regex pattern(R"((?:/file/d/|[?&]id=)([a-zA-Z0-9_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::OneDrive: {
            std::regex pattern(R"(/([a-zA-Z0-9!_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        case CloudPlatform::YandexDisk: {
            std::regex pattern(R"(/d/([a-zA-Z0-9]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) id = match[1].str();
            break;
        }
        default:
            break;
    }
    return id;
}

std::string CloudLinkDetector::normalize_url(const std::string& url) {
    std::string normalized = url;
    const size_t query_pos = normalized.find('?');
    if (query_pos != std::string::npos) {
        normalized = normalized.substr(0, query_pos);
    }
    if (normalized.find("://") == std::string::npos) {
        normalized = "https://" + normalized;
    }
    return normalized;
}

} // namespace falcon
