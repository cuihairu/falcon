/**
 * @file s3_authenticator.cpp
 * @brief AWS Signature Version 4 签名实现
 *
 * 自死代码 s3_plugin.cpp 提取（该文件从未接入任何构建目标，
 * 库符号零 S3Authenticator——nm 实证）；sign_request 增加查询参数
 * 参与，支持 ListObjectsV2 等带查询串请求的规范请求组装。
 */

#include <falcon/storage/s3_authenticator.hpp>

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace falcon {

std::string S3Authenticator::sign_request(
    const std::string& method,
    const std::string& uri,
    const std::map<std::string, std::string>& headers,
    const std::string& payload,
    const std::string& access_key,
    const std::string& secret_key,
    const std::string& region,
    const std::string& service,
    const std::chrono::system_clock::time_point& request_time,
    const std::map<std::string, std::string>& query_params
) {
    // 时间戳（与调用方的 x-amz-date 头同源于 request_time）
    auto time_t = std::chrono::system_clock::to_time_t(request_time);
    std::tm tm = *std::gmtime(&time_t);

    char time_str[30];
    std::strftime(time_str, sizeof(time_str), "%Y%m%dT%H%M%SZ", &tm);

    char date_str[9];
    std::strftime(date_str, sizeof(date_str), "%Y%m%d", &tm);

    // 规范请求（查询参数参与规范化）
    std::string canonical_request = get_canonical_request(
        method, uri, query_params, headers, payload
    );

    // 待签名字符串
    std::string algorithm = "AWS4-HMAC-SHA256";
    std::string credential_scope =
        std::string(date_str) + "/" + region + "/" + service + "/aws4_request";

    std::string string_to_sign = algorithm + "\n" +
                                 std::string(time_str) + "\n" +
                                 credential_scope + "\n" +
                                 sha256(canonical_request);

    // 签名密钥派生（AWS4 规范四段派生）
    std::string k_date = hmac_sha256("AWS4" + secret_key, date_str);
    std::string k_region = hmac_sha256(k_date, region);
    std::string k_service = hmac_sha256(k_region, service);
    std::string k_signing = hmac_sha256(k_service, "aws4_request");

    std::string signature = hex_encode(hmac_sha256(k_signing, string_to_sign));

    return algorithm + " " +
           "Credential=" + access_key + "/" + credential_scope + ", " +
           "SignedHeaders=" + get_signed_headers(headers) + ", " +
           "Signature=" + signature;
}

std::string S3Authenticator::sha256(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, data.c_str(), data.length());
    SHA256_Final(hash, &ctx);

    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(hash[i]);
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
         reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
         result, &result_len);
    return std::string(reinterpret_cast<char*>(result), result_len);
}

std::string S3Authenticator::hex_encode(const std::string& data) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (char byte : data) {
        ss << std::setw(2) << static_cast<int>(static_cast<unsigned char>(byte));
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
    // 规范 URI（path 已由调用方编码；空路径视为根）
    std::string canonical_uri = uri;
    if (canonical_uri.empty()) {
        canonical_uri = "/";
    }

    // 规范查询字符串
    std::string canonical_query = get_canonical_query_string(query_params);

    // 规范头部（键必须已小写——SigV4 规范形态，get_signed_headers 不做转换）
    std::vector<std::string> header_names;
    for (const auto& [key, value] : headers) {
        header_names.push_back(key);
    }
    std::sort(header_names.begin(), header_names.end());

    std::string canonical_headers;
    for (const auto& name : header_names) {
        canonical_headers += name + ":" + headers.at(name) + "\n";
    }

    // 签名头部列表
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
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            encoded << c;
        } else {
            encoded << std::uppercase;
            encoded << '%' << std::setw(2)
                    << static_cast<int>(static_cast<unsigned char>(c));
            encoded << std::nouppercase;
        }
    }

    return encoded.str();
}

} // namespace falcon
