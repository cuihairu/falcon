/**
 * @file http_commands_auth_test.cpp
 * @brief V2 引擎 HTTP 401 认证测试（RFC 7617 Basic + RFC 2617/7616 Digest）
 *
 * 防同 bug 自我印证：Digest 期望值全部来自独立工具（python hashlib /
 * RFC 2617 §3.5 官方示例），e2e 用服务端收到的 cnonce/nc 用本地独立
 * EVP 辅助重算 response，不经 build_digest_authorization。
 *
 * @author Falcon Team
 * @date 2026-11-26
 */

#include <gtest/gtest.h>

#include <falcon/protocols/download_engine_v2.hpp>

#include "commands/http_commands_detail.hpp"
#include "scripted_http_server.hpp"

#ifdef FALCON_ENABLE_OPENSSL
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

using namespace falcon;

namespace {

// ---- 测试基建（与 http_commands_edges_test.cpp 同款，TU 本地拷贝） ----

template <typename Pred>
bool wait_for(Pred&& pred, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

class AuthEngineRunner {
public:
    explicit AuthEngineRunner(DownloadEngineV2& engine)
        : engine_(engine), thread_([this] { engine_.run(); }) {}
    ~AuthEngineRunner() {
        if (thread_.joinable()) {
            engine_.force_shutdown();
            thread_.join();
        }
    }
    AuthEngineRunner(const AuthEngineRunner&) = delete;
    AuthEngineRunner& operator=(const AuthEngineRunner&) = delete;
    void shutdown_and_join() {
        engine_.shutdown();
        if (thread_.joinable()) thread_.join();
    }

private:
    DownloadEngineV2& engine_;
    std::thread thread_;
};

bool wait_group_terminal(DownloadEngineV2& engine, RequestGroup* group,
                         int timeout_ms) {
    (void)engine;
    return wait_for(
        [&] {
            const auto st = group->status();
            return st == RequestGroupStatus::COMPLETED ||
                   st == RequestGroupStatus::FAILED;
        },
        timeout_ms);
}

DownloadOptions base_options(const std::string& out_path) {
    DownloadOptions options;
    options.output_filename = out_path;
    options.max_connections = 1;
    options.max_retries = 0;
    return options;
}

std::string make_body(std::size_t size) {
    std::string body;
    body.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        body += static_cast<char>('a' + (i % 26));
    }
    return body;
}

std::string read_file_content(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

// FakeResponse 12 成员全量 positional init（-Wmissing-field-initializers 防线）
testscripts::FakeResponse make_response(
    int status, const char* status_text,
    std::vector<std::pair<std::string, std::string>> headers,
    const std::string& body) {
    return testscripts::FakeResponse{status,
                                     status_text,
                                     std::move(headers),
                                     body,
                                     false,
                                     {},
                                     false,
                                     false,
                                     false,
                                     false,
                                     0,
                                     {}};
}

// ---- RFC 2617 §3.5 官方示例常量（期望值全部独立重导） ----

constexpr const char* kUser = "Mufasa";
constexpr const char* kRealm = "testrealm@host.com";
constexpr const char* kPass = "Circle Of Life";
constexpr const char* kNonce = "dcd98b7102dd2f0e8b11d0f600bfb0c093";
constexpr const char* kCnonce = "0a4f113b";
constexpr const char* kUri = "/dir/index.html";
constexpr const char* kOpaque = "5ccc069c403ebaf9f0171e9517f40e41";

// python hashlib 独立预计算（见工作汇报）
constexpr const char* kBasicB64 = "TXVmYXNhOkNpcmNsZSBPZiBMaWZl";
constexpr const char* kMd5Qop = "6629fae49393a05397450978507c4ef1";  // RFC 2617 §3.5 示例
constexpr const char* kMd5Legacy = "670fd8c2df070c60b045671b8b24ff02";
constexpr const char* kMd5Sess = "8e3825c57e897f5a0dec6c2d4e5059d0";
constexpr const char* kSha256Qop =
    "5abdd07184ba512a22c53f41470e5eea7dcaa3a93a59b630c13dfe0a5dc6e38b";
constexpr const char* kSha256Legacy =
    "e71f89d8267982ee1cd4dfb3637698eaf2f55848fe056aee7be175262aab5d2a";
constexpr const char* kSha256Sess =
    "b8822e12417cb7750f4e2b8515f0dcf25b7dd26993e80bee1426201446a7f59b";

// RFC 2617 §3.5 示例挑战（无 algorithm → 默认 MD5）
std::string rfc2617_challenge() {
    return std::string("Digest realm=\"") + kRealm +
           "\", qop=\"auth,auth-int\", nonce=\"" + kNonce + "\", opaque=\"" +
           kOpaque + "\"";
}

#ifdef FALCON_ENABLE_OPENSSL

// 本地独立 EVP 摘要（不触碰被测 build_digest_authorization）
std::string local_digest_hex(const char* algo, const std::string& data) {
    const EVP_MD* md = EVP_get_digestbyname(algo);
    if (!md) return {};
    unsigned char buf[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_Digest(data.data(), data.size(), buf, &len, md, nullptr) != 1) {
        return {};
    }
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < len; ++i) {
        out += kHex[(buf[i] >> 4) & 0xfu];
        out += kHex[buf[i] & 0xfu];
    }
    return out;
}

// e2e 用：按线上收到的 cnonce/nc 独立重算 Digest response
std::string expected_response_local(const std::string& cnonce,
                                    const std::string& nc_hex,
                                    const std::string& uri) {
    const std::string ha1 =
        local_digest_hex("MD5", std::string(kUser) + ":" + kRealm + ":" + kPass);
    const std::string ha2 = local_digest_hex("MD5", "GET:" + uri);
    return local_digest_hex("MD5",
                            ha1 + ":" + kNonce + ":" + nc_hex + ":" + cnonce +
                                ":auth:" + ha2);
}

#endif  // FALCON_ENABLE_OPENSSL

// e2e 用：从 Authorization 头值提取 name="value" 参数（独立迷你解析）
std::string extract_quoted_param(const std::string& header,
                                 const std::string& name) {
    const std::string needle = name + "=\"";
    auto pos = header.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string out;
    while (pos < header.size()) {
        if (header[pos] == '\\' && pos + 1 < header.size()) {
            out += header[pos + 1];
            pos += 2;
            continue;
        }
        if (header[pos] == '"') break;
        out += header[pos++];
    }
    return out;
}

// e2e 用：提取不带引号的 token 参数（如 nc=00000001、algorithm=MD5）
std::string extract_token_param(const std::string& header,
                                const std::string& name) {
    const std::string needle = name + "=";
    auto pos = header.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    std::string out;
    while (pos < header.size() &&
           static_cast<unsigned char>(header[pos]) > ' ' && header[pos] != ',') {
        out += header[pos++];
    }
    return out;
}

}  // namespace

