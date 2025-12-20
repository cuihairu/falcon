/**
 * @file oss_browser.cpp
 * @brief 阿里云OSS资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/oss_browser.hpp>
#include <falcon/logger.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cstring>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#ifdef FALCON_BROWSER_NO_JSON
#pragma message "Warning: Compiling OSS browser without JSON support. Some features may not work."
#endif

using json = nlohmann::json;

namespace falcon {

// OSSUrlParser 实现
OSSUrl OSSUrlParser::parse(const std::string& url) {
    OSSUrl oss_url;

    if (url.find("oss://") == 0) {
        // oss://bucket/key 或 oss://bucket.endpoint/key
        size_t bucket_start = 5; // skip "oss://"
        size_t bucket_end = url.find('/', bucket_start);

        if (bucket_end == std::string::npos) {
            oss_url.bucket = url.substr(bucket_start);
        } else {
            std::string bucket_part = url.substr(bucket_start, bucket_end - bucket_start);

            // 检查是否包含endpoint
            size_t dot_pos = bucket_part.find('.');
            if (dot_pos != std::string::npos) {
                oss_url.bucket = bucket_part.substr(0, dot_pos);
                oss_url.endpoint = bucket_part.substr(dot_pos + 1);

                // 从endpoint提取region
                if (oss_url.endpoint.find("oss-") == 0) {
                    size_t region_end = oss_url.endpoint.find(".aliyuncs.com");
                    if (region_end != std::string::npos) {
                        oss_url.region = oss_url.endpoint.substr(4, region_end - 4);
                    }
                }
            } else {
                oss_url.bucket = bucket_part;
                // 使用默认endpoint格式
                oss_url.endpoint = "oss-" + oss_url.region + ".aliyuncs.com";
            }

            oss_url.key = url.substr(bucket_end + 1);
        }
    }

    return oss_url;
}

/**
 * @brief OSS浏览器实现细节
 */
class OSSBrowser::Impl {
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

    std::string build_oss_url(const std::string& bucket, const std::string& key = "") {
        std::string url = "https://" + bucket + "." + oss_url_.endpoint;
        if (!key.empty()) {
            url += "/" + url_encode(key);
        }
        return url;
    }

    std::string generate_signature(const std::string& method, const std::string& uri,
                                  const std::map<std::string, std::string>& headers,
                                  const std::string& query_string = "") {
        // 构建待签名字符串
        std::string canonical_resource = "/" + oss_url_.bucket + uri;
        if (!query_string.empty()) {
            canonical_resource += "?" + query_string;
        }

        std::string canonical_headers;
        std::map<std::string, std::string> lower_headers;

        // 转换headers为小写并构建canonical headers
        for (const auto& [key, value] : headers) {
            std::string lower_key = key;
            std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
            lower_headers[lower_key] = value;
        }

        // 按字母顺序排序headers
        std::vector<std::string> sorted_keys;
        for (const auto& [key, value] : lower_headers) {
            sorted_keys.push_back(key);
        }
        std::sort(sorted_keys.begin(), sorted_keys.end());

        // 构建canonical headers string
        for (const auto& key : sorted_keys) {
            canonical_headers += key + ":" + lower_headers[key] + "\n";
        }

        // 构建string-to-sign
        std::string date = get_gmt_time();
        std::string string_to_sign =
            method + "\n" +
            "\n" +  // Content-MD5
            "\n" +  // Content-Type
            date + "\n" +
            canonical_headers +
            canonical_resource;

        // 生成签名
        return hmac_sha1_base64(config_.access_key_secret, string_to_sign);
    }

