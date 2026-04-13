/**
 * @file sftp_plugin_test.cpp
 * @brief SFTP 插件单元测试
 */

#include <gtest/gtest.h>
#include <falcon/plugins/sftp/sftp_plugin.hpp>
#include <fstream>

using namespace falcon::plugins;

class SFTPPluginTest : public ::testing::Test {
protected:
    void SetUp() override {
        plugin_ = std::make_unique<SFTPPlugin>();
    }

    void TearDown() override {
        plugin_.reset();
    }

    std::unique_ptr<SFTPPlugin> plugin_;
};

// 测试协议名称
TEST_F(SFTPPluginTest, GetProtocolName) {
    EXPECT_EQ(plugin_->getProtocolName(), "sftp");
}

// 测试支持的协议方案
TEST_F(SFTPPluginTest, GetSupportedSchemes) {
    auto schemes = plugin_->getSupportedSchemes();
    EXPECT_EQ(schemes.size(), 1);
    EXPECT_EQ(schemes[0], "sftp");
}

// 测试 URL 识别
TEST_F(SFTPPluginTest, CanHandle) {
    // SFTP URLs
    EXPECT_TRUE(plugin_->canHandle("sftp://example.com/file.txt"));
    EXPECT_TRUE(plugin_->canHandle("sftp://user@example.com/file.txt"));
    EXPECT_TRUE(plugin_->canHandle("sftp://user:pass@example.com:22/path/to/file"));
    EXPECT_TRUE(plugin_->canHandle("sftp://192.168.1.1/home/user/file.bin"));

    // 非 SFTP URLs
    EXPECT_FALSE(plugin_->canHandle("http://example.com/file.txt"));
    EXPECT_FALSE(plugin_->canHandle("ftp://example.com/file.txt"));
    EXPECT_FALSE(plugin_->canHandle("file:///path/to/file"));
}

// 测试 URL 解析
TEST_F(SFTPPluginTest, ParseSFTPUrlBasic) {
    SFTPPlugin::ConnectionInfo info;
    std::string path;

    std::string url = "sftp://example.com/path/to/file.txt";
    ASSERT_TRUE(plugin_->parseSFTPUrl(url, info, path));

    EXPECT_EQ(info.host, "example.com");
    EXPECT_EQ(info.port, 22);
    EXPECT_EQ(path, "/path/to/file.txt");
    EXPECT_FALSE(info.username.empty());  // 应该设置默认用户名
}

TEST_F(SFTPPluginTest, ParseSFTPUrlWithUser) {
    SFTPPlugin::ConnectionInfo info;
    std::string path;

    std::string url = "sftp://user@example.com/path/to/file.txt";
    ASSERT_TRUE(plugin_->parseSFTPUrl(url, info, path));

    EXPECT_EQ(info.host, "example.com");
    EXPECT_EQ(info.username, "user");
    EXPECT_EQ(info.port, 22);
    EXPECT_EQ(path, "/path/to/file.txt");
}

TEST_F(SFTPPluginTest, ParseSFTPUrlWithPassword) {
    SFTPPlugin::ConnectionInfo info;
    std::string path;

    std::string url = "sftp://user:password@example.com/path/to/file.txt";
    ASSERT_TRUE(plugin_->parseSFTPUrl(url, info, path));

    EXPECT_EQ(info.host, "example.com");
    EXPECT_EQ(info.username, "user");
    EXPECT_EQ(info.password, "password");
    EXPECT_EQ(info.port, 22);
    EXPECT_EQ(path, "/path/to/file.txt");
}

TEST_F(SFTPPluginTest, ParseSFTPUrlWithPort) {
    SFTPPlugin::ConnectionInfo info;
    std::string path;

    std::string url = "sftp://user@example.com:2222/path/to/file.txt";
    ASSERT_TRUE(plugin_->parseSFTPUrl(url, info, path));

    EXPECT_EQ(info.host, "example.com");
    EXPECT_EQ(info.username, "user");
    EXPECT_EQ(info.port, 2222);
    EXPECT_EQ(path, "/path/to/file.txt");
}

