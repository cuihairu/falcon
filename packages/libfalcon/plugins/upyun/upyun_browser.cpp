/**
 * @file upyun_browser.cpp
 * @brief 又拍云USS资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/upyun_browser.hpp>
#include <falcon/cloud_url_protocols.hpp>
#include <falcon/logger.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <iomanip>
#include <cstring>
#include <openssl/md5.h>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

#ifdef FALCON_BROWSER_NO_JSON
#pragma message "Warning: Compiling Upyun browser without JSON support. Some features may not work."
#endif

using json = nlohmann::json;

namespace falcon {

// UpyunUrlParser 实现
UpyunUrl UpyunUrlParser::parse(const std::string& url) {
    using namespace cloud;

    UpyunUrl upyun_url;

    if (starts_with_protocol(url, PROTOCOL_UPYUN)) {
        // 使用协议常量自动计算偏移量，无需魔法数字！
        size_t bucket_start = PROTOCOL_UPYUN.size();  // 自动 = 8
        size_t bucket_end = url.find('/', bucket_start);

        if (bucket_end == std::string::npos) {
            upyun_url.bucket = url.substr(bucket_start);
        } else {
            upyun_url.bucket = url.substr(bucket_start, bucket_end - bucket_start);
            upyun_url.key = url.substr(bucket_end + 1);
        }
    }

    return upyun_url;
}

/**
 * @brief 又拍云浏览器实现细节
 */
class UpyunBrowser::Impl {
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

    std::string build_upyun_url(const std::string& path) {
        // 又拍云REST API URL
        std::string url = "https://" + upyun_url_.bucket + "." + config_.api_domain;
        if (!path.empty()) {
            if (path[0] != '/') {
                url += "/";
            }
            url += path;
        }
        return url;
    }

    std::string generate_upyun_signature(const std::string& method,
                                          const std::string& uri,
                                          const std::string& date,
                                          const std::string& password) {
        // 又拍云签名算法
        std::string sign_str = method + "&" + uri + "&" + date;

        // MD5加密密码
        unsigned char md5[MD5_DIGEST_LENGTH];
        MD5((unsigned char*)password.c_str(), password.length(), md5);

        // 转换为十六进制字符串
        char md5_str[33];
        for (int i = 0; i < 16; i++) {
            sprintf(md5_str + i * 2, "%02x", md5[i]);
        }
        md5_str[32] = '\0';

        std::string password_md5 = md5_str;

        // HMAC-MD5签名（兼容 OpenSSL 3.0）
        unsigned char hmac[MD5_DIGEST_LENGTH];
        unsigned int hmac_len = 0;

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        // OpenSSL 3.0+
        HMAC_CTX* hmac_ctx = HMAC_CTX_new();
        HMAC_Init_ex(hmac_ctx, password_md5.c_str(), password_md5.length(), EVP_md5(), nullptr);
        HMAC_Update(hmac_ctx, (unsigned char*)sign_str.c_str(), sign_str.length());
        HMAC_Final(hmac_ctx, hmac, &hmac_len);
        HMAC_CTX_free(hmac_ctx);
#else
        // OpenSSL 1.x
        HMAC_CTX hmac_ctx;
        HMAC_CTX_init(&hmac_ctx);
        HMAC_Init(&hmac_ctx, password_md5.c_str(), password_md5.length(), EVP_md5());
        HMAC_Update(&hmac_ctx, (unsigned char*)sign_str.c_str(), sign_str.length());
        HMAC_Final(&hmac_ctx, hmac, &hmac_len);
        HMAC_CTX_cleanup(&hmac_ctx);
#endif

        // Base64编码
        std::string result = base64_encode(hmac, hmac_len);
        return result;
    }

    std::string perform_upyun_request(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& headers = {},
        const std::string& body = "") {

        std::string url = build_upyun_url(uri);

        // 生成日期
        std::string date = get_gmt_time();

        // 生成签名
        std::string signature = generate_upyun_signature(method, "/" + upyun_url_.bucket + uri, date, config_.password);

        // 准备请求头
        curl_slist* header_list = nullptr;

        // 添加认证头
        std::string auth_header = "Authorization: UPYUN " + config_.username + ":" + signature;
        header_list = curl_slist_append(header_list, auth_header.c_str());

        // 添加日期头
        std::string date_header = "Date: " + date;
        header_list = curl_slist_append(header_list, date_header.c_str());

        // 添加自定义headers
        for (const auto& [key, value] : headers) {
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
            FALCON_LOG_ERROR("Upyun request failed: " << curl_easy_strerror(res));
            return "";
        }

        return response;
    }