// ============================ Basic / 解析辅助 ============================

TEST(AuthDetailHelpers, Base64EncodeRfc4648Vectors) {
    // RFC 4648 §10 测试向量
    EXPECT_EQ(protocols::detail::base64_encode(""), "");
    EXPECT_EQ(protocols::detail::base64_encode("f"), "Zg==");
    EXPECT_EQ(protocols::detail::base64_encode("fo"), "Zm8=");
    EXPECT_EQ(protocols::detail::base64_encode("foo"), "Zm9v");
    EXPECT_EQ(protocols::detail::base64_encode("foob"), "Zm9vYg==");
    EXPECT_EQ(protocols::detail::base64_encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(protocols::detail::base64_encode("foobar"), "Zm9vYmFy");
}

TEST(AuthDetailHelpers, BuildBasicAuthorization) {
    EXPECT_EQ(protocols::detail::build_basic_authorization(kUser, kPass),
              std::string("Basic ") + kBasicB64);
    // 空凭据形态（挑战存在时引擎按 aria2 语义空凭据重发）
    EXPECT_EQ(protocols::detail::build_basic_authorization("", ""), "Basic Og==");
}

TEST(AuthDetailHelpers, ParseWwwAuthenticateQuoteAwareSplit) {
    const auto c = protocols::detail::parse_www_authenticate(
        "Digest realm=\"a,b\", qop=\"auth,auth-int\", nonce=\"n1,n2\"");
    EXPECT_EQ(c.scheme, "digest");
    ASSERT_TRUE(c.params.count("realm"));
    EXPECT_EQ(c.params.at("realm"), "a,b");  // 引号内逗号不是分隔符
    ASSERT_TRUE(c.params.count("qop"));
    EXPECT_EQ(c.params.at("qop"), "auth,auth-int");
    ASSERT_TRUE(c.params.count("nonce"));
    EXPECT_EQ(c.params.at("nonce"), "n1,n2");
}

TEST(AuthDetailHelpers, ParseWwwAuthenticateEscapedQuotes) {
    const auto c = protocols::detail::parse_www_authenticate(
        "Digest realm=\"say \\\"hi\\\"\", x=1");
    EXPECT_EQ(c.scheme, "digest");
    EXPECT_EQ(c.params.at("realm"), "say \"hi\"");  // quoted-pair 还原
    EXPECT_EQ(c.params.at("x"), "1");
}

TEST(AuthDetailHelpers, ParseWwwAuthenticateSchemeOnlyAndKeyCase) {
    const auto ntlm = protocols::detail::parse_www_authenticate("NTLM");
    EXPECT_EQ(ntlm.scheme, "ntlm");
    EXPECT_TRUE(ntlm.params.empty());

    const auto basic = protocols::detail::parse_www_authenticate("Basic Realm=\"r\"");
    EXPECT_EQ(basic.scheme, "basic");
    ASSERT_TRUE(basic.params.count("realm"));  // 键统一小写
    EXPECT_EQ(basic.params.at("realm"), "r");
}

TEST(AuthDetailHelpers, EscapeQuotedStringEscapesBackslashAndQuote) {
    EXPECT_EQ(protocols::detail::escape_quoted_string("a\"b\\c"), "a\\\"b\\\\c");
    EXPECT_EQ(protocols::detail::escape_quoted_string("plain"), "plain");
}

TEST(AuthDetailHelpers, DigestRequestUriVariants) {
    EXPECT_EQ(protocols::detail::digest_request_uri("http://h:8080/dir/index.html"),
              "/dir/index.html");
    EXPECT_EQ(protocols::detail::digest_request_uri("/origin-form"), "/origin-form");
    EXPECT_EQ(protocols::detail::digest_request_uri("https://host"), "/");
    EXPECT_EQ(protocols::detail::digest_request_uri(""), "/");
}

#ifdef FALCON_ENABLE_OPENSSL

// ============================ Digest 授权构造 ============================

TEST(DigestAuthorization, Md5QopMatchesRfc2617Example) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(rfc2617_challenge()), kUser, kPass,
        "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    // RFC 2617 §3.5 官方示例的完整期望头（字段序 + 各字段值）
    EXPECT_EQ(header,
              std::string("Digest username=\"") + kUser + "\", realm=\"" +
                  kRealm + "\", nonce=\"" + kNonce + "\", uri=\"" + kUri +
                  "\", response=\"" + kMd5Qop + "\", opaque=\"" + kOpaque +
                  "\", qop=auth, nc=00000001, cnonce=\"" + kCnonce + "\"");
}

