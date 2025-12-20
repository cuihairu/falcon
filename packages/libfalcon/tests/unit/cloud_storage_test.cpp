/**
 * @file cloud_storage_test.cpp
 * @brief 网盘插件单元测试
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <gtest/gtest.h>
#include <falcon/cloud_storage_plugin.hpp>
#include <algorithm>
#include <chrono>

using namespace falcon;

class CloudStorageTest : public ::testing::Test {
protected:
    void SetUp() override {
        manager_ = std::make_unique<CloudStorageManager>();
    }

    void TearDown() override {
        manager_.reset();
    }

    std::unique_ptr<CloudStorageManager> manager_;
};

// 测试网盘平台检测
TEST_F(CloudStorageTest, DetectCloudPlatform) {
    // 百度网盘
    EXPECT_EQ(CloudPlatform::BaiduNetdisk,
              CloudLinkDetector::detect_platform("https://pan.baidu.com/s/1abcdefg"));

    // 蓝奏云
    EXPECT_EQ(CloudPlatform::LanzouCloud,
              CloudLinkDetector::detect_platform("https://www.lanzoux.com/iabcdefg"));

    // 阿里云盘
    EXPECT_EQ(CloudPlatform::AlibabaCloud,
              CloudLinkDetector::detect_platform("https://www.aliyundrive.com/s/abcdefg"));

    // Google Drive
    EXPECT_EQ(CloudPlatform::GoogleDrive,
              CloudLinkDetector::detect_platform("https://drive.google.com/file/d/abcdefg/view"));

    // OneDrive
    EXPECT_EQ(CloudPlatform::OneDrive,
              CloudLinkDetector::detect_platform("https://1drv.ms/u/s!AbCdEfGhIj"));

    // 未知平台
    EXPECT_EQ(CloudPlatform::Unknown,
              CloudLinkDetector::detect_platform("https://example.com/download/file"));
}

// 测试文件ID提取
TEST_F(CloudStorageTest, ExtractFileId) {
    // 百度网盘
    EXPECT_EQ("1abcdefg",
              CloudLinkDetector::extract_file_id(
                  "https://pan.baidu.com/s/1abcdefg",
                  CloudPlatform::BaiduNetdisk));

    // 蓝奏云
    EXPECT_EQ("iabcdefg",
              CloudLinkDetector::extract_file_id(
                  "https://www.lanzoux.com/iabcdefg",
                  CloudPlatform::LanzouCloud));

    // Google Drive
    EXPECT_EQ("abcdefg",
              CloudLinkDetector::extract_file_id(
                  "https://drive.google.com/file/d/abcdefg/view",
                  CloudPlatform::GoogleDrive));
}

// 测试URL规范化
TEST_F(CloudStorageTest, NormalizeUrl) {
    std::string url1 = "pan.baidu.com/s/1abcdefg";
    std::string normalized1 = CloudLinkDetector::normalize_url(url1);
    EXPECT_EQ("https://pan.baidu.com/s/1abcdefg", normalized1);

    std::string url2 = "https://example.com/file.zip?ref=test&utm_source=google";
    std::string normalized2 = CloudLinkDetector::normalize_url(url2);
    EXPECT_EQ("https://example.com/file.zip", normalized2);
}

// 测试蓝奏云插件
TEST_F(CloudStorageTest, LanzouCloudPlugin) {
    LanzouCloudPlugin plugin;

    EXPECT_EQ(plugin.platform_name(), "LanzouCloud");
    EXPECT_EQ(plugin.platform_type(), CloudPlatform::LanzouCloud);

    // 测试链接识别
    EXPECT_TRUE(plugin.can_handle("https://www.lanzoux.com/iabcdefg"));
    EXPECT_TRUE(plugin.can_handle("https://wwi.lanzouy.com/abcdef123"));
    EXPECT_FALSE(plugin.can_handle("https://pan.baidu.com/s/1abcdefg"));
}

// 模拟网盘插件
class MockCloudPlugin : public ICloudStoragePlugin {
public:
    std::string platform_name() const override {
        return "MockCloud";
    }

    CloudPlatform platform_type() const override {
        return CloudPlatform::Unknown;
    }

    bool can_handle(const std::string& url) const override {
        return url.find("mockcloud.com") != std::string::npos;
    }

    CloudExtractionResult extract_share_link(
        const std::string& share_url,
        const std::string& password = "") override {
        CloudExtractionResult result;
        result.platform_name = platform_name();
        result.platform_type = platform_type();

        if (share_url.find("error") != std::string::npos) {
            result.error_message = "Mock error";
            return result;
        }

        if (share_url.find("password") != std::string::npos && password != "123") {
            result.error_message = "需要密码";
            return result;
        }

        // 创建模拟文件
        CloudFileInfo file;
        file.id = "mock_file_001";
        file.name = "mock_file.zip";
        file.size = 1024 * 1024 * 100; // 100MB
        file.type = "file";
        file.md5 = "d41d8cd98f00b204e9800998ecf8427e";
        file.download_url = "https://mockcloud.com/download/mock_file_001";

        result.success = true;
        result.files.push_back(file);

        return result;
    }

    std::string get_download_url(const std::string& file_id,
                                const CloudDownloadOptions& options = {}) override {
        return "https://mockcloud.com/download/" + file_id;
    }

    bool authenticate(const std::string& token) override {
        return !token.empty();
    }

    std::map<std::string, std::string> get_user_info() override {
        return {
            {"user_id", "mock_user"},
            {"username", "testuser"},
            {"email", "test@example.com"}
        };
    }

    std::map<std::string, size_t> get_quota_info() override {
        return {
            {"used", 1024 * 1024 * 1024},
            {"total", 10ULL * 1024 * 1024 * 1024}
        };
    }
};

// 测试网盘插件注册
TEST_F(CloudStorageTest, RegisterCloudPlugin) {
    auto initial_platforms = manager_->get_supported_platforms();
    size_t initial_count = initial_platforms.size();

    // 注册新插件
    manager_->register_plugin(std::make_unique<MockCloudPlugin>());

    auto platforms = manager_->get_supported_platforms();
    EXPECT_EQ(platforms.size(), initial_count + 1);

    auto it = std::find(platforms.begin(), platforms.end(), "MockCloud");
    EXPECT_NE(it, platforms.end());
}

// 测试网盘链接处理
TEST_F(CloudStorageTest, HandleShareLink) {
    manager_->register_plugin(std::make_unique<MockCloudPlugin>());

    // 成功提取
    auto result1 = manager_->handle_share_link("https://mockcloud.com/share/file1");
    EXPECT_TRUE(result1.success);
    EXPECT_EQ(result1.platform_name, "MockCloud");
    EXPECT_EQ(result1.files.size(), 1);
    EXPECT_EQ(result1.files[0].name, "mock_file.zip");

    // 需要密码
    auto result2 = manager_->handle_share_link("https://mockcloud.com/share/password", "wrong");
    EXPECT_FALSE(result2.success);
    EXPECT_TRUE(result2.error_message.find("密码") != std::string::npos);

    // 正确密码
    auto result3 = manager_->handle_share_link("https://mockcloud.com/share/password", "123");
    EXPECT_TRUE(result3.success);

    // 错误链接
    auto result4 = manager_->handle_share_link("https://unknown.com/file");
    EXPECT_FALSE(result4.success);
}

// 测试获取下载链接
TEST_F(CloudStorageTest, GetDirectDownloadUrl) {
    manager_->register_plugin(std::make_unique<MockCloudPlugin>());

    std::string download_url = manager_->get_direct_download_url(
        "https://mockcloud.com/share/file1");
    EXPECT_EQ(download_url, "https://mockcloud.com/download/mock_file_001");
}

// 测试批量提取
TEST_F(CloudStorageTest, BatchExtract) {
    manager_->register_plugin(std::make_unique<MockCloudPlugin>());

    std::vector<std::string> urls = {
        "https://mockcloud.com/share/file1",
        "https://mockcloud.com/share/file2",
        "https://mockcloud.com/share/password"
    };

    std::map<std::string, std::string> passwords = {
        {"https://mockcloud.com/share/password", "123"}
    };

    auto results = manager_->batch_extract(urls, passwords);
    EXPECT_EQ(results.size(), 3);

    EXPECT_TRUE(results[0].success);
    EXPECT_TRUE(results[1].success);
    EXPECT_TRUE(results[2].success); // 使用了正确的密码
}

// 测试文件信息结构
TEST_F(CloudStorageTest, CloudFileInfoStructure) {
    CloudFileInfo file;
    file.id = "file_001";
    file.name = "test_document.pdf";
    file.size = 2 * 1024 * 1024; // 2MB
    file.type = "file";
    file.md5 = "5d41402abc4b2a76b9719d911017c592";
    file.modified_time = "2023-12-21 10:30:00";
    file.download_url = "https://cloud.example.com/download/file_001";
    file.share_url = "https://cloud.example.com/s/abc123";
    file.password = "abc123";

    EXPECT_EQ(file.id, "file_001");
    EXPECT_EQ(file.name, "test_document.pdf");
    EXPECT_EQ(file.size, 2 * 1024 * 1024);
    EXPECT_EQ(file.type, "file");
    EXPECT_EQ(file.md5, "5d41402abc4b2a76b9719d911017c592");
    EXPECT_EQ(file.modified_time, "2023-12-21 10:30:00");
    EXPECT_EQ(file.download_url, "https://cloud.example.com/download/file_001");
    EXPECT_EQ(file.share_url, "https://cloud.example.com/s/abc123");
    EXPECT_EQ(file.password, "abc123");
}

// 测试提取结果结构
TEST_F(CloudStorageTest, CloudExtractionResultStructure) {
    CloudExtractionResult result;
    result.success = true;
    result.platform_name = "TestCloud";
    result.platform_type = CloudPlatform::Unknown;

    CloudFileInfo file;
    file.id = "test_file";
    file.name = "test.txt";
    file.size = 1024;

    result.files.push_back(file);

    EXPECT_TRUE(result.success);
    EXPECT_TRUE(result.error_message.empty());
    EXPECT_EQ(result.platform_name, "TestCloud");
    EXPECT_EQ(result.files.size(), 1);
    EXPECT_EQ(result.files[0].name, "test.txt");
}

// 测试下载选项结构
TEST_F(CloudStorageTest, CloudDownloadOptionsStructure) {
    CloudDownloadOptions options;
    options.auth_token = "token123";
    options.refresh_token = "refresh456";
    options.api_key = "api_key_789";
    options.api_secret = "api_secret_abc";
    options.use_vip = true;
    options.download_thread = "4";
    options.timeout_seconds = 60;
    options.retry_count = 5;

    EXPECT_EQ(options.auth_token, "token123");
    EXPECT_EQ(options.refresh_token, "refresh456");
    EXPECT_EQ(options.api_key, "api_key_789");
    EXPECT_EQ(options.api_secret, "api_secret_abc");
    EXPECT_TRUE(options.use_vip);
    EXPECT_EQ(options.download_thread, "4");
    EXPECT_EQ(options.timeout_seconds, 60);
    EXPECT_EQ(options.retry_count, 5);
}

// 性能测试 - 大量链接处理
TEST_F(CloudStorageTest, PerformanceLargeBatch) {
    manager_->register_plugin(std::make_unique<MockCloudPlugin>());

    // 生成1000个链接
    std::vector<std::string> urls;
    for (int i = 0; i < 1000; ++i) {
        urls.push_back("https://mockcloud.com/share/file" + std::to_string(i));
    }

    auto start = std::chrono::high_resolution_clock::now();
    auto results = manager_->batch_extract(urls);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    EXPECT_EQ(results.size(), 1000);
    EXPECT_LT(duration.count(), 5000); // 应该在5秒内完成

    // 验证所有结果都成功
    for (const auto& result : results) {
        EXPECT_TRUE(result.success);
    }
}

// 测试各种网盘链接格式
TEST_F(CloudStorageTest, VariousCloudUrlFormats) {
    // 测试各种URL格式识别
    std::map<std::string, CloudPlatform> test_cases = {
        {"https://pan.baidu.com/s/1abc", CloudPlatform::BaiduNetdisk},
        {"https://yun.baidu.com/s/1abc", CloudPlatform::BaiduNetdisk},
        {"baidupan://1abc", CloudPlatform::BaiduNetdisk},
        {"https://lanzouy.com/iabc123", CloudPlatform::LanzouCloud},
        {"https://www.aliyundrive.com/s/abc123", CloudPlatform::AlibabaCloud},
        {"https://www.alipan.com/s/abc123", CloudPlatform::AlibabaCloud},
        {"alipan://abc123", CloudPlatform::AlibabaCloud},
        {"https://share.weiyun.com/abc123", CloudPlatform::TencentWeiyun},
        {"https://drive.google.com/file/d/abc123", CloudPlatform::GoogleDrive},
        {"https://drive.google.com/open?id=abc123", CloudPlatform::GoogleDrive},
        {"https://1drv.ms/u/s!AbCdEf", CloudPlatform::OneDrive},
        {"https://onedrive.live.com/something", CloudPlatform::OneDrive},
        {"https://www.dropbox.com/s/abc123/file", CloudPlatform::Dropbox},
        {"https://dl.dropboxusercontent.com/s/abc123/file", CloudPlatform::Dropbox}
    };

    for (const auto& [url, expected_platform] : test_cases) {
        CloudPlatform detected = CloudLinkDetector::detect_platform(url);
        EXPECT_EQ(detected, expected_platform)
            << "Failed for URL: " << url;
    }
}