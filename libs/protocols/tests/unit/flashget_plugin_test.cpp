/**
 * @file flashget_plugin_test.cpp
 * @brief 快车 (FlashGet) 协议插件测试
 *
 * 测试对象是 IProtocolHandler 公开接口面：协议名/协议簇/URL 识别/
 * get_file_info 解析（fg:// 短格式直发 URL 解码、flashget:// Base64 与
 * [FLASHGET] 前缀、&ref= 引用页剥离）/
 * 未知任务的 pause/cancel 安全空操作 / 注册表路由。
 * 解码函数是私有实现，只能经由 get_file_info 的抛错语义观测。
 * @date 2026-10-10
 */

#include <gtest/gtest.h>

#include "flashget_plugin.hpp"

#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>

#include <algorithm>

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::InvalidURLException;
using falcon::ProtocolRegistry;
using falcon::protocols::FlashGetHandler;

namespace {

// flashget Base64（带 [FLASHGET] 前缀）：base64("[FLASHGET]http://example.com/file.zip")
constexpr const char* kFlashGetBase64Url =
    "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA==";
// base64("http://example.com")：用于 &ref= 剥离场景
constexpr const char* kFlashGetRefUrl =
    "flashget://aHR0cDovL2V4YW1wbGUuY29t&ref=http://example.com/page";

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

class FlashGetPluginTest : public ::testing::Test {
protected:
    FlashGetHandler handler_;
};

TEST_F(FlashGetPluginTest, ProtocolName) {
    EXPECT_EQ(handler_.protocol_name(), "flashget");
}

TEST_F(FlashGetPluginTest, SupportedSchemes) {
    auto schemes = handler_.supported_schemes();
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("flashget")),
              schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("fg")),
              schemes.end());
}

TEST_F(FlashGetPluginTest, CanHandleMatrix) {
    EXPECT_TRUE(handler_.can_handle("flashget://QUFB"));
    EXPECT_TRUE(handler_.can_handle("fg://http://example.com/file.zip"));
    EXPECT_FALSE(handler_.can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler_.can_handle("qqdl://QUFB"));
    EXPECT_FALSE(handler_.can_handle(""));
}

TEST_F(FlashGetPluginTest, GetFileInfoShortFgForm) {
    // fg:// 短格式：载荷就是 URL 本身（仅做 URL 解码）
    const std::string url = "fg://http%3A%2F%2Fexample.com%2Ffile.zip";
    auto info = handler_.get_file_info(url, DownloadOptions{});
    EXPECT_EQ(info.url, url);
    EXPECT_EQ(info.filename, "download");
    EXPECT_EQ(info.total_size, 0u);
    EXPECT_TRUE(info.supports_resume);
}

TEST_F(FlashGetPluginTest, GetFileInfoBase64Form) {
    auto info = handler_.get_file_info(kFlashGetBase64Url, DownloadOptions{});
    EXPECT_EQ(info.url, kFlashGetBase64Url);
    EXPECT_EQ(info.filename, "download");
    EXPECT_EQ(info.total_size, 0u);
}

TEST_F(FlashGetPluginTest, GetFileInfoStripsRefParam) {
    // &ref= 之后的引用页参数在解码前剥离，Base64 载荷照常解码
    auto info = handler_.get_file_info(kFlashGetRefUrl, DownloadOptions{});
    EXPECT_EQ(info.url, kFlashGetRefUrl);
    EXPECT_EQ(info.filename, "download");
}

TEST_F(FlashGetPluginTest, GetFileInfoRejectsMalformedUrls) {
    // 空载荷不匹配 ^(flashget|fg)://(.+)$
    expect_invalid_url(
        [&] { return handler_.get_file_info("flashget://", DownloadOptions{}); });
    expect_invalid_url(
        [&] { return handler_.get_file_info("fg://", DownloadOptions{}); });
    // 非 flashget/fg 前缀整体拒绝
    expect_invalid_url([&] {
        return handler_.get_file_info("http://example.com/file.zip",
                                       DownloadOptions{});
    });
}

TEST_F(FlashGetPluginTest, PauseAndCancelUnknownTaskAreSafeNoOps) {
    auto task = make_task(987654321u, kFlashGetBase64Url);
    EXPECT_NO_THROW(handler_.pause(task));
    EXPECT_NO_THROW(handler_.resume(task, nullptr));
    EXPECT_NO_THROW(handler_.cancel(task));
}

TEST_F(FlashGetPluginTest, SupportsResumeAndPriority) {
    EXPECT_TRUE(handler_.supports_resume());
    EXPECT_EQ(handler_.priority(), 35);
}

TEST_F(FlashGetPluginTest, RegistryRoutesFlashGetUrlsToHandler) {
    ProtocolRegistry registry;
    registry.register_handler(std::make_unique<FlashGetHandler>());
    EXPECT_EQ(registry.get_handler_for_url(kFlashGetBase64Url)->protocol_name(),
              "flashget");
    EXPECT_EQ(registry.get_handler_for_url("ftp://example.com/file.zip"),
              nullptr);
}
