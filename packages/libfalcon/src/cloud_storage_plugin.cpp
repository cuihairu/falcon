/**
 * @file cloud_storage_plugin.cpp
 * @brief 网盘下载插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/cloud_storage_plugin.hpp>
#include <falcon/logger.hpp>
#ifdef HAVE_NLOHMANN_JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif
#include <curl/curl.h>
#include <algorithm>
#include <regex>
#include <sstream>

namespace falcon {

// 云平台URL模式
std::map<CloudPlatform, std::vector<std::regex>> CloudLinkDetector::url_patterns_;

// 初始化URL模式
void CloudLinkDetector::init_patterns() {
    if (!url_patterns_.empty()) return;

    // 百度网盘
    url_patterns_[CloudPlatform::BaiduNetdisk] = {
        std::regex(R"(https?://pan\.baidu\.com/s/[a-zA-Z0-9_-]+)"),
        std::regex(R"(https?://yun\.baidu\.com/s/[a-zA-Z0-9_-]+)"),
        std::regex(R"(baidupan://[a-zA-Z0-9]+)")
    };

    // 蓝奏云
    url_patterns_[CloudPlatform::LanzouCloud] = {
        std::regex(R"(https?://[a-zA-Z0-9-]*lanzou[a-z]\.com/[a-zA-Z0-9]+)"),
        std::regex(R"(https?://[a-zA-Z0-9-]*lanzou[a-z]\.net/[a-zA-Z0-9]+)"),
        std::regex(R"(https?://[a-zA-Z0-9-]*ww[a-z]\.lanzou[a-z]\.com/[a-zA-Z0-9]+)")
    };

    // 阿里云盘
    url_patterns_[CloudPlatform::AlibabaCloud] = {
        std::regex(R"(https?://www\.aliyundrive\.com/s/[a-zA-Z0-9]+)"),
        std::regex(R"(https?://www\.alipan\.com/s/[a-zA-Z0-9]+)"),
        std::regex(R"(alipan://[a-zA-Z0-9]+)")
    };

    // 腾讯微云
    url_patterns_[CloudPlatform::TencentWeiyun] = {
        std::regex(R"(https?://share\.weiyun\.com/[a-zA-Z0-9]+)"),
        std::regex(R"(https?://pan\.qq\.com/s/[a-zA-Z0-9]+)")
    };

    // 115网盘
    url_patterns_[CloudPlatform::115Cloud] = {
        std::regex(R"(https?://115\.com/s/[a-zA-Z0-9]+)"),
        std::regex(R"(https?://anxia\.com/s/[a-zA-Z0-9]+)")
    };

    // 夸克网盘
    url_patterns_[CloudPlatform::Quark] = {
        std::regex(R"(https?://pan\.quark\.cn/s/[a-zA-Z0-9]+)"),
        std::regex(R"(quark://[a-zA-Z0-9]+)")
    };

    // PikPak
    url_patterns_[CloudPlatform::PikPak] = {
        std::regex(R"(https?://mypikpak\.com/s/[a-zA-Z0-9]+)"),
        std::regex(R"(https?://share\.pikpak\.com/s/[a-zA-Z0-9]+)")
    };

    // MEGA
    url_patterns_[CloudPlatform::Mega] = {
        std::regex(R"(https?://mega\.co\.nz/#[a-zA-Z0-9_-]+)"),
        std::regex(R"(https?://mega\.nz/#[a-zA-Z0-9_-]+)"),
        std::regex(R"(mega://[a-zA-Z0-9!@#$%^&*()]+)")
    };

    // Google Drive
    url_patterns_[CloudPlatform::GoogleDrive] = {
        std::regex(R"(https?://drive\.google\.com/file/d/[a-zA-Z0-9_-]+)"),
        std::regex(R"(https?://drive\.google\.com/open\?id=[a-zA-Z0-9_-]+)"),
        std::regex(R"(https?://docs\.google\.com/[^\s]+)")
    };

    // OneDrive
    url_patterns_[CloudPlatform::OneDrive] = {
        std::regex(R"(https?://1drv\.ms/[a-zA-Z]/[a-zA-Z0-9!_-]+)"),
        std::regex(R"(https?://onedrive\.live\.com/[^\s]+)")
    };

    // Dropbox
    url_patterns_[CloudPlatform::Dropbox] = {
        std::regex(R"(https?://www\.dropbox\.com/s/[a-zA-Z0-9]+/[^\s]+)"),
        std::regex(R"(https?://dl\.dropboxusercontent\.com/s/[a-zA-Z0-9]+/[^\s]+)")
    };

    // Yandex Disk
    url_patterns_[CloudPlatform::YandexDisk] = {
        std::regex(R"(https?://disk\.yandex\.ru/d/[a-zA-Z0-9]+/[^\s]+)"),
        std::regex(R"(https?://yadi\.sk/d/[a-zA-Z0-9]+/[^\s]+)")
    };
}

CloudPlatform CloudLinkDetector::detect_platform(const std::string& url) {
    init_patterns();

    for (const auto& [platform, patterns] : url_patterns_) {
        for (const auto& pattern : patterns) {
            if (std::regex_match(url, pattern)) {
                return platform;
            }
        }
    }

    return CloudPlatform::Unknown;
}

std::string CloudLinkDetector::extract_file_id(const std::string& url, CloudPlatform platform) {
    std::string id;

    switch (platform) {
        case CloudPlatform::BaiduNetdisk: {
            std::regex pattern(R"(/s/([a-zA-Z0-9_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) {
                id = match[1].str();
            }
            break;
        }

        case CloudPlatform::LanzouCloud: {
            std::regex pattern(R"(/([a-zA-Z0-9]+)(?:\?|$))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) {
                id = match[1].str();
            }
            break;
        }

        case CloudPlatform::AlibabaCloud: {
            std::regex pattern(R"(/s/([a-zA-Z0-9]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) {
                id = match[1].str();
            }
            break;
        }

        case CloudPlatform::GoogleDrive: {
            std::regex pattern(R"(/file/d/([a-zA-Z0-9_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) {
                id = match[1].str();
            }
            break;
        }

        case CloudPlatform::OneDrive: {
            std::regex pattern(R"(/([a-zA-Z0-9!_-]+))");
            std::smatch match;
            if (std::regex_search(url, match, pattern)) {
                id = match[1].str();
            }
            break;
        }

        default:
            break;
    }

    return id;
}

std::string CloudLinkDetector::normalize_url(const std::string& url) {
    std::string normalized = url;

    // 移除追踪参数
    size_t query_pos = normalized.find('?');
    if (query_pos != std::string::npos) {
        normalized = normalized.substr(0, query_pos);
    }

    // 添加https如果缺失
    if (normalized.find("://") == std::string::npos) {
        normalized = "https://" + normalized;
    }

    return normalized;
}

// 基础HTTP客户端
class HttpClient {
public:
    static std::string get(const std::string& url,
                          const std::map<std::string, std::string>& headers = {},
                          int timeout = 30) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        // 设置请求头
        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : headers) {
            std::string header = key + ": " + value;
            header_list = curl_slist_append(header_list, header.c_str());
        }
        if (header_list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        return (res == CURLE_OK) ? response : "";
    }

    static std::string post(const std::string& url,
                           const std::string& data,
                           const std::map<std::string, std::string>& headers = {}) {
        CURL* curl = curl_easy_init();
        if (!curl) return "";

        std::string response;
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, data.c_str());

        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : headers) {
            std::string header = key + ": " + value;
            header_list = curl_slist_append(header_list, header.c_str());
        }
        if (header_list) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        }

        CURLcode res = curl_easy_perform(curl);
        curl_slist_free_all(header_list);
        curl_easy_cleanup(curl);

        return (res == CURLE_OK) ? response : "";
    }

private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }
};

// 蓝奏云插件实现
class LanzouCloudPlugin : public ICloudStoragePlugin {
public:
    std::string platform_name() const override {
        return "LanzouCloud";
    }

    CloudPlatform platform_type() const override {
        return CloudPlatform::LanzouCloud;
    }

    bool can_handle(const std::string& url) const override {
        CloudPlatform platform = CloudLinkDetector::detect_platform(url);
        return platform == CloudPlatform::LanzouCloud;
    }

    CloudExtractionResult extract_share_link(const std::string& share_url,
                                            const std::string& password = "") override {
        CloudExtractionResult result;
        result.platform_name = platform_name();
        result.platform_type = platform_type();

        // 规范化URL
        std::string normalized_url = CloudLinkDetector::normalize_url(share_url);
        std::string file_id = CloudLinkDetector::extract_file_id(normalized_url, platform_type());

        if (file_id.empty()) {
            result.error_message = "无效的蓝奏云链接";
            return result;
        }

        // 获取分享页面
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
        };

        std::string html = HttpClient::get(normalized_url, headers);
        if (html.empty()) {
            result.error_message = "无法访问分享页面";
            return result;
        }

        // 解析文件信息
        CloudFileInfo file_info;
        file_info.id = file_id;

        // 使用正则表达式提取文件名和大小
        std::regex name_regex(R"("filename":"([^"]+)")");
        std::regex size_regex(R"("size":"([^"]+)")");
        std::regex time_regex(R"("time":"([^"]+)")");

        std::smatch match;
        if (std::regex_search(html, match, name_regex)) {
            file_info.name = match[1].str();
        }

        if (std::regex_search(html, match, size_regex)) {
            file_info.size = parse_size(match[1].str());
        }

        // 获取下载链接
        std::regex download_regex(R"('url':\s*'([^']+)'[^']*?'pwd':\s*'([^']*)')");
        if (std::regex_search(html, match, download_regex)) {
            std::string download_url = match[1].str();
            std::string pwd = match[2].str();

            if (!pwd.empty() && !password.empty()) {
                // 需要密码验证
                json post_data = {
                    {"pwd", password}
                };
                std::string api_response = HttpClient::post(
                    normalized_url + "?ajax=1",
                    post_data.dump(),
                    {{"X-Requested-With", "XMLHttpRequest"}}
                );

                try {
                    json json_resp = json::parse(api_response);
                    if (json_resp.contains("url")) {
                        file_info.download_url = json_resp["url"].get<std::string>();
                        result.success = true;
                    }
                } catch (const std::exception& e) {
                    result.error_message = "密码验证失败";
                }
            } else if (pwd.empty()) {
                file_info.download_url = download_url;
                result.success = true;
            } else {
                file_info.password = pwd;
                result.error_message = "需要提取密码";
            }
        }

        if (result.success) {
            result.files.push_back(file_info);
        }

        return result;
    }

    std::string get_download_url(const std::string& file_id,
                                const CloudDownloadOptions& options = {}) override {
        // 实际实现需要调用API获取下载链接
        return "";
    }

    bool authenticate(const std::string& token) override {
        return true;
    }

    std::map<std::string, std::string> get_user_info() override {
        return {};
    }

    std::map<std::string, size_t> get_quota_info() override {
        return {};
    }

private:
    size_t parse_size(const std::string& size_str) {
        if (size_str.empty()) return 0;

        size_t value = 0;
        char suffix = 0;
        std::istringstream ss(size_str);
        ss >> value >> suffix;

        switch (std::toupper(suffix)) {
            case 'K': return value * 1024;
            case 'M': return value * 1024 * 1024;
            case 'G': return value * 1024 * 1024 * 1024;
            default: return value;
        }
    }
};

// CloudStorageManager实现
class CloudStorageManager::Impl {
public:
    std::vector<std::unique_ptr<ICloudStoragePlugin>> plugins_;

    void register_default_plugins() {
        plugins_.push_back(std::make_unique<LanzouCloudPlugin>());
        // 可以添加其他默认插件
    }
};

CloudStorageManager::CloudStorageManager()
    : p_impl(std::make_unique<Impl>()) {
    p_impl->register_default_plugins();
}

CloudStorageManager::~CloudStorageManager() = default;

void CloudStorageManager::register_plugin(std::unique_ptr<ICloudStoragePlugin> plugin) {
    if (plugin) {
        p_impl->plugins_.push_back(std::move(plugin));
    }
}

CloudExtractionResult CloudStorageManager::handle_share_link(
    const std::string& url,
    const std::string& password) {

    CloudExtractionResult result;

    // 检测平台
    CloudPlatform platform = CloudLinkDetector::detect_platform(url);
    if (platform == CloudPlatform::Unknown) {
        result.error_message = "不支持的网盘平台";
        return result;
    }

    // 查找对应的插件
    for (auto& plugin : p_impl->plugins_) {
        if (plugin->can_handle(url)) {
            Logger::info("使用 {} 处理网盘链接", plugin->platform_name());
            return plugin->extract_share_link(url, password);
        }
    }

    result.error_message = "未找到对应的网盘插件";
    return result;
}

std::string CloudStorageManager::get_direct_download_url(
    const std::string& url,
    const CloudDownloadOptions& options) {

    auto result = handle_share_link(url);
    if (result.success && !result.files.empty()) {
        return result.files[0].download_url;
    }
    return "";
}

std::vector<std::string> CloudStorageManager::get_supported_platforms() {
    std::vector<std::string> platforms;
    for (const auto& plugin : p_impl->plugins_) {
        platforms.push_back(plugin->platform_name());
    }
    return platforms;
}

std::vector<CloudExtractionResult> CloudStorageManager::batch_extract(
    const std::vector<std::string>& urls,
    const std::map<std::string, std::string>& passwords) {

    std::vector<CloudExtractionResult> results;

    for (const auto& url : urls) {
        std::string password;
        auto it = passwords.find(url);
        if (it != passwords.end()) {
            password = it->second;
        }

        results.push_back(handle_share_link(url, password));
    }

    return results;
}

} // namespace falcon