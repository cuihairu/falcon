/**
 * @file kodo_browser.cpp
 * @brief 七牛云Kodo资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/kodo_browser.hpp>
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
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>

#ifdef FALCON_BROWSER_NO_JSON
#pragma message "Warning: Compiling Kodo browser without JSON support. Some features may not work."
#endif

using json = nlohmann::json;

namespace falcon {

// KodoUrlParser 实现
KodoUrl KodoUrlParser::parse(const std::string& url) {
    using namespace cloud;

    KodoUrl kodo_url;

    // 检查支持的协议
    std::string_view protocol;
    if (starts_with_protocol(url, PROTOCOL_KODO)) {
        protocol = PROTOCOL_KODO;
    } else if (starts_with_protocol(url, PROTOCOL_QINIU)) {
        protocol = PROTOCOL_QINIU;
    } else {
        return kodo_url;
    }

    // 使用协议常量自动计算偏移量，无需魔法数字！
    size_t bucket_start = protocol.size();
    size_t bucket_end = url.find('/', bucket_start);

    if (bucket_end == std::string::npos) {
        kodo_url.bucket = url.substr(bucket_start);
    } else {
        kodo_url.bucket = url.substr(bucket_start, bucket_end - bucket_start);
        kodo_url.key = url.substr(bucket_end + 1);
    }

    return kodo_url;
}

/**
 * @brief Kodo浏览器实现细节
 */
class KodoBrowser::Impl {
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

    std::string build_api_url(const std::string& path) {
        // 七牛API基础URL
        std::string url = config_.use_https ? "https://" : "http://";
        url += "rs.qbox.me";

        if (!path.empty()) {
            if (path[0] != '/') {
                url += "/";
            }
            url += path;
        }

        return url;
    }

    std::string build_rsf_url(const std::string& path) {
        // 七牛RSF API基础URL（用于列举）
        std::string url = config_.use_https ? "https://" : "http://";
        url += "rsf.qbox.me";

        if (!path.empty()) {
            if (path[0] != '/') {
                url += "/";
            }
            url += path;
        }

        return url;
    }

