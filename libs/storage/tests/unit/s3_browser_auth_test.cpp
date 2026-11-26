/**
 * @file s3_browser_auth_test.cpp
 * @brief S3 浏览器 SigV4 头签名 mock 测试（服务器侧独立验签）
 * @author Falcon Team
 * @date 2026-09-19
 *
 * MinIO/RustFS 等强制鉴权服务必需 Authorization 签名（此前匿名请求
 * 必然 403 AccessDenied）。本文件不调用 S3Authenticator——测试侧用
 * OpenSSL 独立重导签名全程（规范请求→待签串→四段密钥派生），与线上
 * 收到的 Authorization 精确比对，避免"同一个 bug 自我印证"。
 *
 * 覆盖三路：HEAD 对象（无查询）/ ListObjectsV2（查询参数规范编码与
 * 排序——prefix=docs/ 线上编码为 docs%2F，签名前须先解码还原，否则
 * 二次编码 %252F）/ 无凭据匿名路径（保持 Date/Host，无 Authorization）。
 */

#include <falcon/storage/s3_browser.hpp>

#include <gtest/gtest.h>

#include "mock_http_server.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace falcon;

namespace {

const char* kBucket = "testbucket";
const char* kAccessKey = "test-ak";
const char* kSecretKey = "test-secret-key";

//------------------------------------------------------------------------------
// 测试侧独立 SigV4 重导（OpenSSL 直调，不经 S3Authenticator）
//------------------------------------------------------------------------------

std::string test_lower(std::string s) {
    for (auto& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

/// RFC 3986 非保留字符集编码（S3 规范查询编码同款）
std::string test_url_encode(const std::string& s) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (char c : s) {
        const auto uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        } else {
            out << '%' << std::setw(2) << static_cast<int>(uc);
            out << std::nouppercase;
        }
    }
    return out.str();
}

/// %XX 解码（S3 语义，无 '+' 还原）
std::string test_url_decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            const std::string hex = s.substr(i + 1, 2);
            out += static_cast<char>(std::strtol(hex.c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string test_hex(const std::string& raw) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : raw) {
        out << std::setw(2) << static_cast<int>(static_cast<unsigned char>(byte));
    }
    return out.str();
}

std::string test_sha256_hex(const std::string& data) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);
    return test_hex(std::string(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH));
}

std::string test_hmac(const std::string& key, const std::string& data) {
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(),
         result, &len);
    return std::string(reinterpret_cast<char*>(result), len);
}

/// 从 Authorization 提取 SignedHeaders 列表
std::vector<std::string> parse_signed_headers(const std::string& auth) {
    const std::string kKey = "SignedHeaders=";
    const auto pos = auth.find(kKey);
    if (pos == std::string::npos) return {};
    const auto end = auth.find(", Signature=", pos);
    if (end == std::string::npos) return {};
    const std::string joined = auth.substr(pos + kKey.size(), end - pos - kKey.size());
    std::vector<std::string> names;
    size_t start = 0;
    while (start <= joined.size()) {
        const auto semi = joined.find(';', start);
        if (semi == std::string::npos) {
            names.push_back(joined.substr(start));
            break;
        }
        names.push_back(joined.substr(start, semi - start));
        start = semi + 1;
    }
    return names;
}

std::string extract_signature(const std::string& auth) {
    const std::string kKey = "Signature=";
    const auto pos = auth.rfind(kKey);
    return pos == std::string::npos ? "" : auth.substr(pos + kKey.size());
}

/**
 * 独立重导期望签名：线上收到的 (method, path含查询, 请求头) 为输入，
 * 与客户端侧签名同源的事实只有 AK/SK 与请求本身。
 */
