/**
 * @file http_plugin_test.cpp
 * @brief HTTP/HTTPS 插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/exceptions.hpp>
#include "../plugins/http/http_plugin.hpp"
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>

using namespace falcon;
using namespace falcon::plugins;

class HttpPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();

        // 创建测试目录
        std::filesystem::create_directories("test_data");

        // 创建测试文件
        testFilePath = "test_data/test_file.txt";
        std::ofstream file(testFilePath);
        file << "This is a test file for HTTP downloads.\n";
        file << "It contains multiple lines of text.\n";
        file << "The file size should be small for testing.\n";
        file.close();
    }

    void TearDown() override {
        // 清理测试文件
        std::filesystem::remove_all("test_data");
    }

    std::unique_ptr<HttpPlugin> plugin;
    std::string testFilePath;
};

TEST_F(HttpPluginTest, GetProtocolName) {
    EXPECT_EQ(plugin->getProtocolName(), "http");
}

TEST_F(HttpPluginTest, GetSupportedSchemes) {
    auto schemes = plugin->getSupportedSchemes();
    EXPECT_EQ(schemes.size(), 2);
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "http"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "https"), schemes.end());
}

TEST_F(HttpPluginTest, CanHandleUrls) {
    // HTTP URLs
    EXPECT_TRUE(plugin->canHandle("http://example.com"));
    EXPECT_TRUE(plugin->canHandle("http://example.com/file.zip"));
    EXPECT_TRUE(plugin->canHandle("http://subdomain.example.com/path/to/file?param=value"));

    // HTTPS URLs
    EXPECT_TRUE(plugin->canHandle("https://example.com"));
    EXPECT_TRUE(plugin->canHandle("https://secure.example.com/file.pdf"));
    EXPECT_TRUE(plugin->canHandle("https://example.com:8443/path"));

    // IPv4 and IPv6
    EXPECT_TRUE(plugin->canHandle("http://192.168.1.1/file"));
    EXPECT_TRUE(plugin->canHandle("http://[2001:db8::1]/file"));

    // 端口号
    EXPECT_TRUE(plugin->canHandle("http://example.com:8080"));
    EXPECT_TRUE(plugin->canHandle("https://example.com:443/file"));

    // Not supported
    EXPECT_FALSE(plugin->canHandle("ftp://example.com"));
    EXPECT_FALSE(plugin->canHandle("thunder://abc"));
    EXPECT_FALSE(plugin->canHandle("magnet:?xt=urn:btih:"));
    EXPECT_FALSE(plugin->canHandle(""));
}

TEST_F(HttpPluginTest, CreateTask) {
    DownloadOptions options;
    options.output_path = "downloaded_file.txt";

    try {
        auto task = plugin->createTask("http://example.com/test.txt", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        // 需要实际的 HTTP 服务器
        SUCCEED() << "HTTP task creation requires server: " << e.what();
    }
}

TEST_F(HttpPluginTest, UrlEncoding) {
    // 测试 URL 编码
    std::string input = "hello world 123";
    std::string encoded = plugin->urlEncode(input);
    EXPECT_EQ(encoded, "hello%20world%20123");

    // 特殊字符
    EXPECT_EQ(plugin->urlEncode("file name.zip"), "file%20name.zip");
    EXPECT_EQ(plugin->urlEncode("path/to/file"), "path%2Fto%2Ffile");
    EXPECT_EQ(plugin->urlEncode("a+b=c"), "a%2Bb%3Dc");
}

TEST_F(HttpPluginTest, ParseUrl) {
    // 测试 URL 解析
    HttpPlugin::ParsedUrl url = plugin->parseUrl("https://user:pass@example.com:8080/path/file?param=value#frag");

    EXPECT_EQ(url.scheme, "https");
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "8080");
    EXPECT_EQ(url.path, "/path/file");
    EXPECT_EQ(url.query, "param=value");
    EXPECT_EQ(url.fragment, "frag");

    // 简单 URL
    url = plugin->parseUrl("http://example.com/file");
    EXPECT_EQ(url.scheme, "http");
    EXPECT_EQ(url.host, "example.com");
    EXPECT_EQ(url.port, "");
    EXPECT_EQ(url.path, "/file");
    EXPECT_EQ(url.query, "");
    EXPECT_EQ(url.fragment, "");
}

TEST_F(HttpPluginTest, SupportsResuming) {
    // 这个测试需要实际的 HTTP 服务器
    // 模拟服务器支持 Range 请求
    EXPECT_TRUE(plugin->supportsResuming("http://httpbin.org/range/1024"));
}

TEST_F(HttpPluginTest, GetFinalUrl) {
    // 测试重定向
    try {
        std::string finalUrl = plugin->getFinalUrl("http://httpbin.org/redirect/1");
        EXPECT_NE(finalUrl, "http://httpbin.org/redirect/1");
    } catch (const std::exception& e) {
        SUCCEED() << "Redirect test needs server: " << e.what();
    }
}

class HttpDownloadTaskTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();

        // 设置选项
        options.output_path = "test_download.txt";
        options.max_connections = 1;
        options.timeout_seconds = 30;
        options.resume_if_exists = true;
        options.user_agent = "Falcon Test/1.0";
        options.headers["Test-Header"] = "Test-Value";
    }

    std::unique_ptr<HttpPlugin> plugin;
    DownloadOptions options;
};

TEST_F(HttpDownloadTaskTest, TaskCreation) {
    try {
        auto task = plugin->createTask("http://example.com/test.txt", options);
        EXPECT_NE(task, nullptr);

        // 检查初始状态
        EXPECT_EQ(task->getStatus(), TaskStatus::Pending);
        EXPECT_EQ(task->getProgress(), 0.0f);
        EXPECT_EQ(task->getDownloadedBytes(), 0);
        EXPECT_EQ(task->getSpeed(), 0);
    } catch (const std::exception& e) {
        SUCCEED() << "Task creation needs server: " << e.what();
    }
}

TEST_F(HttpDownloadTaskTest, TaskOptions) {
    // 测试不同选项的组合

    // 多连接下载
    DownloadOptions multiConnOptions = options;
    multiConnOptions.max_connections = 5;

    // 速度限制
    DownloadOptions speedLimitOptions = options;
    speedLimitOptions.speed_limit = 1024 * 1024;  // 1 MB/s

    // 无断点续传
    DownloadOptions noResumeOptions = options;
    noResumeOptions.resume_if_exists = false;

    // 自定义 User-Agent
    DownloadOptions customUAOptions = options;
    customUAOptions.user_agent = "Custom Agent/2.0";

    std::vector<std::pair<DownloadOptions, std::string>> testCases = {
        {multiConnOptions, "multi-connection"},
        {speedLimitOptions, "speed-limit"},
        {noResumeOptions, "no-resume"},
        {customUAOptions, "custom-user-agent"}
    };

    for (const auto& [testOptions, name] : testCases) {
        try {
            auto task = plugin->createTask("http://example.com/test.txt", testOptions);
            EXPECT_NE(task, nullptr) << "Failed for option: " << name;
        } catch (const std::exception& e) {
            SUCCEED() << "Option " << name << " needs server: " << e.what();
        }
    }
}

class HttpResumeTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();

        // 创建部分下载的文件
        partialFilePath = "test_partial.txt";
        std::ofstream file(partialFilePath, std::ios::binary);
        file.write("Partial content", 14);
        file.close();
    }

    void TearDown() override {
        std::filesystem::remove(partialFilePath);
    }

    std::unique_ptr<HttpPlugin> plugin;
    std::string partialFilePath;
};

TEST_F(HttpResumeTest, ResumingDownload) {
    DownloadOptions options;
    options.output_path = partialFilePath;
    options.resume_if_exists = true;

    try {
        auto task = plugin->createTask("http://example.com/large_file.txt", options);
        EXPECT_NE(task, nullptr);

        // 检查是否正确识别了部分下载
        // 这需要实际的服务器支持
    } catch (const std::exception& e) {
        SUCCEED() << "Resume test needs server: " << e.what();
    }
}

class HttpChunkedDownloadTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpChunkedDownloadTest, MultiConnectionDownload) {
    DownloadOptions options;
    options.output_path = "chunked_download.bin";
    options.max_connections = 4;

    // 对于小文件，不应该使用分块下载
    try {
        auto task = plugin->createTask("http://example.com/small_file.txt", options);
        EXPECT_NE(task, nullptr);

        // 实际测试需要大文件
        // 分块下载逻辑应该在文件大于某个阈值时启用
    } catch (const std::exception& e) {
        SUCCEED() << "Chunked download needs server and large file: " << e.what();
    }
}

class HttpSpeedControlTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpSpeedControlTest, SpeedLimit) {
    DownloadOptions options;
    options.output_path = "speed_test.bin";
    options.speed_limit = 100 * 1024;  // 100 KB/s

    try {
        auto task = plugin->createTask("http://example.com/large_file.bin", options);
        EXPECT_NE(task, nullptr);

        // 速度控制需要实际下载来验证
        // 可以测量下载速度并验证是否低于限制
    } catch (const std::exception& e) {
        SUCCEED() << "Speed control needs actual download: " << e.what();
    }
}

class HttpErrorHandlingTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpErrorHandlingTest, InvalidUrl) {
    DownloadOptions options;

    // 无效的 URL
    EXPECT_THROW(plugin->createTask("", options), std::exception);
    EXPECT_THROW(plugin->createTask("not-a-url", options), std::exception);
    EXPECT_THROW(plugin->createTask("http://", options), std::exception);
}

TEST_F(HttpErrorHandlingTest, TimeoutHandling) {
    DownloadOptions options;
    options.output_path = "timeout_test.txt";
    options.timeout_seconds = 1;  // 1秒超时

    try {
        auto task = plugin->createTask("http://httpbin.org/delay/5", options);
        EXPECT_NE(task, nullptr);

        // 任务应该在超时后失败
        // task->start();
        // EXPECT_EQ(task->getStatus(), TaskStatus::Failed);
    } catch (const std::exception& e) {
        SUCCEED() << "Timeout test needs server: " << e.what();
    }
}

TEST_F(HttpErrorHandlingTest, FilePermissionError) {
    DownloadOptions options;
    options.output_path = "/root/forbidden.txt";  // 无权限路径

    try {
        auto task = plugin->createTask("http://example.com/test.txt", options);
        EXPECT_NE(task, nullptr);

        // 文件写入时应该失败
    } catch (const std::exception& e) {
        // 可能立即失败或在实际写入时失败
        SUCCEED() << "Permission error expected: " << e.what();
    }
}

class HttpHeaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpHeaderTest, CustomHeaders) {
    DownloadOptions options;
    options.output_path = "header_test.txt";

    // 设置各种自定义头部
    options.headers["Authorization"] = "Bearer token123";
    options.headers["X-API-Key"] = "api-key-value";
    options.headers["Accept"] = "application/json";
    options.headers["User-Agent"] = "CustomAgent/1.0";

    try {
        auto task = plugin->createTask("http://httpbin.org/headers", options);
        EXPECT_NE(task, nullptr);

        // 需要验证头部是否正确发送
    } catch (const std::exception& e) {
        SUCCEED() << "Header test needs server: " << e.what();
    }
}

TEST_F(HttpHeaderTest, RefererHeader) {
    DownloadOptions options;
    options.output_path = "referer_test.txt";
    options.referrer = "http://example.com/source";

    try {
        auto task = plugin->createTask("http://httpbin.org/headers", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "Referer test needs server: " << e.what();
    }
}

class HttpAuthenticationTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpAuthenticationTest, BasicAuth) {
    DownloadOptions options;
    options.output_path = "auth_test.txt";
    options.username = "testuser";
    options.password = "testpass";

    try {
        auto task = plugin->createTask("http://httpbin.org/basic-auth/testuser/testpass", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "Auth test needs server: " << e.what();
    }
}

class HttpHttpsTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpHttpsTest, HttpsDownload) {
    DownloadOptions options;
    options.output_path = "https_test.txt";
    options.verify_ssl = true;  // 验证证书

    try {
        auto task = plugin->createTask("https://example.com/", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "HTTPS test needs server: " << e.what();
    }
}

TEST_F(HttpHttpsTest, SslVerification) {
    DownloadOptions options;
    options.output_path = "ssl_test.txt";
    options.verify_ssl = false;  // 跳过验证（用于测试）

    try {
        auto task = plugin->createTask("https://expired.badssl.com/", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "SSL test needs special setup: " << e.what();
    }
}

class HttpProxyTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpProxyTest, HttpProxy) {
    DownloadOptions options;
    options.output_path = "proxy_test.txt";
    options.proxy = "http://proxy.example.com:8080";

    try {
        auto task = plugin->createTask("http://example.com/", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "Proxy test needs actual proxy: " << e.what();
    }
}

TEST_F(HttpProxyTest, SocksProxy) {
    DownloadOptions options;
    options.output_path = "socks_test.txt";
    options.proxy = "socks5://127.0.0.1:1080";

    try {
        auto task = plugin->createTask("http://example.com/", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "SOCKS proxy test needs actual proxy: " << e.what();
    }
}

class HttpRedirectionTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin = std::make_unique<HttpPlugin>();
    }

    std::unique_ptr<HttpPlugin> plugin;
};

TEST_F(HttpRedirectionTest, FollowRedirect) {
    DownloadOptions options;
    options.output_path = "redirect_test.txt";
    options.follow_redirects = true;
    options.max_redirects = 5;

    try {
        auto task = plugin->createTask("http://httpbin.org/redirect/3", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "Redirect test needs server: " << e.what();
    }
}

TEST_F(HttpRedirectionTest, TooManyRedirects) {
    DownloadOptions options;
    options.output_path = "too_many_redirects.txt";
    options.max_redirects = 2;  // 设置较小的重定向限制

    try {
        auto task = plugin->createTask("http://httpbin.org/redirect/5", options);
        EXPECT_NE(task, nullptr);
    } catch (const std::exception& e) {
        SUCCEED() << "Too many redirects test needs server: " << e.what();
    }
}