/**
 * @file cloud_storage_plugin.cpp
 * @brief 网盘下载插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/drives/cloud_storage_plugin.hpp>
#include <falcon/logger.hpp>
#ifdef HAVE_NLOHMANN_JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#else
// Fallback: empty json type for compilation without nlohmann
struct json {
    json() = default;
    json(const std::string&) {}
    json(const std::initializer_list<std::pair<std::string, std::string>>&) {}
    std::string dump() const { return "{}"; }
    template<typename T>
    bool contains(const T&) const { return false; }
    template<typename T>
    T get() const { return T{}; }
    static json parse(const std::string&) { return json{}; }
    json& operator[](const std::string&) { return *this; }
    const json& operator[](const std::string&) const { return *this; }
};
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
        std::regex(R"(https?://([\w-]+\.)*lanzou[a-z]\.(com|net)/[\w]+)"),
        std::regex(R"(https?://([\w-]+\.)*lanzou[a-z]\.(com|net)/i[\w]+)")
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
    url_patterns_[CloudPlatform::Cloud115] = {
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

        // 链接已识别，后续任何分支都至少标记 recognized
        result.recognized = true;

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
        std::regex name_regex("\"filename\":\"([^\"]+)\"");
        std::regex size_regex("\"size\":\"([^\"]+)\"");
        std::regex time_regex("\"time\":\"([^\"]+)\"");

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
                    } else {
                        result.error_message = "密码验证失败：服务器未返回下载链接";
                    }
                } catch (const std::exception& e) {
                    result.error_message = std::string("密码验证失败：") + e.what();
                }
            } else if (pwd.empty()) {
                file_info.download_url = download_url;
                result.success = true;
            } else {
                file_info.password = pwd;
                result.error_message = "需要提取密码";
            }
        } else {
            // 页面结构无法识别下载链接（可能页面改版或需要密码入口）
            result.error_message = "未能从分享页面提取下载链接";
        }

        // recognized=true 时始终把已抓取的元数据填入 files
        // 上层可基于 name/size 显示信息，即使 download_url 为空
        result.files.push_back(file_info);

        return result;
    }

    std::string get_download_url(const std::string& /*file_id*/,
                                const CloudDownloadOptions& /*options*/ = {}) override {
        // 实际实现需要调用API获取下载链接
        return "";
    }

    bool authenticate(const std::string& /*token*/) override {
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

// 百度网盘插件实现
class BaiduNetdiskPlugin : public ICloudStoragePlugin {
public:
    std::string platform_name() const override {
        return "BaiduNetdisk";
    }

    CloudPlatform platform_type() const override {
        return CloudPlatform::BaiduNetdisk;
    }

    bool can_handle(const std::string& url) const override {
        CloudPlatform platform = CloudLinkDetector::detect_platform(url);
        return platform == CloudPlatform::BaiduNetdisk;
    }

    CloudExtractionResult extract_share_link(const std::string& share_url,
                                            const std::string& password = "") override {
        CloudExtractionResult result;
        result.platform_name = platform_name();
        result.platform_type = platform_type();

        std::string normalized_url = CloudLinkDetector::normalize_url(share_url);
        std::string file_id = CloudLinkDetector::extract_file_id(normalized_url, platform_type());

        if (file_id.empty()) {
            result.error_message = "无效的百度网盘链接";
            return result;
        }

        // 百度网盘需要通过 API 获取分享信息
        // 这里返回基础信息，实际下载需要用户在浏览器中完成
        CloudFileInfo file_info;
        file_info.id = file_id;
        file_info.share_url = normalized_url;
        file_info.password = password;

        // 尝试获取分享页面信息
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
        };

        std::string html = HttpClient::get(normalized_url, headers);
        if (!html.empty()) {
            // 解析页面标题获取可能的文件名
            std::regex title_regex("<title[^>]*>([^<]+)</title>");
            std::smatch match;
            if (std::regex_search(html, match, title_regex)) {
                std::string title = match[1].str();
                // 移除常见的后缀
                const size_t pos = title.find("百度网盘");
                if (pos != std::string::npos) {
                    title = title.substr(0, pos);
                }
                // 清理标题
                while (!title.empty() && (title.back() == ' ' || title.back() == '-')) {
                    title.pop_back();
                }
                if (!title.empty()) {
                    file_info.name = title;
                }
            }
        }

        // 百度网盘需要通过客户端或浏览器插件获取下载链接
        // 这里标记为已识别，但直链需进一步处理（recognized=true / success=false）
        result.files.push_back(file_info);
        result.recognized = true;
        result.success = false;
        result.error_message = "需要使用百度网盘客户端或浏览器插件获取下载链接";

        return result;
    }

    std::string get_download_url(const std::string& file_id,
                                const CloudDownloadOptions& options = {}) override {
        // 百度网盘的下载链接需要通过复杂的 API 调用获取
        // 这里返回空，表示需要使用其他方式
        (void)file_id;
        (void)options;
        return "";
    }

    bool authenticate(const std::string& token) override {
        // 百度网盘需要 BDUSS 或 access_token
        // 这里简单检查非空
        return !token.empty();
    }

    std::map<std::string, std::string> get_user_info() override {
        // 返回模拟的用户信息
        return {
            {"platform", "BaiduNetdisk"},
            {"status", "anonymous"}
        };
    }

    std::map<std::string, size_t> get_quota_info() override {
        // 返回模拟的配额信息
        return {
            {"used", 0},
            {"total", 0}  // 匿名用户无配额信息
        };
    }
};