    std::string encode_base64_url(const std::string& str) {
        // Base64 URL安全编码
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_write(b64, str.c_str(), str.length());
        BIO_flush(b64);

        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);

        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);

        // 移除填充并替换字符
        while (!result.empty() && result.back() == '=') {
            result.pop_back();
        }
        std::replace(result.begin(), result.end(), '+', '-');
        std::replace(result.begin(), result.end(), '/', '_');

        return result;
    }

    std::string generate_qiniu_token(const std::string& url, const std::string& body = "") {
        // 生成七牛认证token
        std::string timestamp = std::to_string(time(nullptr));
        std::string nonce = generate_random_string(16);

        // 构建待签名字符串
        std::string sign_str = url + "\n" + body;

        // HMAC-SHA1签名
        unsigned char hmac[EVP_MAX_MD_SIZE];
        unsigned int hmac_len;
        HMAC(EVP_sha1(), config_.secret_key.c_str(), config_.secret_key.length(),
             (unsigned char*)sign_str.c_str(), sign_str.length(),
             hmac, &hmac_len);

        // Base64编码签名
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_write(b64, hmac, hmac_len);
        BIO_flush(b64);

        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string encoded_sign(bptr->data, bptr->length - 1);
        BIO_free_all(b64);

        // 构建token
        std::string token = "Qiniu " + config_.access_key + ":" + encoded_sign;
        return token;
    }

    std::string perform_kodo_request(
        const std::string& method,
        const std::string& url,
        const std::string& body = "",
        bool is_rsf = false) {

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

        // 生成认证token
        std::string token = generate_qiniu_token(url, body);

        // 设置请求头
        curl_slist* header_list = nullptr;
        std::string auth_header = "Authorization: " + token;
        header_list = curl_slist_append(header_list, auth_header.c_str());
        header_list = curl_slist_append(header_list, "Content-Type: application/json");

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
            FALCON_LOG_ERROR("Kodo request failed: " << curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    RemoteResource parse_kodo_object(const json& obj, const ListOptions& options) {
        RemoteResource res;

        if (obj.contains("key")) {
            std::string key = obj["key"];
            res.name = key.substr(key.find_last_of('/') + 1);
            res.path = key;
        }

        if (obj.contains("fsize")) {
            res.size = obj["fsize"];
        }

        if (obj.contains("putTime")) {
            int64_t put_time = obj["putTime"];
            time_t timestamp = put_time / 10000; // 转换为秒
            res.modified_time = std::to_string(timestamp);
        }

        if (obj.contains("hash")) {
            res.etag = obj["hash"];
        }

        if (obj.contains("mimeType")) {
            res.mime_type = obj["mimeType"];
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

    std::string generate_random_string(size_t length) {
        const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        std::string result;
        result.reserve(length);

        for (size_t i = 0; i < length; ++i) {
            result += charset[rand() % (sizeof(charset) - 1)];
        }

        return result;
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

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    CURL* curl_;
    KodoConfig config_;
    KodoUrl kodo_url_;
    std::string current_path_;
};

// KodoBrowser 公共接口实现
KodoBrowser::KodoBrowser() : p_impl_(std::make_unique<Impl>()) {}

KodoBrowser::~KodoBrowser() = default;

std::string KodoBrowser::get_name() const {
    return "七牛云Kodo";
}

std::vector<std::string> KodoBrowser::get_supported_protocols() const {
    return {"kodo", "qiniu", "qn"};
}

bool KodoBrowser::can_handle(const std::string& url) const {
    return url.find("kodo://") == 0 ||
           url.find("qiniu://") == 0 ||
           url.find("qn://") == 0;
}

bool KodoBrowser::connect(const std::string& url,
                         const std::map<std::string, std::string>& options) {
    // 解析Kodo URL
    p_impl_->kodo_url_ = KodoUrlParser::parse(url);

    // 设置认证信息
    auto it = options.find("access_key");
    if (it != options.end()) {
        p_impl_->config_.access_key = it->second;
    }

    it = options.find("secret_key");
    if (it != options.end()) {
        p_impl_->config_.secret_key = it->second;
    }

    it = options.find("domain");
    if (it != options.end()) {
        p_impl_->config_.domain = it->second;
    }

    it = options.find("https");
    if (it != options.end()) {
        p_impl_->config_.use_https = (it->second == "true" || it->second == "1");
    }

    // 测试连接
    std::string test_url = p_impl_->build_api_url("stat/" +
        p_impl_->encode_base64_url(p_impl_->kodo_url_.bucket + ":test"));
    std::string response = p_impl_->perform_kodo_request("GET", test_url);

    // 七牛API即使对象不存在也返回JSON，所以只要不是空就算成功
    return !response.empty();
}

void KodoBrowser::disconnect() {
    // Kodo是无状态协议
}

std::vector<RemoteResource> KodoBrowser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::vector<RemoteResource> resources;

    // 构建列举请求
    std::string list_url = p_impl_->build_rsf_url("list");
    std::string bucket_encoded = p_impl_->encode_base64_url(p_impl_->kodo_url_.bucket);

    // 构建请求体
    json request_body = {
        {"bucket", bucket_encoded}
    };

    if (!path.empty() && path != "/") {
        request_body["prefix"] = path;
        if (path.empty() || path.back() != '/') {
            request_body["prefix"] = path + "/";
        }
    }

    if (options.include_metadata) {
        request_body["limit"] = 1000;
    } else {
        request_body["limit"] = 100;
    }

    std::string body = request_body.dump();
    std::string response = p_impl_->perform_kodo_request("POST", list_url, body, true);

    if (response.empty()) {
        FALCON_LOG_ERROR("Failed to list Kodo directory");
        return resources;
    }

    // 解析响应
#ifndef FALCON_BROWSER_NO_JSON
    try {
        json json_response = json::parse(response);

        if (json_response.contains("items")) {
            for (const auto& obj : json_response["items"]) {
                RemoteResource res = p_impl_->parse_kodo_object(obj, options);
                if (p_impl_->apply_filter(res, options)) {
                    resources.push_back(res);
                }
            }
        }

        // 处理目录（需要根据key的/分隔符判断）
        if (options.recursive) {
            std::map<std::string, bool> directories;
            for (const auto& res : resources) {
                size_t slash_pos = res.path.find('/');
                if (slash_pos != std::string::npos && slash_pos > 0) {
                    std::string dir_name = res.path.substr(0, slash_pos);
                    if (directories.find(dir_name) == directories.end()) {
                        RemoteResource dir_res;
                        dir_res.name = dir_name;
                        dir_res.path = dir_name;
                        dir_res.type = ResourceType::Directory;
                        if (p_impl_->apply_filter(dir_res, options)) {
                            resources.insert(resources.begin(), dir_res);
                        }
                        directories[dir_name] = true;
                    }
                }
            }
        }

    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse Kodo response: " << e.what());
    }
#endif

    // 排序
    p_impl_->sort_resources(resources, options);

    return resources;
}

RemoteResource KodoBrowser::get_resource_info(const std::string& path) {
    RemoteResource info;

    // 使用stat API获取对象信息
    std::string entry = p_impl_->kodo_url_.bucket + ":" + path;
    std::string entry_encoded = p_impl_->encode_base64_url(entry);
    std::string url = p_impl_->build_api_url("stat/" + entry_encoded);
    std::string response = p_impl_->perform_kodo_request("GET", url);

    if (!response.empty()) {
        info.path = path;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.type = ResourceType::File;

#ifndef FALCON_BROWSER_NO_JSON
        try {
            json json_response = json::parse(response);
            if (json_response.contains("fsize")) {
                info.size = json_response["fsize"];
            }
            if (json_response.contains("hash")) {
                info.etag = json_response["hash"];
            }
            if (json_response.contains("putTime")) {
                int64_t put_time = json_response["putTime"];
                time_t timestamp = put_time / 10000;
                info.modified_time = std::to_string(timestamp);
            }
            if (json_response.contains("mimeType")) {
                info.mime_type = json_response["mimeType"];
            }
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR("Failed to parse stat response: " << e.what());
        }
#endif
    }

    return info;
}

bool KodoBrowser::create_directory(const std::string& path, bool recursive) {
    // 七牛云没有真正的目录概念，可以通过创建空对象模拟目录
    std::string dir_path = path;
    if (dir_path.empty() || dir_path.back() != '/') {
        dir_path += "/";
    }

    // 创建一个空对象作为目录标识
    std::string url = p_impl_->build_rsf_url("put");
    json request_body = {
        {"bucket", p_impl_->kodo_url_.bucket},
        {"key", dir_path},
        {"overwrite", true}
    };

    // 七牛需要生成上传token，这里简化处理
    std::string body = request_body.dump();
    std::string response = p_impl_->perform_kodo_request("POST", url, body, true);

    return !response.empty();
}

bool KodoBrowser::remove(const std::string& path, bool recursive) {
    std::string url = p_impl_->build_api_url("delete");

    if (recursive) {
        // 递归删除需要先列出所有对象并删除
        ListOptions options;
        options.recursive = true;
        auto resources = list_directory(path.substr(0, path.find_last_of('/')), options);

        for (const auto& res : resources) {
            if (!res.is_directory()) {
                json delete_body = {
                    {"bucket", p_impl_->kodo_url_.bucket},
                    {"key", res.path}
                };
                std::string delete_req = delete_body.dump();
                p_impl_->perform_kodo_request("POST", url, delete_req);
            }
        }
    }

    // 删除指定路径
    json delete_body = {
        {"bucket", p_impl_->kodo_url_.bucket},
        {"key", path}
    };
    std::string body = delete_body.dump();
    std::string response = p_impl_->perform_kodo_request("POST", url, body);

    return !response.empty();
}

bool KodoBrowser::rename(const std::string& old_path, const std::string& new_path) {
    // 七牛云不支持直接重命名，需要复制然后删除
    if (copy(old_path, new_path)) {
        return remove(old_path);
    }
    return false;
}

bool KodoBrowser::copy(const std::string& source_path, const std::string& dest_path) {
    std::string url = p_impl_->build_api_url("copy");

    json copy_body = {
        {"src_bucket", p_impl_->kodo_url_.bucket},
        {"src_key", source_path},
        {"dest_bucket", p_impl_->kodo_url_.bucket},
        {"dest_key", dest_path},
        {"force", true}
    };

    std::string body = copy_body.dump();
    std::string response = p_impl_->perform_kodo_request("POST", url, body);

    return !response.empty();
}

bool KodoBrowser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string KodoBrowser::get_current_directory() {
    return p_impl_->current_path_;
}

bool KodoBrowser::change_directory(const std::string& path) {
    p_impl_->current_path_ = path;
    return true;
}

std::string KodoBrowser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> KodoBrowser::get_quota_info() {
    std::map<std::string, uint64_t> quota;

    // 七牛云需要通过空间统计API获取配额信息
    std::string url = p_impl_->build_api_url("v2/domain/" + p_impl_->kodo_url_.bucket);
    std::string response = p_impl_->perform_kodo_request("GET", url);

#ifndef FALCON_BROWSER_NO_JSON
    try {
        if (!response.empty()) {
            json json_response = json::parse(response);
            if (json_response.contains("bytes")) {
                quota["used"] = json_response["bytes"];
            }
            if (json_response.contains("count")) {
                quota["object_count"] = json_response["count"];
            }
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse quota info: " << e.what());
    }
#endif

    return quota;
}

} // namespace falcon