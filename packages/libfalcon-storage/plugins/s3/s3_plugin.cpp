/**
 * @file s3_plugin.cpp
 * @brief Amazon S3 兼容存储协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/storage/s3_plugin.hpp>
#include <falcon/logger.hpp>
#include <falcon/download_task.hpp>
#include <falcon/download_options.hpp>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <sstream>
#include <algorithm>
#include <iomanip>
#include <ctime>
#include <vector>
#include <openssl/sha.h>
#include <openssl/hmac.h>

using json = nlohmann::json;

namespace falcon {

// S3插件实现类
class S3Plugin::Impl {
public:
    explicit Impl(const S3Config& config = {}) : curl_(curl_easy_init()), config_(config) {
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        // 设置默认CURL选项
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
    }

    ~Impl() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    void set_config(const S3Config& config) {
        config_ = config;
    }

    const S3Config& get_config() const {
        return config_;
    }

    /**
     * 发送HTTP请求（带S3签名）
     */
    std::string http_request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        const std::string& body = ""
    ) {
        // 解析URL获取URI路径
        std::string uri = extract_uri_from_url(url);
        std::string host = extract_host_from_url(url);

        // 构建完整的请求头
        std::map<std::string, std::string> all_headers = headers;

        // 添加必需的S3头部
        all_headers["Host"] = host;

        // 如果有认证信息，添加签名
        if (!config_.access_key_id.empty() && !config_.secret_access_key.empty()) {
            all_headers["x-amz-content-sha256"] = sha256_hex(body);
            all_headers["x-amz-date"] = get_amz_date();

            std::string authorization = sign_request(method, uri, all_headers, body);
            all_headers["Authorization"] = authorization;
        }

        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

        // 设置请求头
        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : all_headers) {
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

        // 设置响应回调
        std::string response;
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

        // 执行请求
        CURLcode res = curl_easy_perform(curl_);

        // 清理
        if (header_list) {
            curl_slist_free_all(header_list);
        }

        if (res != CURLE_OK) {
            Logger::error("CURL error: {}", curl_easy_strerror(res));
            return "";
        }

        long response_code;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &response_code);

        if (response_code < 200 || response_code >= 300) {
            Logger::error("HTTP error: {}", response_code);
            Logger::error("Response body: {}", response);
            return "";
        }

        return response;
    }

    /**
     * 下载S3对象（带签名）
     */
    bool download_object(
        const std::string& bucket,
        const std::string& key,
        const std::string& output_path
    ) {
        // 构建对象URL
        std::string url = build_object_url(bucket, key);

        // 发送GET请求
        std::map<std::string, std::string> headers;
        std::string response = http_request("GET", url, headers, "");

        if (response.empty()) {
            return false;
        }

        // 写入文件
        FILE* file = fopen(output_path.c_str(), "wb");
        if (!file) {
            Logger::error("Failed to open output file: {}", output_path);
            return false;
        }

        fwrite(response.data(), 1, response.size(), file);
        fclose(file);

        return true;
    }

    /**
     * 获取预签名URL
     */
    std::string get_presigned_url(
        const std::string& bucket,
        const std::string& key,
        int expires_in_seconds = 3600
    ) {
        if (config_.access_key_id.empty() || config_.secret_access_key.empty()) {
            Logger::error("S3 credentials not configured");
            return "";
        }

        std::string uri = "/" + bucket + "/" + key;
        std::string host = build_s3_host(bucket);
        std::string url = (config_.use_ssl ? "https://" : "http://") + host + uri;

        // 构建查询参数
        std::map<std::string, std::string> query_params;
        query_params["X-Amz-Algorithm"] = "AWS4-HMAC-SHA256";
        query_params["X-Amz-Credential"] = build_credential_scope();
        query_params["X-Amz-Date"] = get_amz_date();
        query_params["X-Amz-Expires"] = std::to_string(expires_in_seconds);
        query_params["X-Amz-SignedHeaders"] = "host";

        // 生成签名
        std::map<std::string, std::string> headers;
        headers["Host"] = host;
        std::string signature = S3Authenticator::generate_presigned_url(
            "GET", uri, query_params, headers,
            config_.access_key_id, config_.secret_access_key,
            config_.region, "s3", expires_in_seconds,
            std::chrono::system_clock::now()
        );

        // 构建完整URL
        std::string presigned_url = url + "?";
        bool first = true;
        for (const auto& [key, value] : query_params) {
            if (!first) presigned_url += "&";
            presigned_url += key + "=" + url_encode(value);
            first = false;
        }
        presigned_url += "&X-Amz-Signature=" + signature;

        return presigned_url;
    }