// 阿里云盘插件实现
class AliyunDrivePlugin : public ICloudStoragePlugin {
public:
    std::string platform_name() const override {
        return "AliyunDrive";
    }

    CloudPlatform platform_type() const override {
        return CloudPlatform::AlibabaCloud;
    }

    bool can_handle(const std::string& url) const override {
        CloudPlatform platform = CloudLinkDetector::detect_platform(url);
        return platform == CloudPlatform::AlibabaCloud;
    }

    CloudExtractionResult extract_share_link(const std::string& share_url,
                                            const std::string& password = "") override {
        (void)password;  // 阿里云盘通常不需要密码
        CloudExtractionResult result;
        result.platform_name = platform_name();
        result.platform_type = platform_type();

        std::string normalized_url = CloudLinkDetector::normalize_url(share_url);
        std::string file_id = CloudLinkDetector::extract_file_id(normalized_url, platform_type());

        if (file_id.empty()) {
            result.error_message = "无效的阿里云盘链接";
            return result;
        }

        // 阿里云盘分享链接格式: https://www.alipan.com/s/xxxxx
        // 需要通过 API 获取分享信息
        CloudFileInfo file_info;
        file_info.id = file_id;
        file_info.share_url = normalized_url;

        // 尝试获取分享页面
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
        };

        std::string html = HttpClient::get(normalized_url, headers);
        if (!html.empty()) {
            // 尝试从页面提取文件信息
            std::regex title_regex("<title[^>]*>([^<]+)</title>");
            std::smatch match;
            if (std::regex_search(html, match, title_regex)) {
                std::string title = match[1].str();
                // 清理标题
                const size_t pos = title.find("阿里云盘");
                if (pos != std::string::npos) {
                    title = title.substr(0, pos);
                }
                while (!title.empty() && (title.back() == ' ' || title.back() == '-')) {
                    title.pop_back();
                }
                if (!title.empty()) {
                    file_info.name = title;
                }
            }
        }

        result.files.push_back(file_info);
        result.recognized = true;
        result.success = false;
        result.error_message = "需要使用阿里云盘客户端或扫码获取下载链接";

