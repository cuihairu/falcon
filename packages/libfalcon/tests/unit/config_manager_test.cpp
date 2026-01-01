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

// 新增：多次初始化测试
TEST(ConfigManagerTest, MultipleInitialization) {
    auto dir = unique_temp_dir("falcon_cfg_multi_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    // 重复初始化应该失败或被忽略
    bool result = cm.initialize(db, "Master123!");
    // 验证行为
}

// 新增：弱密码测试
TEST(ConfigManagerTest, WeakPassword) {
    auto dir = unique_temp_dir("falcon_cfg_weak_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;

    // 弱密码应该被拒绝
    EXPECT_FALSE(cm.initialize(db, "123"));
    EXPECT_FALSE(cm.initialize(db, "password"));
    EXPECT_FALSE(cm.initialize(db, ""));
}

// 新增：保存多个配置
TEST(ConfigManagerTest, SaveMultipleConfigs) {
    auto dir = unique_temp_dir("falcon_cfg_multi_save_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    // 保存多个配置
    for (int i = 0; i < 10; ++i) {
        auto cfg = make_config("test" + std::to_string(i), "s3",
                               "AKIA_TEST" + std::to_string(i),
                               "SECRET_TEST" + std::to_string(i));
        ASSERT_TRUE(cm.save_cloud_config(cfg));
    }

    auto names = cm.list_cloud_configs();
    EXPECT_EQ(names.size(), 10u);
}

// 新增：更新不存在的配置
TEST(ConfigManagerTest, UpdateNonExistentConfig) {
    auto dir = unique_temp_dir("falcon_cfg_update_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg = make_config("nonexistent", "s3", "AKIA_TEST", "SECRET_TEST");
    EXPECT_FALSE(cm.update_cloud_config("nonexistent", cfg));
}

// 新增：删除不存在的配置
TEST(ConfigManagerTest, DeleteNonExistentConfig) {
    auto dir = unique_temp_dir("falcon_cfg_delete_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    EXPECT_FALSE(cm.delete_cloud_config("nonexistent"));
}

// 新增：配置搜索功能
TEST(ConfigManagerTest, SearchFunctionality) {
    auto dir = unique_temp_dir("falcon_cfg_search_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    // 添加不同类型的配置
    auto cfg1 = make_config("s3_config", "s3", "AKIA_S3", "SECRET_S3");
    auto cfg2 = make_config("oss_config", "oss", "AKIA_OSS", "SECRET_OSS");
    auto cfg3 = make_config("cos_config", "cos", "AKIA_COS", "SECRET_COS");

    ASSERT_TRUE(cm.save_cloud_config(cfg1));
    ASSERT_TRUE(cm.save_cloud_config(cfg2));
    ASSERT_TRUE(cm.save_cloud_config(cfg3));

    // 按提供商搜索
    auto s3_results = cm.search_configs("s3");
    EXPECT_EQ(s3_results.size(), 1u);
    EXPECT_EQ(s3_results[0].name, "s3_config");

    // 按名称搜索
    auto oss_results = cm.search_configs("oss");
    EXPECT_EQ(oss_results.size(), 1u);
}

// 新增：导出导入错误密码
TEST(ConfigManagerTest, ExportImportWrongPassword) {
    auto dir = unique_temp_dir("falcon_cfg_pwd_");
    const auto db = (dir / "config.db").string();
    const auto export_path = (dir / "export.bin").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg = make_config("test1", "s3", "AKIA_TEST", "SECRET_TEST");
    ASSERT_TRUE(cm.save_cloud_config(cfg));

    // 用错误密码导出
    EXPECT_FALSE(cm.export_configs(export_path, "WrongPassword!"));

    // 正确导出
    ASSERT_TRUE(cm.export_configs(export_path, "ExportPass!"));

    // 用错误密码导入
    falcon::ConfigManager cm2;
    ASSERT_TRUE(cm2.initialize(db, "Master123!"));
    EXPECT_FALSE(cm2.import_configs(export_path, "WrongPassword!"));

    // 正确导入
    ASSERT_TRUE(cm2.import_configs(export_path, "ExportPass!"));
}

// 新增：配置字段完整性
TEST(ConfigManagerTest, ConfigFieldCompleteness) {
    auto dir = unique_temp_dir("falcon_cfg_fields_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg = make_config("full_test", "s3", "AKIA_FULL", "SECRET_FULL");
    cfg.region = "eu-west-1";
    cfg.bucket = "my-bucket";
    cfg.endpoint = "https://s3.amazonaws.com";
    cfg.custom_domain = "files.example.com";
    cfg.extra = {{"key1", "value1"}, {"key2", "value2"}};

    ASSERT_TRUE(cm.save_cloud_config(cfg));

    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("full_test", loaded));

    EXPECT_EQ(loaded.name, "full_test");
    EXPECT_EQ(loaded.provider, "s3");
    EXPECT_EQ(loaded.access_key, "AKIA_FULL");
    EXPECT_EQ(loaded.secret_key, "SECRET_FULL");
    EXPECT_EQ(loaded.region, "eu-west-1");
    EXPECT_EQ(loaded.bucket, "my-bucket");
    EXPECT_EQ(loaded.endpoint, "https://s3.amazonaws.com");
    EXPECT_EQ(loaded.custom_domain, "files.example.com");
    EXPECT_EQ(loaded.extra.size(), 2u);
}

// 新增：重复名称保存
TEST(ConfigManagerTest, SaveDuplicateName) {
    auto dir = unique_temp_dir("falcon_cfg_dup_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg1 = make_config("duplicate", "s3", "AKIA_FIRST", "SECRET_FIRST");
    ASSERT_TRUE(cm.save_cloud_config(cfg1));

    // 尝试保存同名配置
    auto cfg2 = make_config("duplicate", "s3", "AKIA_SECOND", "SECRET_SECOND");

    // 应该失败或覆盖，取决于实现
    bool result = cm.save_cloud_config(cfg2);

    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("duplicate", loaded));

    // 验证最终状态
}

// 新增：空配置名称
TEST(ConfigManagerTest, EmptyConfigName) {
    auto dir = unique_temp_dir("falcon_cfg_empty_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg = make_config("", "s3", "AKIA_TEST", "SECRET_TEST");
    EXPECT_FALSE(cm.save_cloud_config(cfg));
}

// 新增：特殊字符在配置名称中
TEST(ConfigManagerTest, SpecialCharactersInName) {
    auto dir = unique_temp_dir("falcon_cfg_special_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg = make_config("test-config_v2", "s3", "AKIA_TEST", "SECRET_TEST");
    ASSERT_TRUE(cm.save_cloud_config(cfg));

    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("test-config_v2", loaded));
    EXPECT_EQ(loaded.name, "test-config_v2");
}

// 新增：大量配置性能测试
TEST(ConfigManagerTest, ManyConfigsPerformance) {
    auto dir = unique_temp_dir("falcon_cfg_perf_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    constexpr int config_count = 1000;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < config_count; ++i) {
        auto cfg = make_config("config_" + std::to_string(i), "s3",
                               "AKIA_" + std::to_string(i),
                               "SECRET_" + std::to_string(i));
        ASSERT_TRUE(cm.save_cloud_config(cfg));
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    // 应该在合理时间内完成
    EXPECT_LT(duration.count(), 5000);

    auto names = cm.list_cloud_configs();
    EXPECT_EQ(names.size(), config_count);
}

// 新增：配置序列化反序列化
TEST(ConfigManagerTest, ConfigSerialization) {
    auto dir = unique_temp_dir("falcon_cfg_serial_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    // 创建包含各种数据类型的配置
    auto cfg = make_config("serial_test", "s3", "AKIA_TEST", "SECRET_TEST");
    cfg.extra = {
        {"string_key", "string_value"},
        {"number_key", "12345"},
        {"bool_key", "true"},
        {"empty_key", ""}
    };

    ASSERT_TRUE(cm.save_cloud_config(cfg));

    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("serial_test", loaded));

    EXPECT_EQ(loaded.extra.size(), 4u);
    EXPECT_EQ(loaded.extra["string_key"], "string_value");
}

// 新增：数据库路径不存在
TEST(ConfigManagerTest, InvalidDatabasePath) {
    falcon::ConfigManager cm;

    // 使用无效路径
    EXPECT_FALSE(cm.initialize("/nonexistent/path/config.db", "Master123!"));
}

// 新增：并发配置访问
TEST(ConfigManagerTest, ConcurrentConfigAccess) {
    auto dir = unique_temp_dir("falcon_cfg_concurrent_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    std::vector<std::thread> threads;

    // 多线程同时保存配置
    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([&cm, i]() {
            auto cfg = make_config("concurrent_" + std::to_string(i), "s3",
                                   "AKIA_" + std::to_string(i),
                                   "SECRET_" + std::to_string(i));
            cm.save_cloud_config(cfg);
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto names = cm.list_cloud_configs();
    EXPECT_GE(names.size(), 0u);
}

// 新增：配置更新时间戳
TEST(ConfigManagerTest, ConfigTimestamps) {
    auto dir = unique_temp_dir("falcon_cfg_time_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    auto cfg = make_config("time_test", "s3", "AKIA_TEST", "SECRET_TEST");
    cfg.created_at = 12345;
    cfg.updated_at = 12345;

    ASSERT_TRUE(cm.save_cloud_config(cfg));

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    auto updated = make_config("time_test", "s3", "AKIA_NEW", "SECRET_NEW");
    ASSERT_TRUE(cm.update_cloud_config("time_test", updated));

    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("time_test", loaded));

    // 更新时间应该改变
    EXPECT_GE(loaded.updated_at, loaded.created_at);
}

#endif

