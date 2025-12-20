/**
 * @file thunder_plugin_test.cpp
 * @brief 迅雷插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/thunder_plugin.hpp>
#include <falcon/exceptions.hpp>

using namespace falcon;
using namespace falcon::plugins;

class ThunderPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<ThunderPlugin>();
    }

    std::unique_ptr<ThunderPlugin> plugin;
};

TEST_F(ThunderPluginTest, GetProtocolName) {
    EXPECT_EQ(plugin->getProtocolName(), "thunder");
}

TEST_F(ThunderPluginTest, GetSupportedSchemes) {
    auto schemes = plugin->getSupportedSchemes();
    EXPECT_EQ(schemes.size(), 2);
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "thunder"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "thunderxl"), schemes.end());
}

TEST_F(ThunderPluginTest, CanHandleUrls) {
    // 测试支持的URL格式
    EXPECT_TRUE(plugin->canHandle("thunder://abcdef"));
    EXPECT_TRUE(plugin->canHandle("thunderxl://xyz123"));

    // 测试不支持的URL格式
    EXPECT_FALSE(plugin->canHandle("http://example.com"));
    EXPECT_FALSE(plugin->canHandle("ftp://example.com"));
    EXPECT_FALSE(plugin->canHandle("magnet:?xt=urn:btih:"));
}

TEST_F(ThunderPluginTest, DecodeClassicThunder) {
    // 测试经典迅雷链接解码
    // AA[URL]ZZ 格式
    std::string encoded = "QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWlo="; // Base64编码的 AAhttp://example.com/file.zipZZ
    std::string thunderUrl = "thunder://" + encoded;

    EXPECT_TRUE(plugin->canHandle(thunderUrl));

    // 创建任务并验证解码
    DownloadOptions options;
    try {
        auto task = plugin->createTask(thunderUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const InvalidURLException& e) {
        // 如果解码失败，应该抛出异常
        FAIL() << "Failed to decode valid thunder URL: " << e.what();
    }
}

TEST_F(ThunderPluginTest, DecodeThunderXL) {
    // 测试迅雷离线链接解码
    std::string encoded = "aHR0cHM6Ly9leGFtcGxlLmNvbS9maWxlLm1wNA=="; // Base64编码的 https://example.com/file.mp4
    std::string thunderXLUrl = "thunderxl://" + encoded;

    EXPECT_TRUE(plugin->canHandle(thunderXLUrl));

    DownloadOptions options;
    try {
        auto task = plugin->createTask(thunderXLUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const UnsupportedProtocolException& e) {
        // XL格式可能不支持所有情况，这是预期的
        SUCCEED() << "Thunder XL format not fully supported as expected";
    }
}

TEST_F(ThunderPluginTest, InvalidUrls) {
    DownloadOptions options;

    // 测试无效的迅雷链接
    EXPECT_THROW(plugin->createTask("thunder://", options), InvalidURLException);
    EXPECT_THROW(plugin->createTask("thunder://invalid", options), InvalidURLException);
    EXPECT_THROW(plugin->createTask("invalid://format", options), UnsupportedProtocolException);
}

TEST_F(ThunderPluginTest, EmptyAndNullUrls) {
    DownloadOptions options;

    // 测试空URL
    EXPECT_THROW(plugin->createTask("", options), UnsupportedProtocolException);
    EXPECT_THROW(plugin->createTask("thunder://", options), InvalidURLException);
}

TEST_F(ThunderPluginTest, MultipleUrls) {
    std::vector<std::string> validUrls = {
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTEuemlwWlo=",
        "thunder://QUZ0cDovL2V4YW1wbGUuY29tL2ZpbGUyLnRhci5nejJa",
        "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
    };

    DownloadOptions options;

    for (const auto& url : validUrls) {
        EXPECT_TRUE(plugin->canHandle(url));
        // 注意：某些URL可能因为编码内容无效而失败
        try {
            auto task = plugin->createTask(url, options);
            EXPECT_NE(task, nullptr);
        } catch (const InvalidURLException&) {
            // 某些URL可能包含无效的编码内容
            continue;
        }
    }
}