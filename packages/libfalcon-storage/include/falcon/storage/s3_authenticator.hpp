#pragma once

#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace falcon {

/**
 * @brief S3 认证器（AWS Signature Version 4 头签名）
 *
 * 从死代码 s3_plugin.{hpp,cpp} 提取并接入构建（此前 perform_s3_request
 * 只发 Date/Host 的匿名请求，对 MinIO/RustFS 等强制鉴权服务必然 403）。
 * sign_request 产出 Authorization 头值；x-amz-date 头由调用方按同一
 * request_time 格式化并放入 headers（必须参与签名）。
 */
class S3Authenticator {
public:
    /**
     * 生成 AWS V4 Authorization 头值
     *
     * @param method        HTTP 方法（GET/PUT/...）
     * @param uri           规范 URI（path 部分，已按 S3 key 编码，含前导 /）
     * @param headers       参与签名的请求头（键必须小写，须含 host 与 x-amz-date）
     * @param payload       请求体（对 GET/HEAD/DELETE 为空串）
     * @param access_key    访问密钥 ID
     * @param secret_key    秘密访问密钥
     * @param region        区域（如 us-east-1；MinIO/RustFS 常为 us-east-1）
     * @param service       服务名（S3 固定 "s3"）
     * @param request_time  请求时间（x-amz-date 与签名必须同源）
     * @param query_params  查询参数（原始值，内部做规范编码与排序；
     *                      ListObjectsV2 等带查询串的请求必须参与规范请求）
     */
    static std::string sign_request(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& headers,
        const std::string& payload,
        const std::string& access_key,
        const std::string& secret_key,
        const std::string& region,
        const std::string& service,
        const std::chrono::system_clock::time_point& request_time,
        const std::map<std::string, std::string>& query_params = {}
    );

    /**
     * 计算 SHA256 十六进制摘要（x-amz-content-sha256 头取值）
     */
    static std::string sha256(const std::string& data);

private:
    /**
     * 计算 HMAC-SHA256
     */
    static std::string hmac_sha256(
        const std::string& key,
        const std::string& data
    );

    /**
     * 十六进制编码（输入为原始字节串）
     */
    static std::string hex_encode(const std::string& data);

    /**
     * 获取规范请求字符串
     */
    static std::string get_canonical_request(
        const std::string& method,
        const std::string& uri,
        const std::map<std::string, std::string>& query_params,
        const std::map<std::string, std::string>& headers,
        const std::string& payload
    );

    /**
     * 获取规范查询字符串（键值各自规范编码后按字典序排序）
     */
    static std::string get_canonical_query_string(
        const std::map<std::string, std::string>& params
    );

    /**
     * 获取 SignedHeaders 列表（分号分隔的有序头名）
     */
    static std::string get_signed_headers(
        const std::map<std::string, std::string>& headers
    );

    /**
     * URL 编码（RFC 3986 非保留字符集，S3 规范编码同款）
     */
    static std::string url_encode(const std::string& str);
};

} // namespace falcon