    std::string perform_oss_request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        const std::string& query_string = "",
        const std::string& body = "") {

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

        // 准备请求头
        std::map<std::string, std::string> request_headers = headers;

        // 添加必要的headers
        std::string date = get_gmt_time();
        request_headers["Date"] = date;
        request_headers["Host"] = get_host_from_url(url);

        // 生成签名
        std::string uri = url.substr(url.find('/') + oss_url_.bucket.length() + 1);
        std::string signature = generate_signature(method, uri, request_headers, query_string);
        request_headers["Authorization"] = "OSS " + config_.access_key_id + ":" + signature;

        // 设置请求头
        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : request_headers) {
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
            Logger::error("OSS request failed: {}", curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    RemoteResource parse_oss_object(const json& obj, const ListOptions& options) {
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
        if (!options.show_hidden && res.name[0] == '.') {
            return false;
        }

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
        }
    }

    std::string get_gmt_time() {
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

    std::string hmac_sha1_base64(const std::string& key, const std::string& data) {
        unsigned char hmac[EVP_MAX_MD_SIZE];
        unsigned int hmac_len;

        HMAC(EVP_sha1(), key.c_str(), key.length(),
             (unsigned char*)data.c_str(), data.length(),
             hmac, &hmac_len);

        // Base64编码
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_write(b64, hmac, hmac_len);
        BIO_flush(b64);

        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);

        std::string result(bptr->data, bptr->length - 1); // 去掉换行符
        BIO_free_all(b64);

        return result;
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    CURL* curl_;
    OSSConfig config_;
    OSSUrl oss_url_;
    std::string current_path_;
};

// OSSBrowser 公共接口实现
OSSBrowser::OSSBrowser() : p_impl_(std::make_unique<Impl>()) {}

OSSBrowser::~OSSBrowser() = default;

std::string OSSBrowser::get_name() const {
    return "阿里云OSS";
}

std::vector<std::string> OSSBrowser::get_supported_protocols() const {
    return {"oss", "aliyun", "oss-cn"};
}

bool OSSBrowser::can_handle(const std::string& url) const {
    return url.find("oss://") == 0 ||
           url.find("aliyun://") == 0 ||
           url.find(".oss-") != std::string::npos ||
           url.find("oss-aliyuncs.com") != std::string::npos;
}

bool OSSBrowser::connect(const std::string& url,
                         const std::map<std::string, std::string>& options) {
    // 解析OSS URL
    p_impl_->oss_url_ = OSSUrlParser::parse(url);

    // 设置认证信息
    auto it = options.find("access_key_id");
    if (it != options.end()) {
        p_impl_->config_.access_key_id = it->second;
    }

    it = options.find("access_key_secret");
    if (it != options.end()) {
        p_impl_->config_.access_key_secret = it->second;
    }

    it = options.find("endpoint");
    if (it != options.end()) {
        p_impl_->oss_url_.endpoint = it->second;
    }

    it = options.find("region");
    if (it != options.end()) {
        p_impl_->oss_url_.region = it->second;
        if (p_impl_->oss_url_.endpoint.empty()) {
            p_impl_->oss_url_.endpoint = "oss-" + it->second + ".aliyuncs.com";
        }
    }

    it = options.find("security_token");
    if (it != options.end()) {
        p_impl_->config_.security_token = it->second;
    }

    // 测试连接
    std::string test_url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket) + "?max-keys=1";
    std::string response = p_impl_->perform_oss_request("GET", test_url);

    return !response.empty();
}

void OSSBrowser::disconnect() {
    // OSS是无状态协议
}

std::vector<RemoteResource> OSSBrowser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::vector<RemoteResource> resources;

    // 构建List Objects请求
    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket);
    std::string query_string = "list-type=2";

    if (!path.empty() && path != "/") {
        query_string += "&prefix=" + p_impl_->url_encode(path);
        if (!path.ends_with('/')) {
            query_string += "/";
        }
    }

    query_string += "&max-keys=" + std::to_string(options.include_metadata ? 1000 : 100);

    // 发送请求
    std::string response = p_impl_->perform_oss_request("GET", url, {}, query_string);

    if (response.empty()) {
        Logger::error("Failed to list OSS directory");
        return resources;
    }

    // 解析响应
