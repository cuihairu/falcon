/**
 * @file ed2k_plugin_test.cpp
 * @brief ED2K插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/ed2k_plugin.hpp>
#include <falcon/exceptions.hpp>

using namespace falcon;
using namespace falcon::plugins;

class ED2KPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<ED2KPlugin>();
    }

    std::unique_ptr<ED2KPlugin> plugin;
};

TEST_F(ED2KPluginTest, GetProtocolName) {
    EXPECT_EQ(plugin->getProtocolName(), "ed2k");
}

TEST_F(ED2KPluginTest, GetSupportedSchemes) {
    auto schemes = plugin->getSupportedSchemes();
    EXPECT_EQ(schemes.size(), 1);
    EXPECT_EQ(schemes[0], "ed2k");
}

TEST_F(ED2KPluginTest, CanHandleUrls) {
    // 测试支持的URL格式
    EXPECT_TRUE(plugin->canHandle("ed2k://file|example.zip|1048576|A1B2C3D4E5F67890|/"));
    EXPECT_TRUE(plugin->canHandle("ed2k://server|server.example.com|4242|/"));

    // 测试不支持的URL格式
    EXPECT_FALSE(plugin->canHandle("http://example.com"));
    EXPECT_FALSE(plugin->canHandle("ftp://example.com"));
    EXPECT_FALSE(plugin->canHandle("magnet:?xt=urn:btih:"));
}

TEST_F(ED2KPluginTest, ParseFileLink) {
    // 测试ED2K文件链接解析
    std::string ed2kUrl = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";

    EXPECT_TRUE(plugin->canHandle(ed2kUrl));

    DownloadOptions options;
    try {
        auto task = plugin->createTask(ed2kUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const InvalidURLException& e) {
        FAIL() << "Failed to parse valid ED2K file URL: " << e.what();
    }
}

TEST_F(ED2KPluginTest, ParseFileLinkWithSources) {
    // 测试带源地址的ED2K链接
    std::string ed2kUrl = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/|s,192.168.1.1:4662|s,192.168.1.2:4662";

    EXPECT_TRUE(plugin->canHandle(ed2kUrl));

    DownloadOptions options;
    try {
        auto task = plugin->createTask(ed2kUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const InvalidURLException& e) {
        FAIL() << "Failed to parse ED2K URL with sources: " << e.what();
    }
}

TEST_F(ED2KPluginTest, ParseFileLinkWithAICH) {
    // 测试带AICH哈希的ED2K链接
    std::string ed2kUrl = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/|h=ABCDEF123456789";

    EXPECT_TRUE(plugin->canHandle(ed2kUrl));

    DownloadOptions options;
    try {
        auto task = plugin->createTask(ed2kUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const InvalidURLException& e) {
        FAIL() << "Failed to parse ED2K URL with AICH: " << e.what();
    }
}

TEST_F(ED2KPluginTest, ParseServerLink) {
    // 测试ED2K服务器链接
    std::string serverUrl = "ed2k://server|server.example.com|4242|MyServer/";

    EXPECT_TRUE(plugin->canHandle(serverUrl));

    DownloadOptions options;
    // 服务器链接应该抛出UnsupportedProtocolException
    EXPECT_THROW(plugin->createTask(serverUrl, options), UnsupportedProtocolException);
}

TEST_F(ED2KPluginTest, InvalidUrls) {
    DownloadOptions options;

    // 测试无效的ED2K链接
    EXPECT_THROW(plugin->createTask("ed2k://", options), InvalidURLException);
    EXPECT_THROW(plugin->createTask("ed2k://file", options), InvalidURLException);
    EXPECT_THROW(plugin->createTask("ed2k://file|incomplete", options), InvalidURLException);

    // 测试无效的哈希长度
    EXPECT_THROW(plugin->createTask("ed2k://file|test.zip|100|ABC|/", options), InvalidURLException);

    // 测试无效的文件大小
    EXPECT_THROW(plugin->createTask("ed2k://file|test.zip|invalid|A1B2C3D4E5F6789012345678901234AB|/", options), InvalidURLException);
}

TEST_F(ED2KPluginTest, UrlEncodedFilenames) {
    // 测试URL编码的文件名
    std::string encodedUrl = "ed2k://file|test%20file.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";

    EXPECT_TRUE(plugin->canHandle(encodedUrl));

    DownloadOptions options;
    try {
        auto task = plugin->createTask(encodedUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const InvalidURLException& e) {
        FAIL() << "Failed to parse ED2K URL with encoded filename: " << e.what();
    }
}

TEST_F(ED2KPluginTest, PriorityParameter) {
    // 测试优先级参数
    std::string priorityUrl = "ed2k://file|priority.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/|p=50";

    EXPECT_TRUE(plugin->canHandle(priorityUrl));

    DownloadOptions options;
    try {
        auto task = plugin->createTask(priorityUrl, options);
        EXPECT_NE(task, nullptr);
    } catch (const InvalidURLException& e) {
        FAIL() << "Failed to parse ED2K URL with priority: " << e.what();
    }
}

TEST_F(ED2KPluginTest, EmptyAndNullUrls) {
    DownloadOptions options;

    // 测试空URL
    EXPECT_THROW(plugin->createTask("", options), UnsupportedProtocolException);
    EXPECT_THROW(plugin->createTask("ed2k://", options), InvalidURLException);
}