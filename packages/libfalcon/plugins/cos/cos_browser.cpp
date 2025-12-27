/**
 * @file cos_browser.cpp
 * @brief 腾讯云COS资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/cos_browser.hpp>
#include <falcon/cloud_url_protocols.hpp>
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
#include <openssl/evp.h>

#ifdef FALCON_BROWSER_NO_JSON
#pragma message "Warning: Compiling COS browser without JSON support. Some features may not work."
#endif

using json = nlohmann::json;

namespace falcon {

// COSUrlParser 实现
COSUrl COSUrlParser::parse(const std::string& url) {
    using namespace cloud;

    COSUrl cos_url;

    if (starts_with_protocol(url, PROTOCOL_COS)) {
        // 使用协议常量自动计算偏移量，无需魔法数字！
        size_t bucket_start = PROTOCOL_COS.size();  // 自动 = 6
        size_t bucket_end = url.find('/', bucket_start);

        if (bucket_end == std::string::npos) {
            cos_url.bucket = url.substr(bucket_start);
        } else {
            std::string bucket_part = url.substr(bucket_start, bucket_end - bucket_start);

            // 检查是否包含region
            size_t dash_pos = bucket_part.find('-');
            if (dash_pos != std::string::npos && dash_pos != 0) {
                // 可能是bucket-region格式
                std::string possible_region = bucket_part.substr(dash_pos + 1);
                if (possible_region.find("ap-") == 0 ||
                    possible_region.find("na-") == 0 ||
                    possible_region.find("eu-") == 0) {
                    cos_url.bucket = bucket_part.substr(0, dash_pos);
                    cos_url.region = possible_region;
                } else {
                    cos_url.bucket = bucket_part;
                }
            } else {
                cos_url.bucket = bucket_part;
            }

            cos_url.key = url.substr(bucket_end + 1);
        }
    }

    return cos_url;
}

/**
 * @brief COS浏览器实现细节
 */
class COSBrowser::Impl {
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

    std::string build_cos_url(const std::string& bucket, const std::string& key = "") {
        // COS服务域名格式: {bucket}-{app_id}.cos.{region}.myqcloud.com
        std::string url = "https://" + bucket;
        if (!config_.app_id.empty()) {
            url += "-" + config_.app_id;
        }
        url += ".cos." + cos_url_.region + ".myqcloud.com";

        if (!key.empty()) {
            url += "/" + url_encode(key);
        }
        return url;
    }

    std::string generate_cos_signature(const std::string& method,
                                       const std::string& uri,
                                       const std::map<std::string, std::string>& headers,
                                       const std::string& query_string = "",
                                       const std::string& body = "") {
        // COS签名算法版本3 (TC3-HMAC-SHA256)
        std::string timestamp = std::to_string(time(nullptr));
        std::string date = get_date(timestamp);

        // 1. 拼接CanonicalRequest
        std::string canonical_headers;
        std::string signed_headers;

        // 添加默认headers
        std::map<std::string, std::string> all_headers = headers;
        all_headers["host"] = get_host_from_url(build_cos_url(cos_url_.bucket));
        all_headers["x-tc-action"] = get_cos_action(method, uri);
        all_headers["x-tc-timestamp"] = timestamp;

        // 构建canonical headers
        std::vector<std::string> header_keys;
        for (const auto& [key, value] : all_headers) {
            std::string lower_key = key;
            std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
            header_keys.push_back(lower_key);
        }
        std::sort(header_keys.begin(), header_keys.end());

        for (const auto& key : header_keys) {
            canonical_headers += key + ":" + all_headers.at(key) + "\n";
            if (!signed_headers.empty()) {
                signed_headers += ";";
            }
            signed_headers += key;
        }

        std::string canonical_request =
            method + "\n" +
            uri + "\n" +
            query_string + "\n" +
            canonical_headers + "\n" +
            signed_headers + "\n" +
            sha256_hex(body);

        // 2. 拼接StringToSign
        std::string algorithm = "TC3-HMAC-SHA256";
        std::string credential_scope = date + "/" + cos_url_.region + "/cos/tc3_request";
        std::string hashed_canonical_request = sha256_hex(canonical_request);

        std::string string_to_sign =
            algorithm + "\n" +
            timestamp + "\n" +
            credential_scope + "\n" +
            hashed_canonical_request;

        // 3. 计算签名
        std::string secret_date = hmac_sha256_hex("TC3" + config_.secret_key, date);
        std::string secret_service = hmac_sha256_hex(secret_date, cos_url_.region);
        std::string secret_signing = hmac_sha256_hex(secret_service, "cos");
        std::string signature = hmac_sha256_hex(secret_signing, string_to_sign);

        // 4. 拼接Authorization
        std::string authorization =
            algorithm + " " +
            "Credential=" + config_.secret_id + "/" + credential_scope + ", " +
            "SignedHeaders=" + signed_headers + ", " +
            "Signature=" + signature;

        return authorization;
    }