    RemoteResource parse_upyun_object(const json& obj, const ListOptions& options) {
        RemoteResource res;

        if (obj.contains("name")) {
            std::string name = obj["name"];
            res.name = name;
            res.path = name;
        }

        if (obj.contains("size")) {
            res.size = obj["size"];
        }

        if (obj.contains("type")) {
            std::string type = obj["type"];
            if (type == "folder") {
                res.type = ResourceType::Directory;
            } else {
                res.type = ResourceType::File;
            }
        }

        if (obj.contains("last_modified")) {
            res.modified_time = obj["last_modified"];
        }

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

    std::string base64_encode(const unsigned char* data, size_t length) {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_write(b64, data, length);
        BIO_flush(b64);

        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length - 1);
        BIO_free_all(b64);

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
    UpyunConfig config_;
    UpyunUrl upyun_url_;
    std::string current_path_;
};

// UpyunBrowser 公共接口实现
UpyunBrowser::UpyunBrowser() : p_impl_(std::make_unique<Impl>()) {}

UpyunBrowser::~UpyunBrowser() = default;

std::string UpyunBrowser::get_name() const {
    return "又拍云USS";
}

std::vector<std::string> UpyunBrowser::get_supported_protocols() const {
    return {"upyun", "upaiyun"};
}

bool UpyunBrowser::can_handle(const std::string& url) const {
    return url.find("upyun://") == 0 ||
           url.find("upaiyun://") == 0;
}

bool UpyunBrowser::connect(const std::string& url,
                         const std::map<std::string, std::string>& options) {
    // 解析又拍云URL
    p_impl_->upyun_url_ = UpyunUrlParser::parse(url);

    // 设置认证信息
    auto it = options.find("username");
    if (it != options.end()) {
        p_impl_->config_.username = it->second;
    }

    it = options.find("password");
    if (it != options.end()) {
        p_impl_->config_.password = it->second;
    }

    it = options.find("bucket");
    if (it != options.end()) {
        p_impl_->config_.bucket = it->second;
        p_impl_->upyun_url_.bucket = it->second;
    }

    it = options.find("domain");
    if (it != options.end()) {
        p_impl_->config_.domain = it->second;
    }

    it = options.find("api_domain");
    if (it != options.end()) {
        p_impl_->config_.api_domain = it->second;
    } else {
        p_impl_->config_.api_domain = "v0.api.upyun.com";
    }

    // 测试连接
    std::string response = p_impl_->perform_upyun_request("GET", "/usage/");

    // 又拍云API返回格式为JSON，只要不是空就算成功
    return !response.empty();
}

void UpyunBrowser::disconnect() {
    // 又拍云是无状态协议
}

std::vector<RemoteResource> UpyunBrowser::list_directory(
    const std::string& path,
    const ListOptions& options) {

    std::vector<RemoteResource> resources;

    // 构建列举请求
    std::string uri = path;
    if (uri.empty() || uri == "/") {
        uri = "/";
    } else {
        if (uri[0] != '/') {
            uri = "/" + uri;
        }
        if (uri.back() != '/') {
            uri += "/";
        }
    }

    // 又拍云的x-list-limit最大10000
    std::map<std::string, std::string> headers;
    headers["x-list-limit"] = std::to_string(options.include_metadata ? 1000 : 100);
    headers["x-list-order"] = options.sort_desc ? "desc" : "asc";

    if (options.sort_by == "size") {
        headers["x-list-order"] = std::string("size:") + (options.sort_desc ? "desc" : "asc");
    } else if (options.sort_by == "modified_time") {
        headers["x-list-order"] = std::string("time:") + (options.sort_desc ? "desc" : "asc");
    }

    std::string response = p_impl_->perform_upyun_request("GET", uri, headers);

    if (response.empty()) {
        FALCON_LOG_ERROR("Failed to list Upyun directory");
        return resources;
    }

    // 解析响应（又拍云返回格式为文本，每行一个文件/目录）
    std::istringstream iss(response);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.empty()) continue;

        RemoteResource res;

        // 解析行格式: name\tsize\ttype\ttime
        std::istringstream line_ss(line);
        std::string token;

        if (std::getline(line_ss, token, '\t')) {
            res.name = token;
            res.path = (uri == "/" ? "" : uri) + token;
        }

