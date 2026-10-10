/**
 * @file hls_plugin_test.cpp
 * @brief HLS/DASH 流媒体协议插件测试
 *
 * 测试对象是 IProtocolHandler 公开接口面：协议名/协议簇/URL 识别/
 * get_file_info 占位契约（流媒体需解析播放列表才知名字与大小）/
 * 未知任务的 pause/cancel 安全空操作 / 注册表路由。
 * 播放列表下载与段合并是私有网络路径，此处只测可离线观测的契约。
 * @date 2026-10-10
 */

#include <gtest/gtest.h>

#include "hls_plugin.hpp"

#include <falcon/download_task.hpp>
#include <falcon/protocol_registry.hpp>

#include <algorithm>

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::ProtocolRegistry;
using falcon::protocols::HLSHandler;

namespace {

std::shared_ptr<DownloadTask> make_task(uint64_t id, const std::string& url) {
    return std::make_shared<DownloadTask>(id, url, DownloadOptions{});
}

}  // namespace

class HLSPluginTest : public ::testing::Test {
protected:
    HLSHandler handler_;
};

TEST_F(HLSPluginTest, ProtocolName) {
    EXPECT_EQ(handler_.protocol_name(), "hls");
}

TEST_F(HLSPluginTest, SupportedSchemes) {
    auto schemes = handler_.supported_schemes();
    for (const char* expected : {"http", "https", "hls", "dash"}) {
        EXPECT_NE(std::find(schemes.begin(), schemes.end(),
                            std::string(expected)),
                  schemes.end())
            << "missing scheme: " << expected;
    }
}

TEST_F(HLSPluginTest, CanHandleMatrix) {
    EXPECT_TRUE(handler_.can_handle("http://example.com/video.m3u8"));
    EXPECT_TRUE(handler_.can_handle("http://example.com/stream.mpd"));
    // 宽松匹配：任意位置含 m3u8/dash 子串即可
    EXPECT_TRUE(handler_.can_handle("http://m3u8.example.com/file"));
    EXPECT_FALSE(handler_.can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler_.can_handle("ftp://example.com/file.bin"));
}

TEST_F(HLSPluginTest, GetFileInfoUsesPlaceholderContract) {
    const std::string url = "http://example.com/video.m3u8";
    auto info = handler_.get_file_info(url, DownloadOptions{});
    EXPECT_EQ(info.url, url);
    // 总大小需解析播放列表才知道，契约是占位名 + 零大小 + 可续传
    EXPECT_EQ(info.filename, "stream");
    EXPECT_EQ(info.total_size, 0u);
    EXPECT_TRUE(info.supports_resume);
}

TEST_F(HLSPluginTest, GetFileInfoNeverThrows) {
    // 识别不依赖解析成功：非流媒体 URL 也不抛（can_handle 为 false 时
    // 不会被路由到本 handler，但直接调用的契约仍是占位返回）
    EXPECT_NO_THROW({
        // 返回值带 [[nodiscard]]，赋值保持"被使用"
        auto info = handler_.get_file_info("http://example.com/file.zip",
                                           DownloadOptions{});
        (void)info;
    });
}

TEST_F(HLSPluginTest, PauseAndCancelUnknownTaskAreSafeNoOps) {
    auto task = make_task(987654321u, "http://example.com/video.m3u8");
    EXPECT_NO_THROW(handler_.pause(task));
    EXPECT_NO_THROW(handler_.resume(task, nullptr));
    EXPECT_NO_THROW(handler_.cancel(task));
}

TEST_F(HLSPluginTest, SupportsResumeAndPriority) {
    EXPECT_TRUE(handler_.supports_resume());
    EXPECT_EQ(handler_.priority(), 45);
}

TEST_F(HLSPluginTest, RegistryRoutesHlsUrlsToHandler) {
    ProtocolRegistry registry;
    registry.register_handler(std::make_unique<HLSHandler>());
    EXPECT_EQ(
        registry.get_handler_for_url("http://example.com/video.m3u8")
            ->protocol_name(),
        "hls");
    EXPECT_EQ(registry.get_handler_for_url("http://example.com/file.zip"),
              nullptr);
}
