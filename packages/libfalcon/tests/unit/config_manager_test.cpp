// Falcon Config Manager Unit Tests

#include <falcon/config_manager.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace {

std::filesystem::path unique_temp_dir(const std::string& prefix) {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / (prefix + std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    return dir;
}

falcon::CloudStorageConfig make_config(const std::string& name,
                                       const std::string& provider,
                                       const std::string& access_key,
                                       const std::string& secret_key) {
    falcon::CloudStorageConfig cfg{};
    cfg.name = name;
    cfg.provider = provider;
    cfg.access_key = access_key;
    cfg.secret_key = secret_key;
    cfg.region = "us-east-1";
    cfg.bucket = "test-bucket";
    cfg.endpoint = "https://example.com";
    cfg.custom_domain = "cdn.example.com";
    cfg.extra = {{"k1", "v1"}, {"k2", "v2"}};
    cfg.created_at = 0;
    cfg.updated_at = 0;
    return cfg;
}

} // namespace

#if !defined(FALCON_ENABLE_CONFIG_MANAGER)

TEST(ConfigManagerTest, Disabled) {
    GTEST_SKIP() << "FALCON_ENABLE_CONFIG_MANAGER is not enabled in this build";
}

#else

TEST(ConfigManagerTest, CrudListSearchExportImport) {
    auto dir1 = unique_temp_dir("falcon_cfg_");
    auto dir2 = unique_temp_dir("falcon_cfg_import_");

    const auto db1 = (dir1 / "config.db").string();
    const auto db2 = (dir2 / "config.db").string();

    falcon::ConfigManager cm1;
    ASSERT_TRUE(cm1.initialize(db1, "Master123!"));

    auto cfg = make_config("test1", "s3", "AKIA_TEST", "SECRET_TEST");
    ASSERT_TRUE(cm1.save_cloud_config(cfg));

    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm1.get_cloud_config("test1", loaded));
    EXPECT_EQ(loaded.name, "test1");
    EXPECT_EQ(loaded.provider, "s3");
    EXPECT_EQ(loaded.access_key, "AKIA_TEST");
    EXPECT_EQ(loaded.secret_key, "SECRET_TEST");
    EXPECT_EQ(loaded.region, "us-east-1");

    auto names = cm1.list_cloud_configs();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "test1");

    auto s3_configs = cm1.search_configs("s3");
    ASSERT_EQ(s3_configs.size(), 1u);
    EXPECT_EQ(s3_configs[0].name, "test1");
    EXPECT_EQ(s3_configs[0].access_key, "AKIA_TEST");

    auto updated = make_config("test1", "s3", "AKIA_NEW", "SECRET_NEW");
    updated.region = "ap-southeast-1";
    ASSERT_TRUE(cm1.update_cloud_config("test1", updated));

    falcon::CloudStorageConfig after_update{};
    ASSERT_TRUE(cm1.get_cloud_config("test1", after_update));
    EXPECT_EQ(after_update.access_key, "AKIA_NEW");
    EXPECT_EQ(after_update.secret_key, "SECRET_NEW");
    EXPECT_EQ(after_update.region, "ap-southeast-1");

    const auto export_path = (dir1 / "export.bin").string();
    ASSERT_TRUE(cm1.export_configs(export_path, "ExportPass!"));

    falcon::ConfigManager cm2;
    ASSERT_TRUE(cm2.initialize(db2, "Master123!"));
    ASSERT_TRUE(cm2.import_configs(export_path, "ExportPass!"));

    falcon::CloudStorageConfig imported{};
    ASSERT_TRUE(cm2.get_cloud_config("test1", imported));
    EXPECT_EQ(imported.access_key, "AKIA_NEW");
    EXPECT_EQ(imported.secret_key, "SECRET_NEW");

    ASSERT_TRUE(cm2.delete_cloud_config("test1"));
    falcon::CloudStorageConfig missing{};
    EXPECT_FALSE(cm2.get_cloud_config("test1", missing));
}

#endif