TEST(DigestAuthorization, Md5LegacyNoQop) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(
            std::string("Digest realm=\"") + kRealm + "\", nonce=\"" + kNonce +
            "\""),
        kUser, kPass, "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("response=\"" + std::string(kMd5Legacy) + "\""),
              std::string::npos);
    // 旧式无 qop：不携带 qop/nc/cnonce
    EXPECT_EQ(header.find("qop="), std::string::npos);
    EXPECT_EQ(header.find("nc="), std::string::npos);
    EXPECT_EQ(header.find("cnonce"), std::string::npos);
}

TEST(DigestAuthorization, Md5Sess) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(
            std::string("Digest realm=\"") + kRealm +
            "\", qop=\"auth\", nonce=\"" + kNonce + "\", algorithm=MD5-sess"),
        kUser, kPass, "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("response=\"" + std::string(kMd5Sess) + "\""),
              std::string::npos);
    EXPECT_NE(header.find(", algorithm=MD5-sess"), std::string::npos);
}

TEST(DigestAuthorization, Sha256Qop) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(
            std::string("Digest realm=\"") + kRealm +
            "\", qop=\"auth\", nonce=\"" + kNonce +
            "\", opaque=\"" + kOpaque + "\", algorithm=SHA-256"),
        kUser, kPass, "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("response=\"" + std::string(kSha256Qop) + "\""),
              std::string::npos);
    EXPECT_NE(header.find(", algorithm=SHA-256"), std::string::npos);
    EXPECT_NE(header.find("opaque=\"" + std::string(kOpaque) + "\""),
              std::string::npos);
}