#ifndef FALCON_BROWSER_NO_JSON
    try {
        json json_response = json::parse(response);

        if (json_response.contains("Contents")) {
            for (const auto& obj : json_response["Contents"]) {
                RemoteResource res = p_impl_->parse_oss_object(obj, options);
                if (p_impl_->apply_filter(res, options)) {
                    resources.push_back(res);
                }
            }
        }

        // 处理CommonPrefixes（子目录）
        if (json_response.contains("CommonPrefixes")) {
            for (const auto& prefix : json_response["CommonPrefixes"]) {
                std::string prefix_name = prefix["Prefix"];
                if (prefix_name.back() == '/') {
                    prefix_name = prefix_name.substr(0, prefix_name.length() - 1);
                }

                RemoteResource dir_res;
                dir_res.name = prefix_name.substr(prefix_name.find_last_of('/') + 1);
                dir_res.path = prefix_name;
                dir_res.type = ResourceType::Directory;

                if (p_impl_->apply_filter(dir_res, options)) {
                    resources.push_back(dir_res);
                }
            }
        }

    } catch (const std::exception& e) {
        Logger::error("Failed to parse OSS response: {}", e.what());
    }
#endif

    // 排序
    p_impl_->sort_resources(resources, options);

    return resources;
}

RemoteResource OSSBrowser::get_resource_info(const std::string& path) {
    RemoteResource info;

    // 使用HEAD请求获取对象信息
    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket, path);
    std::string response = p_impl_->perform_oss_request("HEAD", url);

    if (!response.empty() || response != "error") {
        // 解析响应头获取信息
        info.path = path;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.type = ResourceType::File;

        // 从响应头提取信息需要通过curl获取
        // 这里简化处理
    }

    return info;
}

bool OSSBrowser::create_directory(const std::string& path, bool recursive) {
    // OSS使用PUT操作创建目录对象
    std::string dir_path = path;
    if (!dir_path.ends_with('/')) {
        dir_path += "/";
    }

    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket, dir_path);
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-directory";
    headers["x-oss-meta-type"] = "directory";

    std::string response = p_impl_->perform_oss_request("PUT", url, headers);

    return !response.empty();
}

bool OSSBrowser::remove(const std::string& path, bool recursive) {
    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket, path);

    if (recursive) {
        // 递归删除需要先列出所有对象并删除
        ListOptions options;
        options.recursive = true;
        auto resources = list_directory(path.substr(0, path.find_last_of('/')), options);

        for (const auto& res : resources) {
            if (!res.is_directory()) {
                std::string obj_url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket, res.path);
                p_impl_->perform_oss_request("DELETE", obj_url);
            }
        }
    }

    // 删除指定路径
    std::string response = p_impl_->perform_oss_request("DELETE", url);

    return true; // OSS DELETE即使对象不存在也返回204
}

bool OSSBrowser::rename(const std::string& old_path, const std::string& new_path) {
    // OSS不支持直接重命名，需要复制然后删除
    if (copy(old_path, new_path)) {
        return remove(old_path);
    }
    return false;
}

bool OSSBrowser::copy(const std::string& source_path, const std::string& dest_path) {
    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket, dest_path);
    std::map<std::string, std::string> headers;
    headers["x-oss-copy-source"] = "/" + p_impl_->oss_url_.bucket + "/" + source_path;

    std::string response = p_impl_->perform_oss_request("PUT", url, headers);

    return !response.empty();
}

bool OSSBrowser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string OSSBrowser::get_current_directory() {
    return p_impl_->current_path_;
}

bool OSSBrowser::change_directory(const std::string& path) {
    p_impl_->current_path_ = path;
    return true;
}

std::string OSSBrowser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> OSSBrowser::get_quota_info() {
    std::map<std::string, uint64_t> quota;

    // OSS不直接提供配额信息
    // 可以通过GetBucketStat API获取统计信息
    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket);
    std::map<std::string, std::string> headers;
    headers["x-oss-action"] = "GetBucketStat";

    std::string response = p_impl_->perform_oss_request("GET", url, headers);

#ifndef FALCON_BROWSER_NO_JSON
    try {
        if (!response.empty()) {
            json json_response = json::parse(response);
            if (json_response.contains("StorageSize")) {
                quota["used"] = json_response["StorageSize"];
            }
            if (json_response.contains("ObjectCount")) {
                quota["object_count"] = json_response["ObjectCount"];
            }
        }
    } catch (const std::exception& e) {
        Logger::error("Failed to parse quota info: {}", e.what());
    }
#endif

    return quota;
}

} // namespace falcon