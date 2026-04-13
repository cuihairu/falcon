/**
 * @file s3_plugin.cpp
 * @brief Amazon S3 兼容存储协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/s3_plugin.hpp>
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
    Impl() : curl_(curl_easy_init()) {
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

    /**
     * 发送HTTP请求
     */
    std::string http_request(
        const std::string& method,
        const std::string& url,
        const std::map<std::string, std::string>& headers = {},
        const std::string& body = ""
    ) {
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

        // 设置请求头
        curl_slist* header_list = nullptr;
        for (const auto& [key, value] : headers) {
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
            return "";
        }

        return response;
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

    // 设置配置
    S3Config config;
    // TODO: 从options中获取认证信息
    p_impl_->set_config(config);

    // 获取预签名URL或直接下载
    std::string download_url;
    if (!config.access_key_id.empty()) {
        // 使用认证信息下载
        // TODO: 实现带签名的下载
    } else {
        // 尝试获取预签名URL
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

} // namespace falcon