private:
    /**
     * 从URL提取URI路径
     */
    static std::string extract_uri_from_url(const std::string& url) {
        size_t path_start = url.find('/', url.find("://"));
        if (path_start == std::string::npos) {
            return "/";
        }
        return url.substr(path_start);
    }

    /**
     * 从URL提取主机名
     */
    static std::string extract_host_from_url(const std::string& url) {
        size_t host_start = url.find("://") + 3;
        size_t host_end = url.find('/', host_start);
        if (host_end == std::string::npos) {
            host_end = url.length();
        }
        return url.substr(host_start, host_end - host_start);
    }

    /**
     * 构建对象URL
     */
    std::string build_object_url(const std::string& bucket, const std::string& key) {
        std::string host = build_s3_host(bucket);
        std::string uri = "/" + bucket + "/" + key;
        return (config_.use_ssl ? "https://" : "http://") + host + uri;
    }

    /**
     * 构建S3主机名
     */
    std::string build_s3_host(const std::string& bucket) const {
        if (!config_.endpoint.empty()) {
            return config_.endpoint;
        }

        if (config_.host_style == "virtual") {
            return bucket + ".s3." + config_.region + ".amazonaws.com";
        } else {
            return "s3." + config_.region + ".amazonaws.com";
        }
    }

    /**
     * 构建凭证范围
     */
    std::string build_credential_scope() const {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::gmtime(&time_t);

        char date_str[9];
        std::strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);

        return std::string(config_.access_key_id) + "/" + date_str + "/" +
               config_.region + "/s3/aws4_request";
    }

    /**
     * 获取AWS日期格式
     */
    static std::string get_amz_date() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::gmtime(&time_t);

        char time_str[17];
        std::strftime(time_str, sizeof(time_str), "%Y%m%dT%H%M%SZ", &tm);
        return std::string(time_str);
    }

    /**
     * URL编码
     */
    static std::string url_encode(const std::string& str) {
        std::ostringstream encoded;
        encoded.fill('0');
        encoded << std::hex;

        for (char c : str) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded << c;
            } else {
                encoded << std::uppercase;
                encoded << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
                encoded << std::nouppercase;
            }
        }

        return encoded.str();
    }

    /**
     * 生成AWS V4签名
     */
    std::string sign_request(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const std::string& payload
    ) {
        // 获取当前时间
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm = *std::gmtime(&time_t);

        char time_str[30];
        std::strftime(time_str, sizeof(time_str), "%Y%m%dT%H%M%SZ", &tm);

        char date_str[9];
        std::strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);

        // 创建规范请求
        std::string canonical_uri = uri;
        std::string canonical_query_string = "";

        // 排序头部
        std::vector<std::string> header_names;
        for (const auto& [key, value] : headers) {
            header_names.push_back(key);
        }
        std::sort(header_names.begin(), header_names.end());

        std::string canonical_headers;
        std::string signed_headers;
        for (size_t i = 0; i < header_names.size(); ++i) {
            const std::string& name = header_names[i];
            canonical_headers += name + ":" + headers.at(name) + "\n";
            signed_headers += name;
            if (i < header_names.size() - 1) {
                signed_headers += ";";
            }
        }

        // 计算payload哈希
        std::string payload_hash = sha256_hex(payload);

        // 构建规范请求
        std::string canonical_request = method + "\n" +
                                     canonical_uri + "\n" +
                                     canonical_query_string + "\n" +
                                     canonical_headers + "\n" +
                                     signed_headers + "\n" +
                                     payload_hash;

        // 创建待签名字符串
        std::string algorithm = "AWS4-HMAC-SHA256";
        std::string credential_scope = date_str + "/" + config_.region + "/s3/aws4_request";

        std::string string_to_sign = algorithm + "\n" +
                                     time_str + "\n" +
                                     credential_scope + "\n" +
                                     sha256_hex(canonical_request);

        // 计算签名
        std::string k_date = hmac_sha256("AWS4" + config_.secret_access_key, date_str);
        std::string k_region = hmac_sha256(k_date, config_.region);
        std::string k_service = hmac_sha256(k_region, "s3");
        std::string k_signing = hmac_sha256(k_service, "aws4_request");
        std::string signature = hmac_sha256_hex(k_signing, string_to_sign);

        // 构建授权头部
        std::string authorization = algorithm + " " +
                                  "Credential=" + config_.access_key_id + "/" + credential_scope + ", " +
                                  "SignedHeaders=" + signed_headers + ", " +
                                  "Signature=" + signature;

        return authorization;
    }

