/**
 * @file s3_browser.cpp
 * @brief S3资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/s3_browser.hpp>
#include <falcon/logger.hpp>

// Use spdlog for logging if available
#ifdef FALCON_USE_SPDLOG
#include <spdlog/spdlog.h>
#define LOG_ERROR(msg, ...) spdlog::error(msg, __VA_ARGS__)
#define LOG_WARN(msg, ...) spdlog::warn(msg, __VA_ARGS__)
#define LOG_INFO(msg, ...) spdlog::info(msg, __VA_ARGS__)
#else
// Fallback to simple logger
#include <iostream>
#define LOG_ERROR(msg, ...) std::cerr << "[ERROR] " << msg << std::endl
#define LOG_WARN(msg, ...) std::cerr << "[WARN] " << msg << std::endl
#define LOG_INFO(msg, ...) std::cout << "[INFO] " << msg << std::endl
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>

#ifdef FALCON_BROWSER_NO_JSON
#pragma message "Warning: Compiling S3 browser without JSON support. Some features may not work."
#endif

using json = nlohmann::json;

namespace falcon {

// S3UrlParser 实现
S3Url S3UrlParser::parse(const std::string& url) {
    S3Url s3_url;

    if (url.find("s3://") == 0) {
        // s3://bucket/key
        size_t bucket_start = 5; // skip "s3://" (s3 是 2 字符 + :// 是 3 字符 = 5)
        size_t bucket_end = url.find('/', bucket_start);

        if (bucket_end == std::string::npos) {
            s3_url.bucket = url.substr(bucket_start);
        } else {
            s3_url.bucket = url.substr(bucket_start, bucket_end - bucket_start);
            s3_url.key = url.substr(bucket_end + 1);
        }
    }

    return s3_url;
}

/**
 * @brief S3浏览器实现细节
 */
class S3Browser::Impl {
public:
    Impl() : curl_(curl_easy_init()) {
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }

    ~Impl() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    std::string build_s3_url(const std::string& bucket, const std::string& key = "") {
        std::string url = "https://" + bucket + ".s3." + s3_url_.region + ".amazonaws.com";
        if (!key.empty()) {
            url += "/" + url_encode(key);
        }
        return url;
    }

    std::string perform_s3_request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& body = "") {

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

        // 添加AWS签名头部
        std::map<std::string, std::string> signed_headers = headers;
        if (config_.access_key_id.empty()) {
            LOG_WARN("No AWS credentials provided%s", "");
        }

        // 简化签名（实际应使用AWS签名V4）
        signed_headers["Date"] = get_current_time();
        signed_headers["Host"] = get_host_from_url(url);

        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : signed_headers) {
            std::string header = key + ": " + value;
            header_list = curl_slist_append(header_list, header.c_str());
        }

        if (header_list) {
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);
        }

        // 设置请求体
        if (!body.empty()) {
            curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.c_str());
        }

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        CURLcode res = curl_easy_perform(curl_);

        if (header_list) {
            curl_slist_free_all(header_list);
        }

        if (res != CURLE_OK) {
            LOG_ERROR("S3 request failed: {}", curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    RemoteResource parse_s3_object(const json& obj, const ListOptions& options) {
        RemoteResource res;

        if (obj.contains("Key")) {
            std::string key = obj["Key"];
            res.name = key.substr(key.find_last_of('/') + 1);
            res.path = key;
        }

        if (obj.contains("Size")) {
            res.size = obj["Size"];
        }

        if (obj.contains("LastModified")) {
            res.modified_time = obj["LastModified"];
        }

        if (obj.contains("ETag")) {
            res.etag = obj["ETag"];
        }

        if (obj.contains("StorageClass")) {
            res.metadata["storage_class"] = obj["StorageClass"];
        }

        res.type = ResourceType::File;

        return res;
    }

    bool apply_filter(const RemoteResource& res, const ListOptions& options) {
        // 隐藏文件过滤
        if (!options.show_hidden && res.name[0] == '.') {
            return false;
        }

        // 通配符过滤
        if (!options.filter.empty() && !match_wildcard(res.name, options.filter)) {
            return false;
        }

        return true;
    }

    void sort_resources(std::vector<RemoteResource>& resources, const ListOptions& options) {
        if (options.sort_by == "name") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    if (options.sort_desc) {
                        return a.name > b.name;
                    }
                    return a.name < b.name;
                });
        } else if (options.sort_by == "size") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    if (options.sort_desc) {
                        return a.size > b.size;
                    }
                    return a.size < b.size;
                });
        } else if (options.sort_by == "modified_time") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    return a.modified_time < b.modified_time;
                });
        }
    }

    std::string normalize_path(const std::string& path) {
        if (path.empty() || path == "/") return "/";

        std::string result = path;
        if (result.back() != '/') {
            result += "/";
        }

        return result;
    }

    bool test_connection() {
        std::string url = build_s3_url(s3_url_.bucket);
        std::string response = perform_s3_request("GET", url + "?max-keys=1", {});

        return !response.empty();
    }

    std::string get_current_time() {
        time_t now = time(nullptr);
        struct tm* timeinfo = gmtime(&now);
        char buffer[80];
        strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", timeinfo);
        return std::string(buffer);
    }

    std::string get_host_from_url(const std::string& url) {
        size_t host_start = url.find("://") + 3;
        size_t host_end = url.find('/', host_start);
        if (host_end == std::string::npos) {
            host_end = url.length();
        }
        return url.substr(host_start, host_end - host_start);
    }

    bool match_wildcard(const std::string& str, const std::string& pattern) {
        if (pattern == "*") return true;

        size_t pos = pattern.find('*');
        if (pos == std::string::npos) {
            return str == pattern;
        }

        std::string prefix = pattern.substr(0, pos);
        std::string suffix = pattern.substr(pos + 1);

        return str.length() >= prefix.length() + suffix.length() &&
               str.substr(0, prefix.length()) == prefix &&
               str.substr(str.length() - suffix.length()) == suffix;
    }

    std::string url_encode(const std::string& str) {
        std::ostringstream encoded;
        encoded.fill('0');
        encoded << std::hex;

        for (char c : str) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded << c;
            } else {
                encoded << '%' << std::uppercase << std::setw(2)
                        << static_cast<int>(static_cast<unsigned char>(c));
            }
        }

        return encoded.str();
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    CURL* curl_;
    S3Config config_;
    S3Url s3_url_;
    std::string current_path_;
};