std::string expected_signature(const std::string& method,
                               const std::string& path,
                               const std::map<std::string, std::string>& headers) {
    const std::string& auth = headers.at("authorization");
    const std::string amz_date = headers.at("x-amz-date");
    const std::string date8 = amz_date.substr(0, 8);

    // 规范 URI 与查询参数（线上查询值解码还原，再按规范重编码排序）
    const auto query_begin = path.find('?');
    const std::string canonical_uri =
        query_begin == std::string::npos ? path : path.substr(0, query_begin);
    std::vector<std::pair<std::string, std::string>> params;
    if (query_begin != std::string::npos) {
        std::string query = path.substr(query_begin + 1);
        size_t start = 0;
        while (start <= query.size()) {
            const auto amp = query.find('&', start);
            const std::string pair = query.substr(
                start, amp == std::string::npos ? std::string::npos : amp - start);
            if (!pair.empty()) {
                const auto eq = pair.find('=');
                params.emplace_back(
                    test_url_decode(pair.substr(0, eq)),
                    eq == std::string::npos ? "" : test_url_decode(pair.substr(eq + 1)));
            }
            if (amp == std::string::npos) break;
            start = amp + 1;
        }
    }
    std::vector<std::string> encoded_pairs;
    for (const auto& [k, v] : params) {
        encoded_pairs.push_back(test_url_encode(k) + "=" + test_url_encode(v));
    }
    std::sort(encoded_pairs.begin(), encoded_pairs.end());
    std::string canonical_query;
    for (size_t i = 0; i < encoded_pairs.size(); ++i) {
        if (i > 0) canonical_query += "&";
        canonical_query += encoded_pairs[i];
    }

    // 规范头部（仅 SignedHeaders 列出的头，按该列表顺序）
    const auto signed_names = parse_signed_headers(auth);
    std::string canonical_headers;
    for (const auto& name : signed_names) {
        const auto it = headers.find(name);
        EXPECT_NE(it, headers.end()) << "signed header missing on wire: " << name;
        if (it == headers.end()) return "";
        canonical_headers += name + ":" + it->second + "\n";
    }

    const std::string canonical_request =
        method + "\n" + canonical_uri + "\n" + canonical_query + "\n" +
        canonical_headers + "\n" +
        auth.substr(auth.find("SignedHeaders=") + 14,
                    auth.find(", Signature=") - auth.find("SignedHeaders=") - 14) +
        "\n" + headers.at("x-amz-content-sha256");

    const std::string scope = date8 + "/us-east-1/s3/aws4_request";
    const std::string string_to_sign =
        "AWS4-HMAC-SHA256\n" + amz_date + "\n" + scope + "\n" +
        test_sha256_hex(canonical_request);

    const std::string k_date = test_hmac("AWS4" + std::string(kSecretKey), date8);
    const std::string k_region = test_hmac(k_date, "us-east-1");
    const std::string k_service = test_hmac(k_region, "s3");
    const std::string k_signing = test_hmac(k_service, "aws4_request");
    return test_hex(test_hmac(k_signing, string_to_sign));
}

/// x-amz-date 形状：YYYYMMDDTHHMMSSZ
bool is_amz_date_shape(const std::string& s) {
    if (s.size() != 16 || s[8] != 'T' || s[15] != 'Z') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 15) continue;
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

/// 带凭据的浏览器连接（endpoint 指向 mock，path-style）
bool connect_signed(S3Browser& browser, const std::string& base_url) {
    return browser.connect("s3://" + std::string(kBucket), {
        {"access_key_id", kAccessKey},
        {"secret_access_key", kSecretKey},
        {"region", "us-east-1"},
        {"endpoint", base_url},
    });
}

/// 请求记录（accept 线程写、测试线程读，互斥保护）
struct RecordingServer {
    std::mutex mu;
    std::vector<std::string> methods;
    std::vector<std::string> paths;
    std::vector<std::map<std::string, std::string>> headers;
    MockHttpServer server;

    explicit RecordingServer(int status = 200)
        : server([this, status](const std::string& m, const std::string& p,
                                const std::map<std::string, std::string>& h) {
              std::lock_guard<std::mutex> lock(mu);
              methods.push_back(m);
              paths.push_back(p);
              headers.push_back(h);
              MockHttpServer::Response resp;
              resp.status = status;
              resp.body = "{}";
              return resp;
          }) {}

    std::map<std::string, std::string> request(size_t index) {
        std::lock_guard<std::mutex> lock(mu);
        return headers.at(index);
    }
};

} // namespace

//==============================================================================
// HEAD 对象：无查询请求的签名精确性
//==============================================================================