private:
    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
        userp->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    std::string sha256_hex(const std::string& data) {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, data.c_str(), data.length());
        SHA256_Final(hash, &sha256);

        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
        }
        return ss.str();
    }

    std::string hmac_sha256(const std::string& key, const std::string& data) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int result_len = 0;

        HMAC(EVP_sha256(), key.c_str(), key.length(),
             (unsigned char*)data.c_str(), data.length(),
             result, &result_len);

        return std::string((char*)result, result_len);
    }

    std::string hmac_sha256_hex(const std::string& key, const std::string& data) {
        std::string result = hmac_sha256(key, data);
        std::stringstream ss;
        for (unsigned char c : result) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)c;
        }
        return ss.str();
    }

    CURL* curl_;
    S3Config config_;
};

// S3Plugin 实现
S3Plugin::S3Plugin() : p_impl_(std::make_unique<Impl>()) {}

S3Plugin::~S3Plugin() = default;

bool S3Plugin::can_handle(const std::string& url) const {
    return url.find("s3://") == 0 ||
           url.find(".s3.") != std::string::npos ||
           url.find("s3.amazonaws.com") != std::string::npos;
}

std::shared_ptr<DownloadTask> S3Plugin::download(
    const std::string& url,
    const DownloadOptions& options) {

    // 解析S3 URL
    auto s3_url = S3UrlParser::parse(url);

    // 从options中提取认证信息（通过自定义header）
    S3Config config = p_impl_->get_config();

    // 尝试从options.headers获取S3认证信息
    auto access_key_it = options.headers.find("X-S3-Access-Key");
    auto secret_key_it = options.headers.find("X-S3-Secret-Key");
    auto region_it = options.headers.find("X-S3-Region");
    auto endpoint_it = options.headers.find("X-S3-Endpoint");

    if (access_key_it != options.headers.end()) {
        config.access_key_id = access_key_it->second;
    }
    if (secret_key_it != options.headers.end()) {
        config.secret_access_key = secret_key_it->second;
    }
    if (region_it != options.headers.end()) {
        config.region = region_it->second;
    }
    if (endpoint_it != options.headers.end()) {
        config.endpoint = endpoint_it->second;
    }

    // 从URL解析获取bucket和region
    if (!s3_url.bucket.empty() && config.bucket.empty()) {
        config.bucket = s3_url.bucket;
    }
    if (!s3_url.region.empty() && config.region.empty()) {
        config.region = s3_url.region;
    }

    p_impl_->set_config(config);

    // 获取预签名URL或直接下载
    std::string download_url;
    if (!config.access_key_id.empty() && !config.secret_access_key.empty()) {
        // 使用认证信息生成预签名URL
        download_url = p_impl_->get_presigned_url(config.bucket, s3_url.key, 3600);
    } else {
        // 尝试获取预签名URL（需要先配置认证信息）
        download_url = generate_presigned_url(s3_url.key);
    }

    if (download_url.empty()) {
        Logger::error("Failed to get download URL for S3 object");
        return nullptr;
    }

    // 创建下载任务
    auto task = std::make_shared<DownloadTask>();
    task->set_url(download_url);
    task->set_options(options);

    return task;
}

void S3Plugin::set_config(const S3Config& config) {
    config_ = config;
    p_impl_->set_config(config);
}

