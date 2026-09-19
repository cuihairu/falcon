/**
 * @file storage_injection_test.cpp
 * @brief storage 包故障注入测试（falcon::detail::inject_failure）
 * @author Falcon Team
 * @date 2026-09-19
 *
 * 覆盖四云浏览器 curl 句柄创建失败防御与 upyun/cos 请求签名的 EVP
 * 防御链——这些分支在正常执行下不可达（curl/EVP 仅在内存耗尽时失
 * 败），经注入点强制命中。仅测试构建编译（FALCON_FAILURE_INJECTION）。
 */

#include <falcon/detail/injection.hpp>
#include <falcon/storage/cos_browser.hpp>
#include <falcon/storage/kodo_browser.hpp>
#include <falcon/storage/oss_browser.hpp>
#include <falcon/storage/s3_browser.hpp>
#include <falcon/storage/upyun_browser.hpp>

#include <gtest/gtest.h>

#include "mock_http_server.hpp"

#include <map>
#include <stdexcept>
#include <string>

#if defined(FALCON_FAILURE_INJECTION)

using falcon::detail::InjectPoint;
using falcon::detail::ScopedInjection;

namespace {

using MockServer = MockHttpServer;

std::unique_ptr<MockServer> make_server(MockServer::Handler handler) {
    auto server = std::make_unique<MockServer>(std::move(handler));
    if (!server->start()) {
        return nullptr;
    }
    return server;
}

MockServer::Response okReply(const std::string&, const std::string&) {
    return {200, ""};
}

std::map<std::string, std::string> connect_options(const std::string& base) {
    return {{"username", "operator"}, {"password", "pass"},
            {"api_domain", base}};
}

}  // namespace

//==============================================================================
// curl 句柄创建失败：浏览器构造即抛（Impl 构造函数内 curl_easy_init 防御）
//==============================================================================

TEST(StorageInjectionTest, UpyunBrowserConstructorThrowsOnCurlInitFailure) {
    ScopedInjection guard(InjectPoint::CurlEasyInit);
    EXPECT_THROW(falcon::UpyunBrowser browser, std::runtime_error);
}

TEST(StorageInjectionTest, OssBrowserConstructorThrowsOnCurlInitFailure) {
    ScopedInjection guard(InjectPoint::CurlEasyInit);
    EXPECT_THROW(falcon::OSSBrowser browser, std::runtime_error);
}

TEST(StorageInjectionTest, CosBrowserConstructorThrowsOnCurlInitFailure) {
    ScopedInjection guard(InjectPoint::CurlEasyInit);
    EXPECT_THROW(falcon::COSBrowser browser, std::runtime_error);
}

TEST(StorageInjectionTest, KodoBrowserConstructorThrowsOnCurlInitFailure) {
    ScopedInjection guard(InjectPoint::CurlEasyInit);
    EXPECT_THROW(falcon::KodoBrowser browser, std::runtime_error);
}

TEST(StorageInjectionTest, S3BrowserConstructorThrowsOnCurlInitFailure) {
    ScopedInjection guard(InjectPoint::CurlEasyInit);
    EXPECT_THROW(falcon::S3Browser browser, std::runtime_error);
}

//==============================================================================
// upyun 请求签名（MD5 + HMAC-MD5）防御链：签名失败经公开请求路径命中
//==============================================================================

namespace {

/// 签名防御注入后请求仍以空签名发出（mock 一律 200），断言调用不崩
/// 溃——注入点行的执行即覆盖目标
void exercise_upyun_request(falcon::UpyunBrowser& browser) {
    browser.list_directory("/docs/");
}

}  // namespace

TEST(StorageInjectionTest, UpyunSignatureCtxNewFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::UpyunBrowser browser;
    ASSERT_TRUE(browser.connect("upyun://testbucket",
                                connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::UpyunSigCtxNew);
    EXPECT_NO_THROW(exercise_upyun_request(browser));
}

TEST(StorageInjectionTest, UpyunSignatureDigestInitFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::UpyunBrowser browser;
    ASSERT_TRUE(browser.connect("upyun://testbucket",
                                connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::UpyunSigDigestInit);
    EXPECT_NO_THROW(exercise_upyun_request(browser));
}

TEST(StorageInjectionTest, UpyunSignatureDigestUpdateFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::UpyunBrowser browser;
    ASSERT_TRUE(browser.connect("upyun://testbucket",
                                connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::UpyunSigDigestUpdate);
    EXPECT_NO_THROW(exercise_upyun_request(browser));
}

TEST(StorageInjectionTest, UpyunSignatureDigestFinalFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::UpyunBrowser browser;
    ASSERT_TRUE(browser.connect("upyun://testbucket",
                                connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::UpyunSigDigestFinal);
    EXPECT_NO_THROW(exercise_upyun_request(browser));
}

TEST(StorageInjectionTest, UpyunSignatureHmacNullFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::UpyunBrowser browser;
    ASSERT_TRUE(browser.connect("upyun://testbucket",
                                connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::UpyunSigHmacNull);
    EXPECT_NO_THROW(exercise_upyun_request(browser));
}

//==============================================================================
// cos 请求签名（SHA-256）防御链
//==============================================================================

namespace {

void exercise_cos_request(falcon::COSBrowser& browser) {
    browser.list_directory("/docs/");
}

}  // namespace

namespace {

/// cos 连接选项（endpoint path-style 指向 mock，与 mock 测试同形）
std::map<std::string, std::string> cos_connect_options(
    const std::string& base) {
    return {{"secret_id", "id"}, {"secret_key", "key"},
            {"region", "ap-test"}, {"app_id", "12345"}, {"endpoint", base}};
}

}  // namespace

TEST(StorageInjectionTest, CosSignatureCtxNewFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::COSBrowser browser;
    ASSERT_TRUE(browser.connect("cos://testbucket",
                                cos_connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::CosShaCtxNew);
    EXPECT_NO_THROW(exercise_cos_request(browser));
}

TEST(StorageInjectionTest, CosSignatureDigestInitFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::COSBrowser browser;
    ASSERT_TRUE(browser.connect("cos://testbucket",
                                cos_connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::CosShaDigestInit);
    EXPECT_NO_THROW(exercise_cos_request(browser));
}

TEST(StorageInjectionTest, CosSignatureDigestUpdateFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::COSBrowser browser;
    ASSERT_TRUE(browser.connect("cos://testbucket",
                                cos_connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::CosShaDigestUpdate);
    EXPECT_NO_THROW(exercise_cos_request(browser));
}

TEST(StorageInjectionTest, CosSignatureDigestFinalFailureIsAbsorbed) {
    auto server = make_server(okReply);
    ASSERT_NE(server, nullptr);
    falcon::COSBrowser browser;
    ASSERT_TRUE(browser.connect("cos://testbucket",
                                cos_connect_options(server->base_url())));
    ScopedInjection guard(InjectPoint::CosShaDigestFinal);
    EXPECT_NO_THROW(exercise_cos_request(browser));
}

#endif  // FALCON_FAILURE_INJECTION
