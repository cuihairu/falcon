/**
 * @file ed2k_plugin_test.cpp
 * @brief ED2K (eDonkey2000) 协议插件测试
 *
 * 测试对象是 IProtocolHandler 公开接口面：协议名/协议簇/URL 识别/
 * get_file_info 文件链接解析（文件名/大小真实可观测）/ server 链接拒绝 /
 * 未知任务的 pause/cancel 安全空操作 / 注册表路由。
 *
 * 已知解析形态：正则 ^ed2k://(\w+)\|(.+) 要求类型段紧跟 "ed2k://" 之后，
 * 即 "ed2k://file|..." 可解析；通行的 "ed2k://|file|..." 竖线起步形态
 * 不匹配（测试按真实行为钉住）。
 * @date 2026-10-10
 */

#include <gtest/gtest.h>

#include "ed2k_plugin.hpp"

#include <falcon/download_task.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>

#include <algorithm>

using falcon::DownloadOptions;
using falcon::DownloadTask;
using falcon::InvalidURLException;
using falcon::ProtocolRegistry;
using falcon::protocols::ED2KHandler;

namespace {

constexpr const char* kMd4Hash = "0123456789ABCDEF0123456789ABCDEF";
// 类型段紧跟 scheme（无竖线起步），四段齐全：名/大小/哈希/尾部
const std::string kFileLinkUrl =
    "ed2k://file|test.txt|123|" + std::string(kMd4Hash) + "|/";

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

class ED2KPluginTest : public ::testing::Test {
protected:
    ED2KHandler handler_;
};

TEST_F(ED2KPluginTest, ProtocolName) {
    EXPECT_EQ(handler_.protocol_name(), "ed2k");
}

TEST_F(ED2KPluginTest, SupportedSchemes) {
    auto schemes = handler_.supported_schemes();
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), std::string("ed2k")),
              schemes.end());
}

TEST_F(ED2KPluginTest, CanHandleMatrix) {
    EXPECT_TRUE(handler_.can_handle(kFileLinkUrl));
    EXPECT_TRUE(handler_.can_handle("ed2k://server|192.168.1.1:4661"));
    EXPECT_FALSE(handler_.can_handle("http://example.com/file.zip"));
    EXPECT_FALSE(handler_.can_handle("magnet:?xt=urn:btih:abc"));
    EXPECT_FALSE(handler_.can_handle(""));
}

TEST_F(ED2KPluginTest, GetFileInfoParsesFileLink) {
    auto info = handler_.get_file_info(kFileLinkUrl, DownloadOptions{});
    // 文件链接的文件名与大小是真实解析结果，非占位
    EXPECT_EQ(info.filename, "test.txt");
    EXPECT_EQ(info.total_size, 123u);
    EXPECT_EQ(info.url, kFileLinkUrl);
    EXPECT_TRUE(info.supports_resume);
}

TEST_F(ED2KPluginTest, GetFileInfoDecodesFilename) {
    // 文件名走 URL 解码：%20 → 空格
    const std::string url =
        std::string("ed2k://file|my%20file.bin|456|") + kMd4Hash + "|/";
    auto info = handler_.get_file_info(url, DownloadOptions{});
    EXPECT_EQ(info.filename, "my file.bin");
    EXPECT_EQ(info.total_size, 456u);
}

TEST_F(ED2KPluginTest, GetFileInfoRejectsMalformedUrls) {
    // 通行竖线起步形态（ed2k://|file|...）不匹配解析正则——按真实行为钉住
    expect_invalid_url([&] {
        return handler_.get_file_info(
            "ed2k://|file|test.txt|123|" + std::string(kMd4Hash) + "|/",
            DownloadOptions{});
    });
    // 参数不足（缺哈希与尾段）
    expect_invalid_url([&] {
        return handler_.get_file_info("ed2k://file|only|2", DownloadOptions{});
    });
    // 哈希长度非 32
    expect_invalid_url([&] {
        return handler_.get_file_info("ed2k://file|t.txt|1|ABCD|/",
                                      DownloadOptions{});
    });
    // server 链接在本上下文不支持（经 get_file_info 归一为 InvalidURLException）
    expect_invalid_url([&] {
        return handler_.get_file_info("ed2k://server|192.168.1.1:4661",
                                       DownloadOptions{});
    });
    // 非 ed2k 前缀整体拒绝
    expect_invalid_url([&] {
        return handler_.get_file_info("http://example.com/file.zip",
                                       DownloadOptions{});
    });
}

TEST_F(ED2KPluginTest, PauseAndCancelUnknownTaskAreSafeNoOps) {
    auto task = make_task(987654321u, kFileLinkUrl);
    EXPECT_NO_THROW(handler_.pause(task));
    EXPECT_NO_THROW(handler_.resume(task, nullptr));
    EXPECT_NO_THROW(handler_.cancel(task));
}

TEST_F(ED2KPluginTest, SupportsResumeAndPriority) {
    EXPECT_TRUE(handler_.supports_resume());
    EXPECT_EQ(handler_.priority(), 30);
}

TEST_F(ED2KPluginTest, RegistryRoutesEd2kUrlsToHandler) {
    ProtocolRegistry registry;
    registry.register_handler(std::make_unique<ED2KHandler>());
    EXPECT_EQ(registry.get_handler_for_url(kFileLinkUrl)->protocol_name(),
              "ed2k");
    EXPECT_EQ(registry.get_handler_for_url("http://example.com/file.zip"),
              nullptr);
}