TEST(DigestAuthorization, Sha256Legacy) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(
            std::string("Digest realm=\"") + kRealm + "\", nonce=\"" + kNonce +
            "\", algorithm=SHA-256"),
        kUser, kPass, "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("response=\"" + std::string(kSha256Legacy) + "\""),
              std::string::npos);
}

TEST(DigestAuthorization, Sha256Sess) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(
            std::string("Digest realm=\"") + kRealm +
            "\", qop=\"auth\", nonce=\"" + kNonce +
            "\", algorithm=SHA-256-sess"),
        kUser, kPass, "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("response=\"" + std::string(kSha256Sess) + "\""),
              std::string::npos);
    EXPECT_NE(header.find(", algorithm=SHA-256-sess"), std::string::npos);
}

TEST(DigestAuthorization, AlgorithmCaseInsensitiveButEchoedVerbatim) {
    // 小写挑战值：同一 response 向量，algorithm 逐字回显小写
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(
            std::string("Digest realm=\"") + kRealm +
            "\", qop=\"auth\", nonce=\"" + kNonce + "\", algorithm=sha-256"),
        kUser, kPass, "GET", kUri, kCnonce);
    ASSERT_FALSE(header.empty());
    EXPECT_NE(header.find("response=\"" + std::string(kSha256Qop) + "\""),
              std::string::npos);
    EXPECT_NE(header.find(", algorithm=sha-256"), std::string::npos);
}

TEST(DigestAuthorization, UnsupportedChallengesReturnEmpty) {
    // 未知算法
    EXPECT_EQ(protocols::detail::build_digest_authorization(
                  protocols::detail::parse_www_authenticate(
                      std::string("Digest realm=\"r\", nonce=\"n\", "
                                  "algorithm=SHA-512-256")),
                  kUser, kPass, "GET", kUri, kCnonce),
              "");
    // qop 菜单不含 auth
    EXPECT_EQ(protocols::detail::build_digest_authorization(
                  protocols::detail::parse_www_authenticate(
                      "Digest realm=\"r\", nonce=\"n\", qop=\"auth-int\""),
                  kUser, kPass, "GET", kUri, kCnonce),
              "");
    // 无 nonce
    EXPECT_EQ(protocols::detail::build_digest_authorization(
                  protocols::detail::parse_www_authenticate("Digest realm=\"r\""),
                  kUser, kPass, "GET", kUri, kCnonce),
              "");
    // scheme 非 digest
    EXPECT_EQ(protocols::detail::build_digest_authorization(
                  protocols::detail::parse_www_authenticate("NTLM"), kUser, kPass, "GET",
                  kUri, kCnonce),
              "");
}

TEST(DigestAuthorization, NcCounterFormats) {
    const auto header = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(rfc2617_challenge()), kUser, kPass,
        "GET", kUri, kCnonce, 255);
    EXPECT_NE(header.find("nc=000000ff"), std::string::npos);
    const auto header2 = protocols::detail::build_digest_authorization(
        protocols::detail::parse_www_authenticate(rfc2617_challenge()), kUser, kPass,
        "GET", kUri, kCnonce, 0x1999);
    EXPECT_NE(header2.find("nc=00001999"), std::string::npos);
}