// S3Browser 公共接口实现
S3Browser::S3Browser() : p_impl_(std::make_unique<Impl>()) {}

S3Browser::~S3Browser() = default;

std::string S3Browser::get_name() const {
    return "S3";
}

std::vector<std::string> S3Browser::get_supported_protocols() const {
    return {"s3", "s3n", "aws"};
}

bool S3Browser::can_handle(const std::string& url) const {
    return url.find("s3://") == 0 ||
           url.find(".s3.") != std::string::npos ||
           url.find("s3.amazonaws.com") != std::string::npos;
}

bool S3Browser::connect(const std::string& url,
                         const std::map<std::string, std::string>& options) {
    // 解析S3 URL
    p_impl_->s3_url_ = S3UrlParser::parse(url);

    // 设置认证信息
    auto it = options.find("access_key_id");
    if (it != options.end()) {
        p_impl_->config_.access_key_id = it->second;
    }

    it = options.find("secret_access_key");
    if (it != options.end()) {
        p_impl_->config_.secret_access_key = it->second;
    }

    it = options.find("region");
    if (it != options.end()) {
        p_impl_->config_.region = it->second;
        p_impl_->s3_url_.region = it->second;
    }

    it = options.find("endpoint");
    if (it != options.end()) {
        p_impl_->s3_url_.endpoint = it->second;
    }

    // 测试连接
    return p_impl_->test_connection();
}

void S3Browser::disconnect() {
    // S3是无状态协议
}

std::vector<RemoteResource> S3Browser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::vector<RemoteResource> resources;

    // 构建List Objects请求
    std::string list_url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket) + "?list-type=2";

    if (!path.empty() && path != "/") {
        list_url += "&prefix=" + p_impl_->url_encode(path);
        if (path.back() != '/') {
            list_url += "/";
        }
    }

    list_url += "&max-keys=" + std::to_string(options.include_metadata ? 1000 : 100);

    // 发送请求
    std::string response = p_impl_->perform_s3_request("GET", list_url, {});

    if (response.empty()) {
        LOG_ERROR("Failed to list S3 directory%s", "");
        return resources;
    }

    // 解析响应
