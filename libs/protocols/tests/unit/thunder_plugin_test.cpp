/**
 * @file thunder_plugin_test.cpp
 * @brief Thunder 协议插件测试
 *
 * 测试对象是 IProtocolHandler 公开接口面：协议名/协议簇/URL 识别/
 * get_file_info 解析（经典 AA..ZZ Base64 与 thunderxl 直链两种形态）/
 * 未知任务的 pause/cancel 安全空操作 / 注册表路由。
 * 解码函数是私有实现，只能经由 get_file_info 的抛错语义观测。
 * @date 2026-10-10
 */

#include <gtest/gtest.h>

#include "thunder_plugin.hpp"

#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>

#include <algorithm>

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::InvalidURLException;
using falcon::ProtocolRegistry;
using falcon::protocols::ThunderHandler;

namespace {

// 经典迅雷链接：base64("AA" + "http://example.com/file.zip" + "ZZ")
constexpr const char* kClassicThunderUrl =
    "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg==";
// thunderxl 直链：base64("http://example.com/file.zip")
constexpr const char* kXLThunderUrl =
    "thunderxl://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw";
// base64 解出 3 个 NUL 字节：非直链也非 AA..ZZ 形态
constexpr const char* kThunderNulPayload = "thunder://QUFB";

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

class ThunderPluginTest : public ::testing::Test {
protected:
    ThunderHandler handler_;
};

TEST_F(ThunderPluginTest, ProtocolName) {
    EXPECT_EQ(handler_.protocol_name(), "thunder");
}

TEST_F(ThunderPluginTest, SupportedSchemes) {
    auto schemes = handler_.supported_schemes();
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("thunder")),
              schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("thunderxl")),
              schemes.end());
}

TEST_F(ThunderPluginTest, CanHandleMatrix) {
    EXPECT_TRUE(handler_.can_handle("thunder://QUFB"));
    EXPECT_TRUE(handler_.can_handle("thunderxl://QUFB"));
    EXPECT_FALSE(handler_.can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler_.can_handle("flashget://QUFB"));
    EXPECT_FALSE(handler_.can_handle(""));
}

TEST_F(ThunderPluginTest, GetFileInfoClassicUrl) {
    auto info = handler_.get_file_info(kClassicThunderUrl, DownloadOptions{});
    EXPECT_EQ(info.url, kClassicThunderUrl);
    // 文件名与大小无法从迅雷链接直接获取，契约是占位值
    EXPECT_EQ(info.filename, "download");
    EXPECT_EQ(info.total_size, 0u);
    EXPECT_TRUE(info.supports_resume);
}

TEST_F(ThunderPluginTest, GetFileInfoXLUrl) {
    auto info = handler_.get_file_info(kXLThunderUrl, DownloadOptions{});
    EXPECT_EQ(info.url, kXLThunderUrl);
    EXPECT_EQ(info.filename, "download");
    EXPECT_EQ(info.total_size, 0u);
}

TEST_F(ThunderPluginTest, GetFileInfoRejectsMalformedUrls) {
    // 空载荷不匹配 ^(thunder|thunderxl)://(.+)$
    expect_invalid_url(
        [&] { return handler_.get_file_info("thunder://", DownloadOptions{}); });
    // 解码后既不是直链也没有 AA..ZZ 标记
    expect_invalid_url([&] {
        return handler_.get_file_info(kThunderNulPayload, DownloadOptions{});
    });
    // thunderxl 解码结果非 http/https/ftp/magnet 直链
    expect_invalid_url([&] {
        return handler_.get_file_info("thunderxl://QUFB", DownloadOptions{});
    });
    // 非 thunder 前缀整体拒绝
    expect_invalid_url([&] {
        return handler_.get_file_info("http://example.com/file.zip",
                                       DownloadOptions{});
    });
}

TEST_F(ThunderPluginTest, PauseAndCancelUnknownTaskAreSafeNoOps) {
    auto task = make_task(987654321u, kClassicThunderUrl);
    EXPECT_NO_THROW(handler_.pause(task));
    EXPECT_NO_THROW(handler_.resume(task, nullptr));
    EXPECT_NO_THROW(handler_.cancel(task));
}

TEST_F(ThunderPluginTest, SupportsResumeAndPriority) {
    EXPECT_TRUE(handler_.supports_resume());
    EXPECT_EQ(handler_.priority(), 40);
}

TEST_F(ThunderPluginTest, RegistryRoutesThunderUrlsToHandler) {
    ProtocolRegistry registry;
    registry.register_handler(std::make_unique<ThunderHandler>());
    EXPECT_EQ(registry.get_handler_for_url(kClassicThunderUrl)->protocol_name(),
              "thunder");
    EXPECT_EQ(registry.get_handler_for_url("http://example.com/file.zip"),
              nullptr);
}
