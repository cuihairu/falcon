/**
 * @file qqdl_plugin_test.cpp
 * @brief QQ 旋风 (QQDL) 协议插件测试
 *
 * 测试对象是 IProtocolHandler 公开接口面：协议名/协议簇/URL 识别/
 * get_file_info 解析（GID 竖线分隔与 Base64 直链两种形态）/
 * 未知任务的 pause/cancel 安全空操作 / 注册表路由。
 * 解码函数是私有实现，只能经由 get_file_info 的抛错语义观测。
 * @date 2026-10-10
 */

#include <gtest/gtest.h>

#include "qqdl_plugin.hpp"

#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>

#include <algorithm>

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::InvalidURLException;
using falcon::ProtocolRegistry;
using falcon::protocols::QQDLHandler;

namespace {

// qqdl Base64 直链：base64("http://example.com/file.zip")
constexpr const char* kQQDLBase64Url =
    "qqdl://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw";
// base64("hello")：解码成功但提取不出 URL
constexpr const char* kQQDLNonUrlPayload = "qqlink://aGVsbG8=";

std::shared_ptr<DownloadTask> make_task(uint64_t id, const std::string& url) {
    return std::make_shared<DownloadTask>(id, url, DownloadOptions{});
}

// get_file_info 返回值带 [[nodiscard]]，EXPECT_THROW 直接收表达式会触发
// 弃用告警；经 lambda 转发让返回值保持"被使用"
template <typename F>
void expect_invalid_url(F&& call) {
    EXPECT_THROW(call(), InvalidURLException);
}

}  // namespace

class QQDLPluginTest : public ::testing::Test {
protected:
    QQDLHandler handler_;
};

TEST_F(QQDLPluginTest, ProtocolName) {
    EXPECT_EQ(handler_.protocol_name(), "qqdl");
}

TEST_F(QQDLPluginTest, SupportedSchemes) {
    auto schemes = handler_.supported_schemes();
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("qqlink")),
              schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("qqdl")),
              schemes.end());
}

TEST_F(QQDLPluginTest, CanHandleMatrix) {
    EXPECT_TRUE(handler_.can_handle("qqlink://QUFB"));
    EXPECT_TRUE(handler_.can_handle("qqdl://QUFB"));
    EXPECT_FALSE(handler_.can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler_.can_handle("thunder://QUFB"));
    EXPECT_FALSE(handler_.can_handle(""));
}

TEST_F(QQDLPluginTest, GetFileInfoGidForm) {
    // GID 形态：第一段是 GID，第二段取到下一个竖线为 URL
    const std::string url = "qqlink://gid123|http://example.com/file.zip|extra";
    auto info = handler_.get_file_info(url, DownloadOptions{});
    EXPECT_EQ(info.url, url);
    EXPECT_EQ(info.filename, "download");
    EXPECT_EQ(info.total_size, 0u);
    EXPECT_TRUE(info.supports_resume);
}

TEST_F(QQDLPluginTest, GetFileInfoBase64Form) {
    auto info = handler_.get_file_info(kQQDLBase64Url, DownloadOptions{});
    EXPECT_EQ(info.url, kQQDLBase64Url);
    EXPECT_EQ(info.filename, "download");
    EXPECT_EQ(info.total_size, 0u);
}

TEST_F(QQDLPluginTest, GetFileInfoRejectsMalformedUrls) {
    // 空载荷不匹配 ^(qqlink|qqdl)://(.+)$
    expect_invalid_url(
        [&] { return handler_.get_file_info("qqlink://", DownloadOptions{}); });
    // Base64 解码失败的垃圾载荷
    expect_invalid_url(
        [&] { return handler_.get_file_info("qqdl://!!!", DownloadOptions{}); });
    // 解码成功但提取不出含 "://" 的 URL
    expect_invalid_url([&] {
        return handler_.get_file_info(kQQDLNonUrlPayload, DownloadOptions{});
    });
    // 非 qqdl 前缀整体拒绝
    expect_invalid_url([&] {
        return handler_.get_file_info("http://example.com/file.zip",
                                       DownloadOptions{});
    });
}

TEST_F(QQDLPluginTest, PauseAndCancelUnknownTaskAreSafeNoOps) {
    auto task = make_task(987654321u, kQQDLBase64Url);
    EXPECT_NO_THROW(handler_.pause(task));
    EXPECT_NO_THROW(handler_.resume(task, nullptr));
    EXPECT_NO_THROW(handler_.cancel(task));
}

TEST_F(QQDLPluginTest, SupportsResumeAndPriority) {
    EXPECT_TRUE(handler_.supports_resume());
    EXPECT_EQ(handler_.priority(), 35);
}

TEST_F(QQDLPluginTest, RegistryRoutesQQDLUrlsToHandler) {
    ProtocolRegistry registry;
    registry.register_handler(std::make_unique<QQDLHandler>());
    EXPECT_EQ(
        registry.get_handler_for_url(kQQDLBase64Url)->protocol_name(), "qqdl");
    EXPECT_EQ(registry.get_handler_for_url("http://example.com/file.zip"),
              nullptr);
}