        return result;
    }

    std::string get_download_url(const std::string& file_id,
                                const CloudDownloadOptions& options = {}) override {
        (void)file_id;
        (void)options;
        return "";
    }

    bool authenticate(const std::string& token) override {
        return !token.empty();
    }

    std::map<std::string, std::string> get_user_info() override {
        return {
            {"platform", "AliyunDrive"},
            {"status", "anonymous"}
        };
    }

    std::map<std::string, size_t> get_quota_info() override {
        return {
            {"used", 0},
            {"total", 0}
        };
    }
};

// 夸克网盘插件实现
class QuarkDrivePlugin : public ICloudStoragePlugin {
public:
    std::string platform_name() const override {
        return "QuarkDrive";
    }

    CloudPlatform platform_type() const override {
        return CloudPlatform::Quark;
    }

    bool can_handle(const std::string& url) const override {
        CloudPlatform platform = CloudLinkDetector::detect_platform(url);
        return platform == CloudPlatform::Quark;
    }

    CloudExtractionResult extract_share_link(const std::string& share_url,
                                            const std::string& password = "") override {
        (void)password;
        CloudExtractionResult result;
        result.platform_name = platform_name();
        result.platform_type = platform_type();

        std::string normalized_url = CloudLinkDetector::normalize_url(share_url);
        std::string file_id = CloudLinkDetector::extract_file_id(normalized_url, platform_type());

        if (file_id.empty()) {
            result.error_message = "无效的夸克网盘链接";
            return result;
        }

        CloudFileInfo file_info;
        file_info.id = file_id;
        file_info.share_url = normalized_url;

        // 尝试获取分享页面
        std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
        };

        std::string html = HttpClient::get(normalized_url, headers);
        if (!html.empty()) {
            std::regex title_regex("<title[^>]*>([^<]+)</title>");
            std::smatch match;
            if (std::regex_search(html, match, title_regex)) {
                std::string title = match[1].str();
                const size_t pos = title.find("夸克网盘");
                if (pos != std::string::npos) {
                    title = title.substr(0, pos);
                }
                while (!title.empty() && (title.back() == ' ' || title.back() == '-')) {
                    title.pop_back();
                }
                if (!title.empty()) {
                    file_info.name = title;
                }
            }
        }

        result.files.push_back(file_info);
        result.recognized = true;
        result.success = false;
        result.error_message = "需要使用夸克网盘客户端获取下载链接";

        return result;
    }

    std::string get_download_url(const std::string& file_id,
                                const CloudDownloadOptions& options = {}) override {
        (void)file_id;
        (void)options;
        return "";
    }

    bool authenticate(const std::string& token) override {
        return !token.empty();
    }

    std::map<std::string, std::string> get_user_info() override {
        return {
            {"platform", "QuarkDrive"},
            {"status", "anonymous"}
        };
    }

    std::map<std::string, size_t> get_quota_info() override {
        return {
            {"used", 0},
            {"total", 0}
        };
    }
};

// ============================================================================
// 轻量级网盘插件模板
// ----------------------------------------------------------------------------
// 处理分享链接识别、规范化、HTTP 页面标题解析等通用流程
// 具体平台只需提供：平台名称、平台枚举、提取密码前缀、无法直链的提示
// ============================================================================
class LightweightCloudPluginBase : public ICloudStoragePlugin {
public:
    CloudExtractionResult extract_share_link(const std::string& share_url,
                                            const std::string& password = "") override {
        CloudExtractionResult result;
        result.platform_name = platform_name();
        result.platform_type = platform_type();

        const std::string normalized_url = CloudLinkDetector::normalize_url(share_url);
        const std::string file_id = CloudLinkDetector::extract_file_id(normalized_url, platform_type());
        if (file_id.empty()) {
            result.error_message = "无效的" + platform_display_name() + "链接";
            return result;
        }

        CloudFileInfo file_info;
        file_info.id = file_id;
        file_info.share_url = normalized_url;
        file_info.password = password;

        const std::map<std::string, std::string> headers = {
            {"User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36"}
        };
        const std::string html = HttpClient::get(normalized_url, headers);
        if (!html.empty()) {
            const std::string title = extract_html_title(html);
            if (!title.empty()) {
                file_info.name = cleanup_title(title);
            }
        }

        result.files.push_back(file_info);
        // 已识别平台并提取基础元数据；但无直接可下载的 URL，故 success=false
        // 上层可据此区分「识别成功但需进一步处理」与「无法识别」
        result.recognized = true;
        result.success = false;
        result.error_message = resolve_hint();
        return result;
    }

