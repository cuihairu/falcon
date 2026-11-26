/**
 * @file oss_browser.cpp
 * @brief 阿里云OSS资源浏览器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/storage/oss_browser.hpp>
#include <falcon/detail/injection.hpp>
#include <falcon/storage/cloud_url_protocols.hpp>
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
    using namespace cloud;

    if (!starts_with_protocol(url, PROTOCOL_OSS)) {
        throw std::invalid_argument("Invalid OSS URL: missing oss:// protocol");
    }

    OSSUrl oss_url;
    const size_t host_start = PROTOCOL_OSS.size();
    const size_t path_start = url.find('/', host_start);
    const std::string host = path_start == std::string::npos
        ? url.substr(host_start)
        : url.substr(host_start, path_start - host_start);

    if (host.empty()) {
        throw std::invalid_argument("Invalid OSS URL: missing host");
    }
    if (host.find('@') != std::string::npos) {
        throw std::invalid_argument("Invalid OSS URL: unsupported host format");
    }

    const size_t dot_pos = host.find('.');
    if (dot_pos != std::string::npos) {
        oss_url.bucket = host.substr(0, dot_pos);
        oss_url.endpoint = host.substr(dot_pos + 1);
    } else {
        oss_url.bucket = host;
    }

    if (!oss_url.endpoint.empty() && oss_url.endpoint.rfind("oss-", 0) == 0) {
        const size_t region_end = oss_url.endpoint.find(".aliyuncs.com");
        if (region_end != std::string::npos && region_end > 4) {
            oss_url.region = oss_url.endpoint.substr(4, region_end - 4);
        }
    }

    if (path_start != std::string::npos) {
        oss_url.key = url.substr(path_start + 1);
    }

    return oss_url;
}

/**
 * @brief OSS浏览器实现细节
 */
class OSSBrowser::Impl {
public:
    Impl() {
        // 注入命中时短路真实调用，避免已创建句柄在 throw 路径泄漏
        curl_ = detail::inject_failure(detail::InjectPoint::CurlEasyInit)
                    ? nullptr
                    : curl_easy_init();
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
        // 自定义 endpoint（携带 scheme，如 S3 兼容网关/私有化部署/Mock）走
        // path-style：endpoint/bucket/key；否则官方 virtual-host 域名
        if (oss_url_.endpoint.rfind("http://", 0) == 0 ||
            oss_url_.endpoint.rfind("https://", 0) == 0) {
            std::string url = oss_url_.endpoint;
            if (url.back() == '/') {
                url.pop_back();
            }
            url += "/" + bucket;
            if (!key.empty()) {
                url += "/" + encode_key(key);
            }
            return url;
        }
        std::string url = "https://" + bucket + "." + oss_url_.endpoint;
        if (!key.empty()) {
            url += "/" + encode_key(key);
        }
        return url;
    }