// ============================ 401 认证 e2e ============================

TEST(AuthEndToEnd, DigestChallengeResendsAndCompletes) {
    testscripts::TempDir dir;
    const std::string out_path = (dir.path() / "digest.bin").string();
    const std::string body = make_body(4096);

    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string path = "/auth/digest.bin";
    server.set_script(path,
                      {make_response(401, "Unauthorized",
                                     {{"WWW-Authenticate",
                                       std::string("Digest realm=\"") +
                                           kRealm +
                                           "\", qop=\"auth,auth-int\", "
                                           "nonce=\"" +
                                           kNonce + "\", opaque=\"" + kOpaque +
                                           "\", algorithm=MD5"}},
                                     "auth required"),
                       make_response(200, "OK", {}, body)});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.http_username = kUser;
    options.http_password = kPass;
    const TaskId task_id = engine.add_download(server.url(path), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    AuthEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    runner.shutdown_and_join();

    // 401 → 同连接重发：恰好 2 次 GET、1 条连接、成品逐字节一致
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);
    EXPECT_EQ(server.count_requests(path, "GET"), 2u);
    EXPECT_EQ(server.connections(), 1);
    EXPECT_EQ(read_file_content(out_path), body);

    // 从线上 Authorization 头提取字段，独立重算 response
    const auto requests = server.requests();
    ASSERT_EQ(requests.size(), 2u);
    const auto auth_it = requests[1].headers.find("authorization");
    ASSERT_NE(auth_it, requests[1].headers.end());
    const std::string& auth = auth_it->second;
    EXPECT_EQ(auth.substr(0, 7), "Digest ");
    EXPECT_EQ(extract_quoted_param(auth, "username"), kUser);
    EXPECT_EQ(extract_quoted_param(auth, "realm"), kRealm);
    EXPECT_EQ(extract_quoted_param(auth, "nonce"), kNonce);
    EXPECT_EQ(extract_quoted_param(auth, "opaque"), kOpaque);
    EXPECT_EQ(extract_quoted_param(auth, "uri"), path);
    EXPECT_EQ(extract_token_param(auth, "algorithm"), "MD5");
    EXPECT_EQ(extract_token_param(auth, "qop"), "auth");
    const std::string nc = extract_token_param(auth, "nc");
    EXPECT_EQ(nc, "00000001");
    const std::string cnonce = extract_quoted_param(auth, "cnonce");
    EXPECT_EQ(cnonce.size(), 16u);
    EXPECT_EQ(extract_quoted_param(auth, "response"),
              expected_response_local(cnonce, nc, path));
}

#endif  // FALCON_ENABLE_OPENSSL（Digest e2e 需要摘要计算；Basic e2e 与
        // 通用 401 失败收口不受构建配置门控）