    std::string get_download_url(const std::string& /*file_id*/,
                                const CloudDownloadOptions& /*options*/ = {}) override {
        // 平台 API 限制：默认轻量实现无法直接给出可下载直链
        return "";
    }

    bool authenticate(const std::string& token) override {
        return !token.empty();
    }

    std::map<std::string, std::string> get_user_info() override {
        return {
            {"platform", platform_name()},
            {"status", "anonymous"}
        };
    }

    std::map<std::string, size_t> get_quota_info() override {
        return {{"used", 0}, {"total", 0}};
    }

protected:
    /// 平台中文展示名称，用于错误信息
    virtual std::string platform_display_name() const = 0;
    /// 返回需要进一步处理时的提示文本
    virtual std::string resolve_hint() const = 0;

private:
    static std::string extract_html_title(const std::string& html) {
        std::regex title_regex("<title[^>]*>([^<]+)</title>");
        std::smatch match;
        if (std::regex_search(html, match, title_regex)) {
            return match[1].str();
        }
        return {};
    }

    static std::string cleanup_title(std::string title) {
        const std::vector<std::string> suffixes = {
            "百度网盘", "阿里云盘", "夸克网盘", "腾讯微云", "115网盘",
            "PikPak", "MEGA", "Google Drive", "OneDrive", "Dropbox"
        };
        for (const auto& suffix : suffixes) {
            const size_t pos = title.find(suffix);
            if (pos != std::string::npos) {
                title.erase(pos);
                break;
            }
        }
        while (!title.empty() && (title.back() == ' ' || title.back() == '-' ||
                                  title.back() == '_' || title.back() == '|')) {
            title.pop_back();
        }
        return title;
    }
};

// 腾讯微云插件实现
class TencentWeiyunPlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "TencentWeiyun"; }
    CloudPlatform platform_type() const override { return CloudPlatform::TencentWeiyun; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::TencentWeiyun;
    }
protected:
    std::string platform_display_name() const override { return "腾讯微云"; }
    std::string resolve_hint() const override {
        return "需要使用腾讯微云客户端或 QQ 账号登录获取下载链接";
    }
};

// 115 网盘插件实现
class Cloud115Plugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "Cloud115"; }
    CloudPlatform platform_type() const override { return CloudPlatform::Cloud115; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::Cloud115;
    }
protected:
    std::string platform_display_name() const override { return "115网盘"; }
    std::string resolve_hint() const override {
        return "需要使用 115 浏览器或 115 客户端登录获取下载链接";
    }
};

// PikPak 插件实现
class PikPakPlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "PikPak"; }
    CloudPlatform platform_type() const override { return CloudPlatform::PikPak; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::PikPak;
    }
protected:
    std::string platform_display_name() const override { return "PikPak"; }
    std::string resolve_hint() const override {
        return "需要使用 PikPak 客户端登录获取下载链接";
    }
};

// MEGA 插件实现
class MegaPlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "MEGA"; }
    CloudPlatform platform_type() const override { return CloudPlatform::Mega; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::Mega;
    }
protected:
    std::string platform_display_name() const override { return "MEGA"; }
    std::string resolve_hint() const override {
        return "需要 MEGA 账号或客户端解密后获取下载链接";
    }
};