        if (std::getline(line_ss, token, '\t') && !token.empty()) {
            try {
                res.size = std::stoull(token);
                res.type = ResourceType::File;
            } catch (...) {
                // 如果不是数字，可能是文件夹
                res.type = ResourceType::Directory;
            }
        }

        if (std::getline(line_ss, token, '\t')) {
            if (token == "N") {
                res.type = ResourceType::Directory;
            } else {
                res.type = ResourceType::File;
            }
        }

        if (std::getline(line_ss, token, '\t')) {
            res.modified_time = token;
        }

        if (p_impl_->apply_filter(res, options)) {
            resources.push_back(res);
        }
    }

    // 递归列出子目录
    if (options.recursive) {
        for (const auto& res : resources) {
            if (res.is_directory() && res.name != "." && res.name != "..") {
                std::string sub_path = res.path;
                if (sub_path.back() == '/') {
                    sub_path.pop_back();
                }

                std::vector<RemoteResource> sub_resources =
                    list_directory(sub_path, options);

                resources.insert(resources.end(),
                    std::make_move_iterator(sub_resources.begin()),
                    std::make_move_iterator(sub_resources.end()));
            }
        }
    }

    return resources;
}

RemoteResource UpyunBrowser::get_resource_info(const std::string& path) {
    RemoteResource info;

    // 使用HEAD请求获取对象信息
    std::string uri = path;
    if (uri[0] != '/') {
        uri = "/" + uri;
    }

    std::string response = p_impl_->perform_upyun_request("HEAD", uri);

    if (!response.empty() || response != "error") {
        info.path = path;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.type = ResourceType::File;

        // HEAD请求的响应在headers中，这里简化处理
    }

    return info;
}

bool UpyunBrowser::create_directory(const std::string& path, bool recursive) {
    std::string uri = path;
    if (uri[0] != '/') {
        uri = "/" + uri;
    }
    if (uri.empty() || uri.back() != '/') {
        uri += "/";
    }

    std::map<std::string, std::string> headers;
    headers["folder"] = "true";

    std::string response = p_impl_->perform_upyun_request("POST", uri, headers);

    return !response.empty();
}

bool UpyunBrowser::remove(const std::string& path, bool recursive) {
    std::string uri = path;
    if (uri[0] != '/') {
        uri = "/" + uri;
    }

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
            if (!res.is_directory()) {
                std::string obj_uri = "/" + res.path;
                p_impl_->perform_upyun_request("DELETE", obj_uri);
            }
        }
    }

    // 删除指定路径
    std::string response = p_impl_->perform_upyun_request("DELETE", uri);

    return !response.empty();
}

bool UpyunBrowser::rename(const std::string& old_path, const std::string& new_path) {
    // 又拍云不支持直接重命名，需要复制然后删除
    if (copy(old_path, new_path)) {
        return remove(old_path);
    }
    return false;
}

bool UpyunBrowser::copy(const std::string& source_path, const std::string& dest_path) {
    std::string uri = dest_path;
    if (uri[0] != '/') {
        uri = "/" + uri;
    }

    std::map<std::string, std::string> headers;
    headers["x-upyun-copy-source"] = "/" + p_impl_->upyun_url_.bucket + "/" + source_path;

    std::string response = p_impl_->perform_upyun_request("PUT", uri, headers);

    return !response.empty();
}

bool UpyunBrowser::exists(const std::string& path) {
    RemoteResource info = get_resource_info(path);
    return !info.name.empty();
}

std::string UpyunBrowser::get_current_directory() {
    return p_impl_->current_path_;
}

bool UpyunBrowser::change_directory(const std::string& path) {
    p_impl_->current_path_ = path;
    return true;
}

std::string UpyunBrowser::get_root_path() {
    return "/";
}

std::map<std::string, uint64_t> UpyunBrowser::get_quota_info() {
    std::map<std::string, uint64_t> quota;

    // 又拍云通过/usage/ API获取使用量
    std::string response = p_impl_->perform_upyun_request("GET", "/usage/");

#ifndef FALCON_BROWSER_NO_JSON
    try {
        if (!response.empty()) {
            json json_response = json::parse(response);
            if (json_response.contains("space")) {
                quota["used"] = json_response["space"];
            }
            if (json_response.contains("amount")) {
                quota["file_count"] = json_response["amount"];
            }
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse quota info: " << e.what());
    }
#endif

    return quota;
}

} // namespace falcon