// S3UrlParser 实现
S3UrlParser::S3Url S3UrlParser::parse(const std::string& url) {
    S3Url s3_url;

    if (url.find("s3://") == 0) {
        // s3://bucket/key 格式
        size_t bucket_start = 5; // "s3://" 的长度
        size_t bucket_end = url.find('/', bucket_start);

        s3_url.bucket = url.substr(bucket_start, bucket_end - bucket_start);
        s3_url.key = url.substr(bucket_end + 1);
        s3_url.use_ssl = true;
        s3_url.is_virtual_host = false;
        s3_url.endpoint = "s3.amazonaws.com";
    } else {
        // HTTP/HTTPS URL格式
        bool use_ssl = url.find("https://") == 0;

        // 查找存储桶
        size_t host_start = url.find("://") + 3;
        size_t host_end = url.find('/', host_start);
        std::string host = url.substr(host_start, host_end - host_start);

        if (host.find(".s3.") != std::string::npos) {
            // 虚拟主机样式
            size_t dot_pos = host.find(".s3.");
            s3_url.bucket = host.substr(0, dot_pos);
            s3_url.is_virtual_host = true;

            // 提取区域
            size_t region_start = dot_pos + 4;
            size_t region_end = host.find(".amazonaws.com", region_start);
            if (region_end != std::string::npos) {
                s3_url.region = host.substr(region_start, region_end - region_start);
            }
        } else if (host.find("s3.") == 0) {
            // 路径样式
            s3_url.is_virtual_host = false;

            // 提取区域
            size_t region_start = 3;
            size_t region_end = host.find(".amazonaws.com", region_start);
            if (region_end != std::string::npos) {
                s3_url.region = host.substr(region_start, region_end - region_start);
            }

            // 从路径中提取存储桶
            size_t bucket_end = url.find('/', host_end + 1);
            s3_url.bucket = url.substr(host_end + 1, bucket_end - host_end - 1);
        }

        s3_url.key = url.substr(host_end + 1 + s3_url.bucket.length() + 1);
        s3_url.use_ssl = use_ssl;
        s3_url.endpoint = host;
    }

    return s3_url;
}

std::string S3UrlParser::build(const S3Url& s3_url) {
    std::string scheme = s3_url.use_ssl ? "https://" : "http://";

    if (s3_url.is_virtual_host) {
        return scheme + s3_url.bucket + ".s3." + s3_url.region +
               ".amazonaws.com/" + s3_url.key;
    } else {
        return scheme + "s3." + s3_url.region + ".amazonaws.com/" +
               s3_url.bucket + "/" + s3_url.key;
    }
}

std::string S3UrlParser::normalize(const std::string& url) {
    auto s3_url = parse(url);
    return build(s3_url);
}

// S3Utils 实现
S3StorageClass S3Utils::parse_storage_class(const std::string& class_name) {
    if (class_name == "STANDARD") return S3StorageClass::Standard;
    if (class_name == "REDUCED_REDUNDANCY") return S3StorageClass::ReducedRedundancy;
    if (class_name == "STANDARD_IA") return S3StorageClass::StandardIA;
    if (class_name == "ONEZONE_IA") return S3StorageClass::OnezoneIA;
    if (class_name == "INTELLIGENT_TIERING") return S3StorageClass::IntelligentTiering;
    if (class_name == "GLACIER") return S3StorageClass::Glacier;
    if (class_name == "DEEP_ARCHIVE") return S3StorageClass::GlacierDeepArchive;
    if (class_name == "GLACIER_IR") return S3StorageClass::GlacierInstantRetrieval;

    return S3StorageClass::Standard;
}

std::string S3Utils::get_storage_class_name(S3StorageClass storage_class) {
    switch (storage_class) {
        case S3StorageClass::Standard: return "STANDARD";
        case S3StorageClass::ReducedRedundancy: return "REDUCED_REDUNDANCY";
        case S3StorageClass::StandardIA: return "STANDARD_IA";
        case S3StorageClass::OnezoneIA: return "ONEZONE_IA";
        case S3StorageClass::IntelligentTiering: return "INTELLIGENT_TIERING";
        case S3StorageClass::Glacier: return "GLACIER";
        case S3StorageClass::GlacierDeepArchive: return "DEEP_ARCHIVE";
        case S3StorageClass::Outposts: return "OUTPOSTS";
        case S3StorageClass::GlacierInstantRetrieval: return "GLACIER_IR";
    }
    return "STANDARD";
}