// Google Drive 插件实现
class GoogleDrivePlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "GoogleDrive"; }
    CloudPlatform platform_type() const override { return CloudPlatform::GoogleDrive; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::GoogleDrive;
    }
protected:
    std::string platform_display_name() const override { return "Google Drive"; }
    std::string resolve_hint() const override {
        return "需要 Google 账号授权或 API Key 调用 Drive API 获取下载链接";
    }
};

// OneDrive 插件实现
class OneDrivePlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "OneDrive"; }
    CloudPlatform platform_type() const override { return CloudPlatform::OneDrive; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::OneDrive;
    }
protected:
    std::string platform_display_name() const override { return "OneDrive"; }
    std::string resolve_hint() const override {
        return "需要 Microsoft 账号授权或 Graph API 获取下载链接";
    }
};

// Dropbox 插件实现
class DropboxPlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "Dropbox"; }
    CloudPlatform platform_type() const override { return CloudPlatform::Dropbox; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::Dropbox;
    }
protected:
    std::string platform_display_name() const override { return "Dropbox"; }
    std::string resolve_hint() const override {
        return "可通过将 dl.dropboxusercontent.com 替换域名或调用 Dropbox API 获取直链";
    }
};

// Yandex Disk 插件实现
class YandexDiskPlugin : public LightweightCloudPluginBase {
public:
    std::string platform_name() const override { return "YandexDisk"; }
    CloudPlatform platform_type() const override { return CloudPlatform::YandexDisk; }
    bool can_handle(const std::string& url) const override {
        return CloudLinkDetector::detect_platform(url) == CloudPlatform::YandexDisk;
    }
protected:
    std::string platform_display_name() const override { return "Yandex Disk"; }
    std::string resolve_hint() const override {
        return "可通过将 disk.yandex.ru 直链加 ?dl=1 或调用 Yandex Disk API 获取下载链接";
    }
};

// CloudStorageManager实现
class CloudStorageManager::Impl {
public:
    std::vector<std::unique_ptr<ICloudStoragePlugin>> plugins_;

    void register_default_plugins() {
        // 已实现的完整支持
        plugins_.push_back(std::make_unique<LanzouCloudPlugin>());

        // 识别 + 元数据 + 链接路由（直链获取待后续接入各平台 API）
        plugins_.push_back(std::make_unique<BaiduNetdiskPlugin>());
        plugins_.push_back(std::make_unique<AliyunDrivePlugin>());
        plugins_.push_back(std::make_unique<QuarkDrivePlugin>());
        plugins_.push_back(std::make_unique<TencentWeiyunPlugin>());
        plugins_.push_back(std::make_unique<Cloud115Plugin>());
        plugins_.push_back(std::make_unique<PikPakPlugin>());
        plugins_.push_back(std::make_unique<MegaPlugin>());
        plugins_.push_back(std::make_unique<GoogleDrivePlugin>());
        plugins_.push_back(std::make_unique<OneDrivePlugin>());
        plugins_.push_back(std::make_unique<DropboxPlugin>());
        plugins_.push_back(std::make_unique<YandexDiskPlugin>());
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

    // 查找对应的插件（允许插件自行识别，避免平台检测误判导致无法处理）
    for (auto& plugin : p_impl->plugins_) {
        if (plugin->can_handle(url)) {
            log_info("使用 " + plugin->platform_name() + " 处理网盘链接");
            return plugin->extract_share_link(url, password);
        }
    }

    result.error_message = "未找到对应的网盘插件";
    return result;
}

std::string CloudStorageManager::get_direct_download_url(
    const std::string& url,
    const CloudDownloadOptions& /*options*/) {

    auto result = handle_share_link(url);
    if (result.success && !result.files.empty()) {
        return result.files[0].download_url;
    }
    // 识别到平台但未拿到直链，记录诊断信息便于上层反馈
    if (result.recognized) {
        log_info("网盘 " + result.platform_name + " 需要进一步处理：" + result.error_message);
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