TEST(AuthEndToEnd, BasicChallengeResendsWithMufasaCredentials) {
    testscripts::TempDir dir;
    const std::string out_path = (dir.path() / "basic.bin").string();
    const std::string body = make_body(2048);

    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string path = "/auth/basic.bin";
    server.set_script(path,
                      {make_response(
                           401, "Unauthorized",
                           {{"WWW-Authenticate", "Basic realm=\"falcon\""}},
                           "auth required"),
                       make_response(200, "OK", {}, body)});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.http_username = kUser;
    options.http_password = kPass;
    const TaskId task_id = engine.add_download(server.url(path), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    AuthEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    runner.shutdown_and_join();

    ASSERT_EQ(group->status(), RequestGroupStatus::COMPLETED)
        << group->error_message();
    EXPECT_EQ(server.count_requests(path, "GET"), 2u);
    EXPECT_EQ(server.connections(), 1);
    EXPECT_EQ(read_file_content(out_path), body);

    const auto requests = server.requests();
    const auto auth_it = requests[1].headers.find("authorization");
    ASSERT_NE(auth_it, requests[1].headers.end());
    EXPECT_EQ(auth_it->second, std::string("Basic ") + kBasicB64);
}

TEST(AuthEndToEnd, Second401AfterAuthFailsWithOnceGuard) {
    testscripts::TempDir dir;
    const std::string out_path = (dir.path() / "wrong.bin").string();

    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string path = "/auth/wrong.bin";
    // 两条剧本：首 GET 消费第一条（401 Basic），重发命中第二条（再 401）
    server.set_script(
        path,
        {make_response(401, "Unauthorized",
                       {{"WWW-Authenticate", "Basic realm=\"falcon\""}},
                       "auth required"),
         make_response(401, "Unauthorized",
                       {{"WWW-Authenticate", "Basic realm=\"falcon\""}},
                       "still unauthorized")});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.http_username = kUser;
    options.http_password = "definitely-wrong";
    const TaskId task_id = engine.add_download(server.url(path), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    AuthEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    runner.shutdown_and_join();

    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("401"), std::string::npos)
        << group->error_message();
    EXPECT_EQ(server.count_requests(path, "GET"), 2u);
    EXPECT_FALSE(fs::exists(out_path));
}

TEST(AuthEndToEnd, NoChallengeNoCredentialsFailsCleanly) {
    testscripts::TempDir dir;
    const std::string out_path = (dir.path() / "noauth.bin").string();

    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string path = "/auth/noauth.bin";
    // 401 且无 WWW-Authenticate + 无凭据 → 直接失败收口
    server.set_script(path, {make_response(401, "Unauthorized", {}, "denied")});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    const TaskId task_id =
        engine.add_download(server.url(path), base_options(out_path));
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    AuthEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    runner.shutdown_and_join();

    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("no credentials"),
              std::string::npos)
        << group->error_message();
    EXPECT_EQ(server.count_requests(path, "GET"), 1u);
    EXPECT_FALSE(fs::exists(out_path));
}

TEST(AuthEndToEnd, NtlmChallengeFailsUnsupportedScheme) {
    testscripts::TempDir dir;
    const std::string out_path = (dir.path() / "ntlm.bin").string();

    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string path = "/auth/ntlm.bin";
    server.set_script(
        path, {make_response(401, "Unauthorized",
                             {{"WWW-Authenticate", "NTLM TlRMTVNTUAACAAAA"}},
                             "auth required")});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.http_username = kUser;
    options.http_password = kPass;
    const TaskId task_id = engine.add_download(server.url(path), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    AuthEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    runner.shutdown_and_join();

    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("unsupported authentication scheme"),
              std::string::npos)
        << group->error_message();
    EXPECT_FALSE(fs::exists(out_path));
}

#ifdef FALCON_ENABLE_OPENSSL

TEST(AuthEndToEnd, UnsupportedDigestAlgorithmFailsCleanly) {
    testscripts::TempDir dir;
    const std::string out_path = (dir.path() / "baddigest.bin").string();

    testscripts::ScriptedHttpServer server;
    server.start();
    const std::string path = "/auth/baddigest.bin";
    server.set_script(path,
                      {make_response(
                          401, "Unauthorized",
                          {{"WWW-Authenticate",
                            "Digest realm=\"r\", nonce=\"" +
                                std::string(kNonce) +
                                "\", qop=\"auth\", algorithm=SHA-512-256"}},
                          "auth required")});

    EngineConfigV2 config;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options = base_options(out_path);
    options.http_username = kUser;
    options.http_password = kPass;
    const TaskId task_id = engine.add_download(server.url(path), options);
    ASSERT_GT(task_id, 0u);
    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);

    AuthEngineRunner runner(engine);
    ASSERT_TRUE(wait_group_terminal(engine, group, 30000));
    runner.shutdown_and_join();

    ASSERT_EQ(group->status(), RequestGroupStatus::FAILED);
    EXPECT_NE(group->error_message().find("unsupported Digest challenge"),
              std::string::npos)
        << group->error_message();
    EXPECT_FALSE(fs::exists(out_path));
}

#endif  // FALCON_ENABLE_OPENSSL