// S3Authenticator 静态方法实现
std::string S3Authenticator::sign_request(
    const std::string& method,
    const std::string& uri,
    const std::map<std::string, std::string>& headers,
    const std::string& payload,
    const std::string& access_key,
    const std::string& secret_key,
    const std::string& region,
    const std::string& service,
    const std::chrono::system_clock::time_point& request_time
) {
    // 获取时间戳
    auto time_t = std::chrono::system_clock::to_time_t(request_time);
    std::tm tm = *std::gmtime(&time_t);

    char time_str[30];
    std::strftime(time_str, sizeof(time_str), "%Y%m%dT%H%M%SZ", &tm);

    char date_str[9];
    std::strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);

    // 构建规范请求
    std::string canonical_request = get_canonical_request(
        method, uri, {}, headers, payload
    );

    // 构建待签名字符串
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credential_scope = std::string(date_str) + "/" + region + "/" + service + "/aws4_request";

    std::string string_to_sign = algorithm + "\n" +
                                 std::string(time_str) + "\n" +
                                 credential_scope + "\n" +
                                 sha256(canonical_request);

    // 计算签名密钥
    std::string k_date = hmac_sha256("AWS4" + secret_key, date_str);
    std::string k_region = hmac_sha256(k_date, region);
    std::string k_service = hmac_sha256(k_region, service);
    std::string k_signing = hmac_sha256(k_service, "aws4_request");

    // 计算签名
    std::string signature = hex_encode(hmac_sha256(k_signing, string_to_sign));

    // 构建授权头部
    std::string signed_headers = get_signed_headers(headers);
    std::string authorization = algorithm + " " +
                              "Credential=" + access_key + "/" + credential_scope + ", " +
                              "SignedHeaders=" + signed_headers + ", " +
                              "Signature=" + signature;

    return authorization;
}

std::string S3Authenticator::generate_presigned_url(
    const std::string& method,
    const std::string& uri,
    const std::map<std::string, std::string>& query_params,
    const std::map<std::string, std::string>& headers,
    const std::string& access_key,
    const std::string& secret_key,
    const std::string& region,
    const std::string& service,
    int expires_in_seconds,
    const std::chrono::system_clock::time_point& request_time
) {
    // 获取时间戳
    auto time_t = std::chrono::system_clock::to_time_t(request_time);
    std::tm tm = *std::gmtime(&time_t);

    char time_str[30];
    std::strftime(time_str, sizeof(time_str), "%Y%m%dT%H%M%SZ", &tm);

    char date_str[9];
    std::strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);

    // 构建查询参数
    std::map<std::string, std::string> all_params = query_params;
    all_params["X-Amz-Algorithm"] = "AWS4-HMAC-SHA256";
    all_params["X-Amz-Credential"] = access_key + "/" + date_str + "/" + region + "/" + service + "/aws4_request";
    all_params["X-Amz-Date"] = time_str;
    all_params["X-Amz-Expires"] = std::to_string(expires_in_seconds);
    all_params["X-Amz-SignedHeaders"] = get_signed_headers(headers);

    // 构建规范请求
    std::string canonical_query_string = get_canonical_query_string(all_params);
    std::string canonical_request = get_canonical_request(
        method, uri, all_params, headers, ""
    );

    // 构建待签名字符串
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credential_scope = std::string(date_str) + "/" + region + "/" + service + "/aws4_request";

    std::string string_to_sign = algorithm + "\n" +
                                 std::string(time_str) + "\n" +
                                 credential_scope + "\n" +
                                 sha256(canonical_request);

    // 计算签名密钥
    std::string k_date = hmac_sha256("AWS4" + secret_key, date_str);
    std::string k_region = hmac_sha256(k_date, region);
    std::string k_service = hmac_sha256(k_region, service);
    std::string k_signing = hmac_sha256(k_service, "aws4_request");

    // 计算签名
    std::string signature = hex_encode(hmac_sha256(k_signing, string_to_sign));

    return signature;
}