    /// 对象 key 编码：逐段编码、保留 '/'（url_encode 会把 '/' 编成 %2F，
    /// 导致子目录 key 的对象在真实服务上必然 404）
    std::string encode_key(const std::string& key) {
        std::string out;
        size_t start = 0;
        while (true) {
            size_t slash = key.find('/', start);
            out += url_encode(key.substr(start, slash == std::string::npos
                                                   ? std::string::npos
                                                   : slash - start));
            if (slash == std::string::npos) {
                break;
            }
            out += '/';
            start = slash + 1;
        }
        return out;
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
        const std::string& body = "",
        std::map<std::string, std::string>* response_headers = nullptr,
        bool* ok = nullptr) {

        // 列举等请求的查询串此前从未拼到 URL 上（只进了签名）——prefix/
        // max-keys 等参数从未真正发到服务端；统一在此追加
        std::string full_url = url;
        if (!query_string.empty()) {
            full_url += (full_url.find('?') == std::string::npos ? "?" : "&") + query_string;
        }

        curl_easy_setopt(curl_, CURLOPT_URL, full_url.c_str());
        if (method == "HEAD") {
            // CURLOPT_NOBODY 才是真正的 HEAD：CUSTOMREQUEST 只改请求行方法
            // 字符串、响应仍按 GET 处理，且 handle 复用时残留会覆盖 NOBODY
            // 的方法切换——二者必须互斥清设
            curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, nullptr);
        } else {
            curl_easy_setopt(curl_, CURLOPT_NOBODY, 0L);
            curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());
        }

        // 准备请求头
        std::map<std::string, std::string> request_headers = headers;

        // 添加必要的headers
        std::string date = get_gmt_time();
        request_headers["Date"] = date;
        request_headers["Host"] = get_host_from_url(url);

        // 生成签名（取 URL 的 path 部分参与签名——原 find('/')+bucket
        // 偏移算术假设固定布局，host/port 长度不同即把 authority 片段
        // 混进规范资源，签名恒错）
        size_t scheme_end = url.find("://");
        size_t path_start =
            scheme_end == std::string::npos ? std::string::npos : url.find('/', scheme_end + 3);
        std::string uri = path_start == std::string::npos ? "/" : url.substr(path_start);
        // 虚拟主机域名下 path 不含 bucket，规范资源须以 /bucket 开头
        if (uri.rfind("/" + oss_url_.bucket, 0) != 0) {
            uri = "/" + oss_url_.bucket + uri;
        }
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

        // 设置请求体（handle 复用：空 body 请求清除先前残留）
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS,
                         body.empty() ? nullptr : body.c_str());

        std::string response;
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        // 响应头回传（HEAD 的元数据只在头部）
        std::map<std::string, std::string> header_map;
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &header_map);

        CURLcode res = curl_easy_perform(curl_);

        if (header_list) {
            curl_slist_free_all(header_list);
        }

        if (response_headers) {
            *response_headers = std::move(header_map);
        }

        // HTTP 层失败（4xx/5xx）同样视为请求失败：成功与否不能只看传输
        // 结果，OSS 用状态码表达对象不存在/权限不足等业务错误
        long status = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status);
        const bool request_ok = (res == CURLE_OK) && status >= 200 && status < 400;
        if (ok) {
            *ok = request_ok;
        }

        if (!request_ok) {
            if (res != CURLE_OK) {
                FALCON_LOG_ERROR_STREAM("OSS request failed: " << curl_easy_strerror(res));
            } else {
                FALCON_LOG_ERROR_STREAM("OSS request failed with HTTP status " << status);
            }
            return "";
        }

        return response;
    }

    RemoteResource parse_oss_object(const json& obj, [[maybe_unused]] const ListOptions& options) {
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
        } else if (options.sort_by == "modified_time") {
            std::sort(resources.begin(), resources.end(),
                [&](const RemoteResource& a, const RemoteResource& b) {
                    if (options.sort_desc) {
                        return a.modified_time > b.modified_time;
                    }
                    return a.modified_time < b.modified_time;
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

        HMAC(EVP_sha1(), key.c_str(), static_cast<int>(key.length()),
             (unsigned char*)data.c_str(), data.length(),
             hmac, &hmac_len);

        // Base64编码
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* bmem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, bmem);
        BIO_write(b64, hmac, static_cast<int>(hmac_len));
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

    static size_t HeaderCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* headers = static_cast<std::map<std::string, std::string>*>(userp);
        std::string line(static_cast<char*>(contents), size * nmemb);
        // 去掉行终止符
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon != std::string::npos && colon > 0) {
            std::string key = line.substr(0, colon);
            std::string value = line.substr(colon + 1);
            // 值前导空白剔除（HTTP 允许 ": value"）
            size_t begin = value.find_first_not_of(" \t");
            value = (begin == std::string::npos) ? "" : value.substr(begin);
            (*headers)[key] = value;
        }
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
    bool ok = false;
    p_impl_->perform_oss_request("GET", test_url, {}, "", "", nullptr, &ok);

    return ok;
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
        if (path.empty() || path.back() != '/') {
            query_string += "/";
        }
    }

    query_string += "&max-keys=" + std::to_string(options.include_metadata ? 1000 : 100);

    // 发送请求
    std::string response = p_impl_->perform_oss_request("GET", url, {}, query_string);

    if (response.empty()) {
        FALCON_LOG_ERROR_STREAM("Failed to list OSS directory");
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
        FALCON_LOG_ERROR_STREAM("Failed to parse OSS response: " << e.what());
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

    // HEAD 无响应体，成功与否看状态码（原条件 `!response.empty() ||
    // response != "error"` 恒真——对象不存在也报"存在"）；元数据在响应头里回传
    std::map<std::string, std::string> response_headers;
    bool ok = false;
    p_impl_->perform_oss_request("HEAD", url, {}, "", "", &response_headers, &ok);

    if (ok) {
        info.path = path;
        info.name = path.substr(path.find_last_of('/') + 1);
        info.type = ResourceType::File;

        auto content_length = response_headers.find("Content-Length");
        if (content_length != response_headers.end()) {
            info.size = std::stoull(content_length->second);
        }

        auto last_modified = response_headers.find("Last-Modified");
        if (last_modified != response_headers.end()) {
            info.modified_time = last_modified->second;
        }

        auto etag = response_headers.find("ETag");
        if (etag != response_headers.end()) {
            info.etag = etag->second;
        }

        auto content_type = response_headers.find("Content-Type");
        if (content_type != response_headers.end()) {
            info.mime_type = content_type->second;
        }
    }

    return info;
}

bool OSSBrowser::create_directory(const std::string& path, [[maybe_unused]] bool recursive) {
    // OSS使用PUT操作创建目录对象
    std::string dir_path = path;
    if (dir_path.empty() || dir_path.back() != '/') {
        dir_path += "/";
    }

    std::string url = p_impl_->build_oss_url(p_impl_->oss_url_.bucket, dir_path);
    std::map<std::string, std::string> headers;
    headers["Content-Type"] = "application/x-directory";
    headers["x-oss-meta-type"] = "directory";

    // OSS 的写操作成功响应通常无响应体（204 No Content），按请求结果判定
    bool ok = false;
    p_impl_->perform_oss_request("PUT", url, headers, "", "", nullptr, &ok);

    return ok;
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

    // 删除指定路径（对象不存在的 204 仍算成功，网络/权限失败如实返回 false）
    bool ok = false;
    p_impl_->perform_oss_request("DELETE", url, {}, "", "", nullptr, &ok);

    return ok;
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

    bool ok = false;
    p_impl_->perform_oss_request("PUT", url, headers, "", "", nullptr, &ok);

    return ok;
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
        FALCON_LOG_ERROR_STREAM("Failed to parse quota info: " << e.what());
    }
#endif

    return quota;
}

} // namespace falcon