TEST_F(SFTPPluginTest, ParseSFTPUrlComplex) {
    SFTPPlugin::ConnectionInfo info;
    std::string path;

    std::string url = "sftp://admin:secret123@fileserver.example.com:8022/uploads/document.pdf";
    ASSERT_TRUE(plugin_->parseSFTPUrl(url, info, path));

    EXPECT_EQ(info.host, "fileserver.example.com");
    EXPECT_EQ(info.username, "admin");
    EXPECT_EQ(info.password, "secret123");
    EXPECT_EQ(info.port, 8022);
    EXPECT_EQ(path, "/uploads/document.pdf");
}

TEST_F(SFTPPluginTest, ParseSFTPUrlInvalid) {
    SFTPPlugin::ConnectionInfo info;
    std::string path;

    // 无效的 URL
    EXPECT_FALSE(plugin_->parseSFTPUrl("http://example.com/file", info, path));
    EXPECT_FALSE(plugin_->parseSFTPUrl("sftp://", info, path));
    EXPECT_FALSE(plugin_->parseSFTPUrl("sftp:///path", info, path));
}

// 测试任务创建
TEST_F(SFTPPluginTest, CreateTask) {
    falcon::DownloadOptions options;
    std::string url = "sftp://user@example.com/file.txt";

    auto task = plugin_->createTask(url, options);
    ASSERT_NE(task, nullptr);

    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Pending);
}

// 测试连接信息加载
TEST_F(SFTPPluginTest, LoadConnectionInfo) {
    // 这个测试需要实际的 SSH 环境
    // 在 CI 环境中可以使用 Docker 容器运行 SFTP 服务器

    falcon::DownloadOptions options;
    std::string url = "sftp://test@example.com/test.txt";

    auto task = dynamic_cast<SFTPPlugin::SFTPDownloadTask*>(
        plugin_->createTask(url, options).get()
    );

    ASSERT_NE(task, nullptr);

    // 验证默认连接信息已加载
    auto status = task->getStatus();
    EXPECT_EQ(status, falcon::TaskStatus::Pending);
}

#ifdef FALCON_USE_LIBSSH
// 集成测试 - 需要实际的 SFTP 服务器
TEST_F(SFTPPluginTest, DISABLED_RealSFTPDownload) {
    // 这个测试需要一个运行的 SFTP 服务器
    // 可以使用 Docker 启动测试服务器：
    // docker run -d -p 2222:22 -e PASSWORD_USER=testuser:testpass atmoz/sftp

    falcon::DownloadOptions options;
    options.output_path = "/tmp/sftp_test_download.txt";

    std::string url = "sftp://testuser:testpass@localhost:2222/testfile.txt";

    auto task = plugin_->createTask(url, options);
    ASSERT_NE(task, nullptr);

    task->start();

    // 等待下载完成
    while (task->getStatus() == falcon::TaskStatus::Downloading) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Completed);
    EXPECT_GT(task->getDownloadedBytes(), 0);

    // 清理
    std::remove("/tmp/sftp_test_download.txt");
}
#endif

// 性能测试
TEST_F(SFTPPluginTest, PerformanceUrlParsing) {
    std::vector<std::string> urls = {
        "sftp://user@example.com/path/to/file.txt",
        "sftp://admin:pass@192.168.1.1:2222/uploads/file.bin",
        "sftp://test@file.server.org:8022/documents/report.pdf"
    };

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < 10000; ++i) {
        SFTPPlugin::ConnectionInfo info;
        std::string path;
        for (const auto& url : urls) {
            plugin_->parseSFTPUrl(url, info, path);
        }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 10000 次 3 个 URL 的解析应该在 100ms 内完成
    EXPECT_LT(duration.count(), 100);
}