TEST(S3BrowserAuthTest, HeadObjectSignatureMatchesIndependentDerivation) {
    RecordingServer rec;
    ASSERT_TRUE(rec.server.start());

    S3Browser browser;
    ASSERT_TRUE(connect_signed(browser, rec.server.base_url()));

    ASSERT_TRUE(browser.exists("hello.txt"));

    // 请求 0 = connect 的桶探测 GET（带 max-keys=1），请求 1 = HEAD 对象
    auto probe = rec.request(0);
    EXPECT_EQ(rec.methods[0], "GET");
    EXPECT_EQ(rec.paths[0], "/testbucket?max-keys=1");
    ASSERT_TRUE(probe.count("authorization") > 0);
    EXPECT_EQ(extract_signature(probe.at("authorization")),
              expected_signature("GET", rec.paths[0], probe));

    auto head = rec.request(1);
    EXPECT_EQ(rec.methods[1], "HEAD");
    EXPECT_EQ(rec.paths[1], "/testbucket/hello.txt");

    // 结构断言：算法 + Credential + scope + SignedHeaders 三件套
    const std::string& auth = head.at("authorization");
    EXPECT_EQ(auth.substr(0, 36), "AWS4-HMAC-SHA256 Credential=test-ak/");
    EXPECT_NE(auth.find("/us-east-1/s3/aws4_request, "), std::string::npos);
    EXPECT_NE(auth.find(
                  "SignedHeaders=host;x-amz-content-sha256;x-amz-date, Signature="),
              std::string::npos);

    // x-amz-* 头形状与载荷哈希（GET/HEAD 空体 = SHA256("")）
    EXPECT_TRUE(is_amz_date_shape(head.at("x-amz-date")));
    EXPECT_EQ(head.at("x-amz-content-sha256"),
              test_sha256_hex(""));

    // 签名精确比对（测试侧独立重导）
    EXPECT_EQ(extract_signature(auth), expected_signature("HEAD", rec.paths[1], head));
}

//==============================================================================
// ListObjectsV2：查询参数规范编码与排序（防二次编码 / 乱序）
//==============================================================================

TEST(S3BrowserAuthTest, ListQueryCanonicalizedInSignature) {
    RecordingServer rec;
    ASSERT_TRUE(rec.server.start());

    S3Browser browser;
    ASSERT_TRUE(connect_signed(browser, rec.server.base_url()));

    // prefix=docs/ 经 url_encode 线上为 docs%2F——签名前必须先解码还原，
    // 规范查询再编码回 %2F；三个参数按编码后字典序排序
    const auto listed = browser.list_directory("docs/");
    (void)listed;

    std::lock_guard<std::mutex> lock(rec.mu);
    ASSERT_GE(rec.paths.size(), size_t{2});
    const auto& path = rec.paths[1];
    EXPECT_EQ(rec.methods[1], "GET");
    EXPECT_NE(path.find("list-type=2"), std::string::npos);
    EXPECT_NE(path.find("prefix=docs%2F"), std::string::npos);
    EXPECT_NE(path.find("max-keys=100"), std::string::npos);

    // 独立重导按"解码→重编码→排序"处理线上一致才可能相等——
    // 客户端若把 docs%2F 原样再编码（%252F）或跳过排序即红
    EXPECT_EQ(extract_signature(rec.headers[1].at("authorization")),
              expected_signature("GET", path, rec.headers[1]));
}

//==============================================================================
// 无凭据：匿名路径保持（无 Authorization，Date/Host 在位，请求可用）
//==============================================================================

TEST(S3BrowserAuthTest, AnonymousWithoutCredentialsKeepsWorking) {
    RecordingServer rec;
    ASSERT_TRUE(rec.server.start());

    S3Browser browser;
    ASSERT_TRUE(browser.connect("s3://" + std::string(kBucket),
                                {{"endpoint", rec.server.base_url()}}));

    ASSERT_TRUE(browser.exists("anon.txt"));

    std::lock_guard<std::mutex> lock(rec.mu);
    ASSERT_GE(rec.headers.size(), size_t{2});
    for (const auto& h : rec.headers) {
        EXPECT_EQ(h.count("authorization"), size_t{0});
        EXPECT_GT(h.count("date"), size_t{0});
        EXPECT_GT(h.count("host"), size_t{0});
    }
}