std::string S3Authenticator::sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX sha256;
    SHA256_Init(&sha256);
    SHA256_Update(&sha256, data.c_str(), data.length());
    SHA256_Final(hash, &sha256);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    return ss.str();
}

std::string S3Authenticator::hmac_sha256(
    const std::string& key,
    const std::string& data
) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int result_len = 0;

    HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.length()),
         (unsigned char*)data.c_str(), data.length(),
         result, &result_len);

    return std::string((char*)result, result_len);
}

std::string S3Authenticator::hex_encode(const std::vector<uint8_t>& data) {
    std::stringstream ss;
    for (uint8_t byte : data) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)byte;
    }
    return ss.str();
}

std::string S3Authenticator::get_canonical_request(
    const std::string& method,
    const std::string& uri,
    const std::map<std::string, std::string>& query_params,
    const std::map<std::string, std::string>& headers,
    const std::string& payload
) {
    // 规范URI
    std::string canonical_uri = uri;
    if (canonical_uri.empty()) {
        canonical_uri = "/";
    }

    // 规范查询字符串
    std::string canonical_query = get_canonical_query_string(query_params);

    // 规范头部
    std::vector<std::string> header_names;
    for (const auto& [key, value] : headers) {
        header_names.push_back(key);
    }
    std::sort(header_names.begin(), header_names.end());

    std::string canonical_headers;
    for (const auto& name : header_names) {
        canonical_headers += name + ":" + headers.at(name) + "\n";
    }

    // 签名头部
    std::string signed_headers = get_signed_headers(headers);

    // 载荷哈希
    std::string payload_hash = sha256(payload);

    // 组装规范请求
    return method + "\n" +
           canonical_uri + "\n" +
           canonical_query + "\n" +
           canonical_headers + "\n" +
           signed_headers + "\n" +
           payload_hash;
}

std::string S3Authenticator::get_string_to_sign(
    const std::chrono::system_clock::time_point& request_time,
    const std::string& region,
    const std::string& service,
    const std::string& canonical_request
) {
    // 获取时间戳
    auto time_t = std::chrono::system_clock::to_time_t(request_time);
    std::tm tm = *std::gmtime(&time_t);

    char time_str[30];
    std::strftime(time_str, sizeof(time_str), "%Y%m%dT%H%M%SZ", &tm);

    char date_str[9];
    std::strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);

    // 构建待签名字符串
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credential_scope = std::string(date_str) + "/" + region + "/" + service + "/aws4_request";

    return algorithm + "\n" +
           std::string(time_str) + "\n" +
           credential_scope + "\n" +
           sha256(canonical_request);
}

std::string S3Authenticator::get_canonical_query_string(
    const std::map<std::string, std::string>& params
) {
    std::vector<std::string> encoded_pairs;
    for (const auto& [key, value] : params) {
        encoded_pairs.push_back(url_encode(key) + "=" + url_encode(value));
    }
    std::sort(encoded_pairs.begin(), encoded_pairs.end());

    std::string result;
    for (size_t i = 0; i < encoded_pairs.size(); ++i) {
        if (i > 0) result += "&";
        result += encoded_pairs[i];
    }
    return result;
}

std::string S3Authenticator::get_signed_headers(
    const std::map<std::string, std::string>& headers
) {
    std::vector<std::string> header_names;
    for (const auto& [key, value] : headers) {
        header_names.push_back(key);
    }
    std::sort(header_names.begin(), header_names.end());

    std::string result;
    for (size_t i = 0; i < header_names.size(); ++i) {
        if (i > 0) result += ";";
        result += header_names[i];
    }
    return result;
}

std::string S3Authenticator::url_encode(const std::string& str) {
    std::ostringstream encoded;
    encoded.fill('0');
    encoded << std::hex;

    for (char c : str) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << std::uppercase;
            encoded << '%' << std::setw(2) << int(static_cast<unsigned char>(c));
            encoded << std::nouppercase;
        }
    }

    return encoded.str();
}

} // namespace falcon