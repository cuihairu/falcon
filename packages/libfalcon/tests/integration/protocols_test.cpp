/**
 * @file protocols_test.cpp
 * @brief 集成测试：测试所有协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/falcon.hpp>
#include <falcon/plugin_manager.hpp>
#include <thread>
#include <chrono>

using namespace falcon;

class ProtocolsTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine = std::make_unique<DownloadEngine>();

        // 加载所有插件
        engine->loadAllPlugins();

        // 列出支持的协议
        protocols = engine->listSupportedProtocols();
    }

    void TearDown() override {
        engine.reset();
    }

    std::unique_ptr<DownloadEngine> engine;
    std::vector<std::string> protocols;
};

TEST_F(ProtocolsTest, TestPluginRegistration) {
    // 检查插件是否正确注册
    EXPECT_GT(protocols.size(), 0);

    // 应该包含基础协议
    EXPECT_NE(std::find(protocols.begin(), protocols.end(), "http"), protocols.end());

    // 检查私有协议（如果启用）
#ifdef FALCON_ENABLE_THUNDER
    EXPECT_NE(std::find(protocols.begin(), protocols.end(), "thunder"), protocols.end());
#endif
}

TEST_F(ProtocolsTest, TestURLProtocolDetection) {
    struct TestCase {
        std::string url;
        std::string expectedProtocol;
    };

    std::vector<TestCase> testCases = {
        {"http://example.com/file.zip", "http"},
        {"https://example.com/file.zip", "http"},
        {"ftp://example.com/file.zip", "ftp"},

        // 私有协议
        {"thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==", "thunder"},
        {"qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==", "qqdl"},
        {"flashget://W10=", "flashget"},
        {"ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/", "ed2k"},

        // 流媒体
        {"https://example.com/playlist.m3u8", "http"},  // HLS 会被 HTTP 插件处理
        {"https://example.com/manifest.mpd", "http"},   // DASH 会被 HTTP 插件处理

        // BitTorrent
        {"magnet:?xt=urn:btih:test1234567890abcdef", "bittorrent"},
        {"https://example.com/file.torrent", "http"},   // torrent 文件通过 HTTP 下载
    };

    for (const auto& testCase : testCases) {
        bool supported = engine->supportsUrl(testCase.url);
        EXPECT_TRUE(supported) << "URL not supported: " << testCase.url;

        if (supported) {
            // 创建任务但不启动（避免实际下载）
            DownloadOptions options;
            auto task = engine->startDownload(testCase.url, options);
            EXPECT_NE(task, nullptr) << "Failed to create task for: " << testCase.url;

            // 取消任务
            task->cancel();
        }
    }
}

TEST_F(ProtocolsTest, TestInvalidURLs) {
    std::vector<std::string> invalidUrls = {
        "",
        "not-a-url",
        "://missing-protocol",
        "unknown://example.com/file",
    };

    for (const auto& url : invalidUrls) {
        EXPECT_FALSE(engine->supportsUrl(url)) << "Should not support invalid URL: " << url;
    }
}

TEST_F(ProtocolsTest, TestProtocolSchemes) {
    auto schemes = engine->listSupportedSchemes();
    EXPECT_GT(schemes.size(), 0);

    // 应该包含基础方案
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "http"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "https"), schemes.end());
}

TEST_F(ProtocolsTest, TestThunderProtocol) {
#ifdef FALCON_ENABLE_THUNDER
    std::string thunderUrl = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==";

    EXPECT_TRUE(engine->supportsUrl(thunderUrl));

    DownloadOptions options;
    auto task = engine->startDownload(thunderUrl, options);
    EXPECT_NE(task, nullptr);

    // 任务应该解析为HTTP下载
    EXPECT_EQ(task->getStatus(), TaskStatus::Downloading);

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 取消任务
    task->cancel();
#endif
}

TEST_F(ProtocolsTest, TestED2KProtocol) {
#ifdef FALCON_ENABLE_ED2K
    std::string ed2kUrl = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";

    EXPECT_TRUE(engine->supportsUrl(ed2kUrl));

    DownloadOptions options;
    auto task = engine->startDownload(ed2kUrl, options);
    EXPECT_NE(task, nullptr);

    // 取消任务
    task->cancel();
#endif
}

TEST_F(ProtocolsTest, TestHLSProtocol) {
#ifdef FALCON_ENABLE_HLS
    std::string hlsUrl = "https://example.com/playlist.m3u8";

    EXPECT_TRUE(engine->supportsUrl(hlsUrl));

    DownloadOptions options;
    options.output_path = "test_output.mp4";

    auto task = engine->startDownload(hlsUrl, options);
    EXPECT_NE(task, nullptr);

    // HLS 插件应该检测并处理
    // 取消任务
    task->cancel();
#endif
}

TEST_F(ProtocolsTest, TestMultipleSimultaneousDownloads) {
    std::vector<std::string> urls = {
        "http://example.com/file1.zip",
        "https://example.com/file2.zip",
    };

    std::vector<std::shared_ptr<DownloadTask>> tasks;

    for (const auto& url : urls) {
        if (engine->supportsUrl(url)) {
            DownloadOptions options;
            auto task = engine->startDownload(url, options);
            EXPECT_NE(task, nullptr);
            tasks.push_back(task);
        }
    }

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 取消所有任务
    for (auto& task : tasks) {
        task->cancel();
    }
}

TEST_F(ProtocolsTest, TestDownloadOptions) {
    DownloadOptions options;
    options.max_connections = 5;
    options.timeout_seconds = 30;
    options.speed_limit = 1024 * 1024;  // 1 MB/s
    options.user_agent = "Falcon Test/1.0";
    options.headers["Custom-Header"] = "Test-Value";

    std::string url = "http://example.com/test.zip";

    if (engine->supportsUrl(url)) {
        auto task = engine->startDownload(url, options);
        EXPECT_NE(task, nullptr);
        task->cancel();
    }
}

// 性能测试
TEST_F(ProtocolsTest, PerformanceTest) {
    auto start = std::chrono::high_resolution_clock::now();

    // 创建100个任务
    for (int i = 0; i < 100; ++i) {
        std::string url = "http://example.com/test" + std::to_string(i) + ".zip";

        if (engine->supportsUrl(url)) {
            DownloadOptions options;
            auto task = engine->startDownload(url, options);
            EXPECT_NE(task, nullptr);
            task->cancel();
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该能在合理时间内完成（1秒）
    EXPECT_LT(duration.count(), 1000);

    FALCON_LOG_INFO("Created 100 tasks in {} ms", duration.count());
}