#ifndef FALCON_BROWSER_NO_JSON
    try {
        json json_response = json::parse(response);

        if (json_response.contains("Contents")) {
            for (const auto& obj : json_response["Contents"]) {
                RemoteResource res = p_impl_->parse_s3_object(obj, options);
                if (p_impl_->apply_filter(res, options)) {
                    resources.push_back(res);
                }
            }
        }

        // 处理CommonPrefixes（子目录）
        if (options.recursive) {
            if (json_response.contains("CommonPrefixes")) {
                for (const auto& prefix : json_response["CommonPrefixes"]) {
                    std::string prefix_name = prefix["Prefix"];
                    if (prefix_name.back() == '/') {
                        prefix_name = prefix_name.substr(0, prefix_name.length() - 1);
                    }

                    // 递归列出子目录
                    std::string sub_dir = prefix_name;
                    if (!sub_dir.empty()) {
                        std::vector<RemoteResource> sub_resources =
                            list_directory(sub_dir, options);
                        resources.insert(resources.end(),
                                   std::make_move_iterator(sub_resources.begin()),
                                   std::make_move_iterator(sub_resources.end()));
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse S3 response: {}", e.what());
    }
#endif

    // 排序
    p_impl_->sort_resources(resources, options);

    return resources;
}

RemoteResource S3Browser::get_resource_info(const std::string& path) {
    RemoteResource info;

    // 使用HEAD请求获取对象信息
    std::string url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket, path);

    std::map<std::string, std::string> headers;
    std::string response = p_impl_->perform_s3_request("HEAD", url, headers);

    if (!response.empty()) {
        // 解析响应头获取信息
        info.path = path;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.type = ResourceType::File;

        // 从响应头提取信息
        auto content_length = headers.find("Content-Length");
        if (content_length != headers.end()) {
            info.size = std::stoull(content_length->second);
        }

        auto last_modified = headers.find("Last-Modified");
        if (last_modified != headers.end()) {
            info.modified_time = last_modified->second;
        }

        auto etag = headers.find("ETag");
        if (etag != headers.end()) {
            info.etag = etag->second;
        }

        auto content_type = headers.find("Content-Type");
        if (content_type != headers.end()) {
            info.mime_type = content_type->second;
        }
    }

    return info;
}

bool S3Browser::create_directory(const std::string& path, bool recursive) {
    // S3使用PUT操作创建目录对象
    std::string dir_path = path;
    if (dir_path.back() != '/') {
        dir_path += "/";
    }

    std::string url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket, dir_path);

    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-directory";
    headers["x-amz-meta-type"] = "directory";

    std::string response = p_impl_->perform_s3_request("PUT", url, headers, "");

    return !response.empty();
}

bool S3Browser::remove(const std::string& path, bool recursive) {
    std::string url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket, path);

    if (recursive) {
        // 递归删除需要先列出所有对象并删除
        ListOptions options;
        options.recursive = true;
        auto resources = list_directory(path.substr(0, path.find_last_of('/')), options);

        // 从最深的对象开始删除
        std::sort(resources.begin(), resources.end(),
            [](const RemoteResource& a, const RemoteResource& b) {
                return a.path > b.path;
            });

        for (const auto& res : resources) {
            std::string obj_url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket, res.path);
            p_impl_->perform_s3_request("DELETE", obj_url, {});
        }
    }

    // 删除指定路径
    std::string response = p_impl_->perform_s3_request("DELETE", url, {});

    return !response.empty();
}

bool S3Browser::rename(const std::string& old_path, const std::string& new_path) {
    // S3不支持直接重命名，需要复制然后删除
    if (copy(old_path, new_path)) {
        return remove(old_path);
    }
    return false;
}

bool S3Browser::copy(const std::string& source_path, const std::string& dest_path) {
    // 使用S3复制API
    std::string source_url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket, source_path);
    std::string dest_url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket, dest_path);

    std::map<std::string, std::string> headers;
    headers["x-amz-copy-source"] = "/" + p_impl_->s3_url_.bucket + "/" + source_path;

    std::string response = p_impl_->perform_s3_request("PUT", dest_url, headers, "");

    return !response.empty();
}

bool S3Browser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string S3Browser::get_current_directory() {
    return p_impl_->current_path_;
}

bool S3Browser::change_directory(const std::string& path) {
    p_impl_->current_path_ = p_impl_->normalize_path(path);
    return true;
}

std::string S3Browser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> S3Browser::get_quota_info() {
    std::map<std::string, uint64_t> quota;

    // 获取存储桶配额信息
    std::string url = p_impl_->build_s3_url(p_impl_->s3_url_.bucket) + "?quota";
    std::string response = p_impl_->perform_s3_request("GET", url, {});

#ifndef FALCON_BROWSER_NO_JSON
    try {
        if (!response.empty()) {
            json json_response = json::parse(response);

            if (json_response.contains("Quota")) {
                auto quota_info = json_response["Quota"];
                if (quota_info.contains("StorageBytes")) {
                    quota["total"] = quota_info["StorageBytes"];
                }
            }
        }
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to parse quota info: {}", e.what());
    }
#endif

    return quota;
}

} // namespace falcon