    std::string perform_cos_request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        const std::string& query_string = "",
        const std::string& body = "") {

        std::string uri = url.substr(url.find('/') + cos_url_.bucket.length() + 7); // 去掉 https://
        uri = uri.substr(uri.find('/'));

        // 生成签名
        std::string authorization = generate_cos_signature(method, uri, headers, query_string, body);

        // 准备请求头
        std::map<std::string, std::string> request_headers = headers;
        request_headers["Authorization"] = authorization;

        // 设置请求头
        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : request_headers) {
            std::string header = key + ": " + value;
            header_list = curl_slist_append(header_list, header.c_str());
        }

        if (header_list) {
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, header_list);
        }

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

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
            FALCON_LOG_ERROR("COS request failed: " << curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    std::string get_cos_action(const std::string& method, const std::string& uri) {
        // 根据不同的API路径返回对应的Action
        if (method == "GET" && uri.find("?list-type") != std::string::npos) {
            return "ListObjects";
        } else if (method == "HEAD") {
            return "HeadObject";
        } else if (method == "PUT") {
            return "PutObject";
        } else if (method == "DELETE") {
            return "DeleteObject";
        }
        return "CosCommonRequest";
    }

    RemoteResource parse_cos_object(const json& obj, const ListOptions& options) {
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

    std::string get_date(const std::string& timestamp) {
        time_t ts = std::stoul(timestamp);
        struct tm* timeinfo = gmtime(&ts);
        char buffer[11];
        strftime(buffer, sizeof(buffer), "%Y-%m-%d", timeinfo);
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

    std::string sha256_hex(const std::string& data) {
        // 使用 OpenSSL 3.0 EVP API
        EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
        if (!mdctx) {
            return "";
        }

        const EVP_MD* md = EVP_sha256();
        if (EVP_DigestInit_ex(mdctx, md, nullptr) != 1) {
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        if (EVP_DigestUpdate(mdctx, data.c_str(), data.length()) != 1) {
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len = 0;
        if (EVP_DigestFinal_ex(mdctx, hash, &hash_len) != 1) {
            EVP_MD_CTX_free(mdctx);
            return "";
        }

        EVP_MD_CTX_free(mdctx);

        std::ostringstream ss;
        ss << std::hex;
        for (unsigned int i = 0; i < hash_len; i++) {
            ss << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

    std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int hash_len;

        HMAC(EVP_sha256(), key.c_str(), key.length(),
             (unsigned char*)data.c_str(), data.length(),
             hash, &hash_len);

        std::ostringstream ss;
        ss << std::hex;
        for (unsigned int i = 0; i < hash_len; i++) {
            ss << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        return ss.str();
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    CURL* curl_;
    COSConfig config_;
    COSUrl cos_url_;
    std::string current_path_;
};

// COSBrowser 公共接口实现
COSBrowser::COSBrowser() : p_impl_(std::make_unique<Impl>()) {}

COSBrowser::~COSBrowser() = default;

std::string COSBrowser::get_name() const {
    return "腾讯云COS";
}

std::vector<std::string> COSBrowser::get_supported_protocols() const {
    return {"cos", "tencent", "qcloud"};
}

bool COSBrowser::can_handle(const std::string& url) const {
    return url.find("cos://") == 0 ||
           url.find("tencent://") == 0 ||
           url.find(".cos.") != std::string::npos ||
           url.find("myqcloud.com") != std::string::npos;
}

bool COSBrowser::connect(const std::string& url,
                         const std::map<std::string, std::string>& options) {
    // 解析COS URL
    p_impl_->cos_url_ = COSUrlParser::parse(url);

    // 设置认证信息
    auto it = options.find("secret_id");
    if (it != options.end()) {
        p_impl_->config_.secret_id = it->second;
    }

    it = options.find("secret_key");
    if (it != options.end()) {
        p_impl_->config_.secret_key = it->second;
    }

    it = options.find("region");
    if (it != options.end()) {
        p_impl_->cos_url_.region = it->second;
    }

    it = options.find("app_id");
    if (it != options.end()) {
        p_impl_->config_.app_id = it->second;
        p_impl_->cos_url_.app_id = it->second;
    }

    it = options.find("token");
    if (it != options.end()) {
        p_impl_->config_.token = it->second;
    }

    // 测试连接
    std::string test_url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket);
    test_url += "?max-keys=1";
    std::string response = p_impl_->perform_cos_request("GET", test_url);

    return !response.empty();
}

void COSBrowser::disconnect() {
    // COS是无状态协议
}

std::vector<RemoteResource> COSBrowser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::vector<RemoteResource> resources;

    // 构建List Objects请求
    std::string url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket);
    std::string query_string = "list-type=2";

    if (!path.empty() && path != "/") {
        query_string += "&prefix=" + p_impl_->url_encode(path);
        if (path.empty() || path.back() != '/') {
            query_string += "/";
        }
    }

    query_string += "&max-keys=" + std::to_string(options.include_metadata ? 1000 : 100);

    // 发送请求
    std::string response = p_impl_->perform_cos_request("GET", url, {}, query_string);

    if (response.empty()) {
        FALCON_LOG_ERROR("Failed to list COS directory");
        return resources;
    }

    // 解析响应
#ifndef FALCON_BROWSER_NO_JSON
    try {
        json json_response = json::parse(response);

        if (json_response.contains("Contents")) {
            for (const auto& obj : json_response["Contents"]) {
                RemoteResource res = p_impl_->parse_cos_object(obj, options);
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
        FALCON_LOG_ERROR("Failed to parse COS response: " << e.what());
    }
#endif

    // 排序
    p_impl_->sort_resources(resources, options);

    return resources;
}

RemoteResource COSBrowser::get_resource_info(const std::string& path) {
    RemoteResource info;

    // 使用HEAD请求获取对象信息
    std::string url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket, path);
    std::string response = p_impl_->perform_cos_request("HEAD", url);

    if (!response.empty() || response != "error") {
        info.path = path;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.type = ResourceType::File;
    }

    return info;
}

bool COSBrowser::create_directory(const std::string& path, bool recursive) {
    // COS使用PUT操作创建目录对象
    std::string dir_path = path;
    if (dir_path.empty() || dir_path.back() != '/') {
        dir_path += "/";
    }

    std::string url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket, dir_path);
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-directory";
    headers["x-cos-meta-type"] = "directory";

    std::string response = p_impl_->perform_cos_request("PUT", url, headers);

    return !response.empty();
}

bool COSBrowser::remove(const std::string& path, bool recursive) {
    std::string url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket, path);

    if (recursive) {
        // 递归删除需要先列出所有对象并删除
        ListOptions options;
        options.recursive = true;
        auto resources = list_directory(path.substr(0, path.find_last_of('/')), options);

        for (const auto& res : resources) {
            if (!res.is_directory()) {
                std::string obj_url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket, res.path);
                p_impl_->perform_cos_request("DELETE", obj_url);
            }
        }
    }

    // 删除指定路径
    std::string response = p_impl_->perform_cos_request("DELETE", url);

    return true; // COS DELETE即使对象不存在也返回204
}

bool COSBrowser::rename(const std::string& old_path, const std::string& new_path) {
    // COS不支持直接重命名，需要复制然后删除
    if (copy(old_path, new_path)) {
        return remove(old_path);
    }
    return false;
}

bool COSBrowser::copy(const std::string& source_path, const std::string& dest_path) {
    std::string url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket, dest_path);
    std::map<std::string, std::string> headers;
    headers["x-cos-copy-source"] = "/" + p_impl_->cos_url_.bucket + "/" + source_path;

    std::string response = p_impl_->perform_cos_request("PUT", url, headers);

    return !response.empty();
}

bool COSBrowser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string COSBrowser::get_current_directory() {
    return p_impl_->current_path_;
}

bool COSBrowser::change_directory(const std::string& path) {
    p_impl_->current_path_ = path;
    return true;
}

std::string COSBrowser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> COSBrowser::get_quota_info() {
    std::map<std::string, uint64_t> quota;

    // COS不直接提供配额信息
    // 可以通过GetBucketStatistics API获取统计信息
    std::string url = p_impl_->build_cos_url(p_impl_->cos_url_.bucket);
    std::string response = p_impl_->perform_cos_request("GET", url + "?statistics");

#ifndef FALCON_BROWSER_NO_JSON
    try {
        if (!response.empty()) {
            json json_response = json::parse(response);
            if (json_response.contains("Size")) {
                quota["used"] = json_response["Size"];
            }
            if (json_response.contains("Count")) {
                quota["object_count"] = json_response["Count"];
            }
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse quota info: " << e.what());
    }
#endif

    return quota;
}

} // namespace falcon