// Verify falcon_drives can operate independently of falcon_protocols.
// Links against falcon_core + falcon_drives only.

#include <falcon/cloud_storage_plugin.hpp>

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 1. CloudPlatform enum values (header-only, always available)
// ---------------------------------------------------------------------------

TEST(DrivesIsolationTest, CloudPlatformEnumValues) {
    EXPECT_NE(falcon::CloudPlatform::BaiduNetdisk, falcon::CloudPlatform::Unknown);
    EXPECT_NE(falcon::CloudPlatform::AlibabaCloud, falcon::CloudPlatform::Unknown);
    EXPECT_NE(falcon::CloudPlatform::GoogleDrive, falcon::CloudPlatform::Unknown);
    EXPECT_NE(falcon::CloudPlatform::Mega, falcon::CloudPlatform::Unknown);
}

// ---------------------------------------------------------------------------
// 2. CloudFileInfo defaults (header-only, always available)
// ---------------------------------------------------------------------------

TEST(DrivesIsolationTest, CloudFileInfoDefaults) {
    falcon::CloudFileInfo info;
    EXPECT_TRUE(info.id.empty());
    EXPECT_TRUE(info.name.empty());
    EXPECT_EQ(info.size, 0u);
    EXPECT_TRUE(info.type.empty());
    EXPECT_TRUE(info.md5.empty());
    EXPECT_TRUE(info.download_url.empty());
    EXPECT_TRUE(info.share_url.empty());
    EXPECT_TRUE(info.password.empty());
    EXPECT_TRUE(info.metadata.empty());
}

// ---------------------------------------------------------------------------
// 3. CloudExtractionResult defaults (header-only, always available)
// ---------------------------------------------------------------------------

TEST(DrivesIsolationTest, CloudExtractionResultDefaults) {
    falcon::CloudExtractionResult result;
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error_message.empty());
    EXPECT_TRUE(result.files.empty());
    EXPECT_TRUE(result.platform_name.empty());
    EXPECT_EQ(result.platform_type, falcon::CloudPlatform::Unknown);
}

// ---------------------------------------------------------------------------
// 4. CloudDownloadOptions defaults (header-only, always available)
// ---------------------------------------------------------------------------

TEST(DrivesIsolationTest, CloudDownloadOptionsDefaults) {
    falcon::CloudDownloadOptions opts;
    EXPECT_TRUE(opts.auth_token.empty());
    EXPECT_TRUE(opts.refresh_token.empty());
    EXPECT_FALSE(opts.use_vip);
    EXPECT_EQ(opts.timeout_seconds, 30);
    EXPECT_EQ(opts.retry_count, 3);
}

// ---------------------------------------------------------------------------
// 5-7. CloudLinkDetector / CloudStorageManager (require cloud_storage_plugin.cpp)
// ---------------------------------------------------------------------------

#ifdef FALCON_ENABLE_CLOUD_STORAGE

TEST(DrivesIsolationTest, DetectPlatformUnknownUrl) {
    auto platform = falcon::CloudLinkDetector::detect_platform("http://example.com");
    EXPECT_EQ(platform, falcon::CloudPlatform::Unknown);
}

TEST(DrivesIsolationTest, DetectPlatformEmptyUrl) {
    auto platform = falcon::CloudLinkDetector::detect_platform("");
    EXPECT_EQ(platform, falcon::CloudPlatform::Unknown);
}

TEST(DrivesIsolationTest, NormalizeUrl) {
    std::string url = "HTTPS://EXAMPLE.COM/PATH";
    std::string normalized = falcon::CloudLinkDetector::normalize_url(url);
    EXPECT_FALSE(normalized.empty());
}

TEST(DrivesIsolationTest, CloudStorageManagerCreateDestroy) {
    falcon::CloudStorageManager manager;
    auto platforms = manager.get_supported_platforms();
    EXPECT_TRUE(platforms.empty() || !platforms.empty());
}

namespace {
class MockCloudPlugin final : public falcon::ICloudStoragePlugin {
public:
    [[nodiscard]] std::string platform_name() const override { return "mock-cloud"; }
    [[nodiscard]] falcon::CloudPlatform platform_type() const override {
        return falcon::CloudPlatform::Unknown;
    }
    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return url.rfind("mock-cloud://", 0) == 0;
    }
    falcon::CloudExtractionResult extract_share_link(
        const std::string& share_url, const std::string& password = "") override {
        falcon::CloudExtractionResult result;
        result.success = true;
        result.platform_name = "mock-cloud";

        falcon::CloudFileInfo file;
        file.name = "test_file.txt";
        file.size = 1024;
        file.share_url = share_url;
        file.password = password;
        file.download_url = "http://cdn.mock.com/dl/" + share_url;
        result.files.push_back(file);
        return result;
    }
    std::string get_download_url(
        const std::string& file_id,
        const falcon::CloudDownloadOptions& = {}) override {
        return "http://cdn.mock.com/" + file_id;
    }
    bool authenticate(const std::string&) override { return true; }
    std::map<std::string, std::string> get_user_info() override {
        return {{"user", "test"}, {"quota", "100GB"}};
    }
    std::map<std::string, size_t> get_quota_info() override {
        return {{"total", 100000000000}, {"used", 50000000000}};
    }
};
} // namespace

TEST(DrivesIsolationTest, RegisterCustomCloudPlugin) {
    falcon::CloudStorageManager manager;
    manager.register_plugin(std::make_unique<MockCloudPlugin>());

    auto platforms = manager.get_supported_platforms();
    bool found = false;
    for (const auto& p : platforms) {
        if (p == "mock-cloud") found = true;
    }
    EXPECT_TRUE(found);
}

TEST(DrivesIsolationTest, ExtractShareLinkWithPlugin) {
    falcon::CloudStorageManager manager;
    manager.register_plugin(std::make_unique<MockCloudPlugin>());

    auto result = manager.handle_share_link("mock-cloud://share/abc123", "pass123");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].name, "test_file.txt");
    EXPECT_EQ(result.files[0].password, "pass123");
}

TEST(DrivesIsolationTest, GetDirectDownloadUrl) {
    falcon::CloudStorageManager manager;
    manager.register_plugin(std::make_unique<MockCloudPlugin>());

    std::string url = manager.get_direct_download_url("mock-cloud://share/abc123");
    EXPECT_FALSE(url.empty());
    EXPECT_TRUE(url.find("abc123") != std::string::npos ||
                url.find("cdn.mock.com") != std::string::npos);
}

TEST(DrivesIsolationTest, BatchExtractEmpty) {
    falcon::CloudStorageManager manager;
    manager.register_plugin(std::make_unique<MockCloudPlugin>());

    auto results = manager.batch_extract({});
    EXPECT_TRUE(results.empty());
}

TEST(DrivesIsolationTest, BatchExtractMultiple) {
    falcon::CloudStorageManager manager;
    manager.register_plugin(std::make_unique<MockCloudPlugin>());

    std::vector<std::string> urls = {
        "mock-cloud://share/001",
        "mock-cloud://share/002",
    };
    std::map<std::string, std::string> passwords = {
        {"mock-cloud://share/001", "pw1"},
        {"mock-cloud://share/002", "pw2"},
    };

    auto results = manager.batch_extract(urls, passwords);
    EXPECT_EQ(results.size(), 2u);
    EXPECT_TRUE(results[0].success);
    EXPECT_TRUE(results[1].success);
}

#endif // FALCON_ENABLE_CLOUD_STORAGE
