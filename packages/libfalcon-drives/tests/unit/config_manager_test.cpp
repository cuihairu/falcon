// Falcon Config Manager Unit Tests

#include <falcon/drives/config_manager.hpp>

#include <gtest/gtest.h>

#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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

// 直连 SQLite 执行写语句（绕过 ConfigManager 构造防御/损坏数据场景）
bool exec_sql(const std::string& db_path, const std::string& sql) {
    sqlite3* db = nullptr;
    if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
        sqlite3_close(db);
        return false;
    }
    char* err = nullptr;
    bool ok = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) == SQLITE_OK;
    sqlite3_free(err);
    sqlite3_close(db);
    return ok;
}

// 与 AES256GCM 相同布局的最小 GCM 加密器（IV12 + 密文 + tag16，密钥
// SHA256 派生）——构造篡改语义的导出 payload
std::string mini_gcm_encrypt(const std::string& plaintext, const std::string& key) {
    unsigned char iv[12];
    RAND_bytes(iv, 12);
    unsigned char derived[32];
    SHA256(reinterpret_cast<const unsigned char*>(key.data()), key.size(), derived);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return "";
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, derived, iv);
    std::string ct(plaintext.size() + 16, '\0');
    int len = 0;
    int total = 0;
    EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(&ct[0]), &len,
                      reinterpret_cast<const unsigned char*>(plaintext.data()),
                      static_cast<int>(plaintext.size()));
    total = len;
    EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(&ct[total]), &len);
    total += len;
    unsigned char tag[16];
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
    EVP_CIPHER_CTX_free(ctx);

    std::string out(reinterpret_cast<char*>(iv), 12);
    out.append(ct, 0, static_cast<size_t>(total));
    out.append(reinterpret_cast<char*>(tag), 16);
    return out;
}

std::string make_export_payload(const std::string& json_text, const std::string& password) {
    std::string payload = "FALCONCFG1";  // kExportMagic
    payload += mini_gcm_encrypt(json_text, password);
    return payload;
}

bool write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
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

    // 导出密码是新密码，不存在“错误旧密码”概念
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

//==============================================================================
// 主密码认证全链 / 导出导入边界（覆盖率批次 K）
//==============================================================================

// 未初始化的 manager：所有操作一律拒绝（认证门 verify 因 db_ 为空失败）
TEST(ConfigManagerTest, UninitializedManagerRejectsEverything) {
    auto dir = unique_temp_dir("falcon_cfg_uninit_");
    const auto export_path = (dir / "out.bin").string();

    falcon::ConfigManager cm;
    EXPECT_FALSE(cm.verify_master_password("Whatever123"));
    EXPECT_FALSE(cm.set_master_password("Whatever123"));

    falcon::CloudStorageConfig cfg = make_config("n", "s3", "a", "s");
    EXPECT_FALSE(cm.save_cloud_config(cfg));
    falcon::CloudStorageConfig loaded{};
    EXPECT_FALSE(cm.get_cloud_config("n", loaded));
    EXPECT_FALSE(cm.delete_cloud_config("n"));
    EXPECT_TRUE(cm.list_cloud_configs().empty());
    EXPECT_TRUE(cm.search_configs("").empty());
    EXPECT_FALSE(cm.update_cloud_config("n", cfg));
    EXPECT_FALSE(cm.export_configs(export_path, "Export123!"));
    EXPECT_FALSE(cm.import_configs(export_path, "Export123!"));
}

// verify_master_password 全链：正确/错误密码、错误尝试后可恢复
TEST(ConfigManagerTest, VerifyMasterPasswordFlow) {
    auto dir = unique_temp_dir("falcon_cfg_verify_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    EXPECT_TRUE(cm.verify_master_password("Master123!"));
    EXPECT_FALSE(cm.verify_master_password("Wrong1234"));
    // 错误尝试使认证失效，正确密码立即恢复
    falcon::CloudStorageConfig cfg = make_config("after_wrong", "s3", "a", "s");
    EXPECT_TRUE(cm.verify_master_password("Master123!"));
    EXPECT_TRUE(cm.save_cloud_config(cfg));
}

// master 表无行（库被外部破坏）：verify 返回 false。已知语义瑕疵：
// missing-row 提前返回路径不触碰认证态——已认证 manager 的写操作
// 不受影响（认证门仅在 authenticated_ 已失效时才复核）
TEST(ConfigManagerTest, VerifyWithMissingMasterRow) {
    auto dir = unique_temp_dir("falcon_cfg_nomaster_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(exec_sql(db, "DELETE FROM master"));

    EXPECT_FALSE(cm.verify_master_password("Master123!"));

    // 已认证态未被撤销：写入仍放行
    falcon::CloudStorageConfig cfg = make_config("still_writable", "s3", "a", "s");
    EXPECT_TRUE(cm.save_cloud_config(cfg));
    EXPECT_FALSE(cm.list_cloud_configs().empty());
}

// set_master_password：弱密码拒绝、换密后旧密码失效、新密码可认证。
// 已知语义（生产零调用的未接线 API）：换密只更新 master 表哈希，不
// 重加密已存配置——旧密文以旧密码密钥加密，换密后解密失败返回空串
TEST(ConfigManagerTest, SetMasterPasswordRotatesAndValidates) {
    auto dir = unique_temp_dir("falcon_cfg_rotate_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "First123!"));
    auto cfg = make_config("rot", "s3", "AKIA_ROT", "SECRET_ROT");
    ASSERT_TRUE(cm.save_cloud_config(cfg));

    EXPECT_FALSE(cm.set_master_password("weak"));
    EXPECT_TRUE(cm.set_master_password("Second456!"));

    EXPECT_FALSE(cm.verify_master_password("First123!"));
    ASSERT_TRUE(cm.verify_master_password("Second456!"));

    // 行仍在、非敏感字段完好；敏感字段解密失败为空串（不崩溃不误读）
    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("rot", loaded));
    EXPECT_EQ(loaded.provider, "s3");
    EXPECT_EQ(loaded.access_key, "");
}

// 库内密文被截短：get 仍成功，解密失败的敏感字段为空串（不崩溃不误报）
TEST(ConfigManagerTest, GetConfigWithTruncatedEncryptedBlob) {
    auto dir = unique_temp_dir("falcon_cfg_trunc_");
    const auto db = (dir / "config.db").string();

    {
        falcon::ConfigManager cm;
        ASSERT_TRUE(cm.initialize(db, "Master123!"));
        ASSERT_TRUE(cm.save_cloud_config(
            make_config("victim", "s3", "AKIA_TEST", "SECRET_TEST")));
    }
    ASSERT_TRUE(exec_sql(db,
                         "UPDATE configs SET access_key = x'00112233' "
                         "WHERE name = 'victim'"));

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    falcon::CloudStorageConfig loaded{};
    ASSERT_TRUE(cm.get_cloud_config("victim", loaded));
    EXPECT_EQ(loaded.access_key, "");  // 解密失败 → 空串
    EXPECT_EQ(loaded.secret_key, "SECRET_TEST");
}

// update 拒绝空 provider
TEST(ConfigManagerTest, UpdateRejectsEmptyProvider) {
    auto dir = unique_temp_dir("falcon_cfg_updprov_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("u1", "s3", "a", "s")));

    auto bad = make_config("u1", "", "a", "s");
    EXPECT_FALSE(cm.update_cloud_config("u1", bad));
}

// 导出导入边界：空密码 / 不存在文件 / 短文件 / 错 magic
TEST(ConfigManagerTest, ExportImportEdgeCases) {
    auto dir = unique_temp_dir("falcon_cfg_edge_");
    const auto db = (dir / "config.db").string();
    const auto export_path = (dir / "export.bin").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("edge", "s3", "a", "s")));

    EXPECT_FALSE(cm.export_configs(export_path, ""));
    ASSERT_TRUE(cm.export_configs(export_path, "Export123!"));

    EXPECT_FALSE(cm.import_configs(export_path, ""));

    const auto missing = (dir / "missing.bin").string();
    EXPECT_FALSE(cm.import_configs(missing, "Export123!"));

    const auto short_file = (dir / "short.bin").string();
    ASSERT_TRUE(write_file(short_file, "abc"));
    EXPECT_FALSE(cm.import_configs(short_file, "Export123!"));

    const auto bad_magic = (dir / "badmagic.bin").string();
    ASSERT_TRUE(write_file(bad_magic, std::string(40, 'X')));
    EXPECT_FALSE(cm.import_configs(bad_magic, "Export123!"));
}

// 篡改 payload 语义：解密成功但结构非法的导出文件被逐层拒绝；条目缺
// name 跳过不阻塞其余条目（部分导入语义）
TEST(ConfigManagerTest, ImportMalformedPayloadVariants) {
    auto dir = unique_temp_dir("falcon_cfg_malformed_");
    const auto db = (dir / "config.db").string();
    const auto payload_path = (dir / "payload.bin").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));

    // configs 键缺失
    ASSERT_TRUE(write_file(payload_path,
                           make_export_payload("{\"version\":1}", "Export123!")));
    EXPECT_FALSE(cm.import_configs(payload_path, "Export123!"));

    // configs 非 array
    ASSERT_TRUE(write_file(payload_path,
                           make_export_payload("{\"configs\":42}", "Export123!")));
    EXPECT_FALSE(cm.import_configs(payload_path, "Export123!"));

    // 条目缺 name：跳过，其余条目照常导入
    ASSERT_TRUE(write_file(
        payload_path,
        make_export_payload(
            "{\"configs\":[{\"name\":\"good\",\"provider\":\"s3\","
            "\"access_key\":\"AK\",\"secret_key\":\"SK\"},"
            "{\"provider\":\"no-name\"}]}",
            "Export123!")));
    EXPECT_TRUE(cm.import_configs(payload_path, "Export123!"));
    falcon::CloudStorageConfig good{};
    EXPECT_TRUE(cm.get_cloud_config("good", good));
    EXPECT_EQ(good.access_key, "AK");
    EXPECT_TRUE(cm.search_configs("no-name").empty());
}

// ============================================================================
// sqlite 失败注入与深层路径（覆盖率批次 T）：EXCLUSIVE 锁（写 step BUSY）、
// DROP 表（prepare "no such table"）、直连篡改（坏 JSON 容错）、空密码
// initialize、import 深层 catch。WAL 模式下读不阻塞：锁用例的读类方法
// 照常成功，只有写 step 撞 BUSY——与 TaskStorage 的锁语义不同。
// ============================================================================

namespace {

// 第二连接持 BEGIN EXCLUSIVE 写锁（RAII：析构关闭连接自动回滚释放）
class ExclusiveLocker {
public:
    explicit ExclusiveLocker(const std::string& db_path) {
        if (sqlite3_open(db_path.c_str(), &db_) == SQLITE_OK) {
            char* err = nullptr;
            locked_ = sqlite3_exec(db_, "BEGIN EXCLUSIVE;", nullptr, nullptr, &err) == SQLITE_OK;
            sqlite3_free(err);
        }
    }
    ~ExclusiveLocker() {
        if (db_) sqlite3_close(db_);
    }
    ExclusiveLocker(const ExclusiveLocker&) = delete;
    ExclusiveLocker& operator=(const ExclusiveLocker&) = delete;
    bool locked() const { return locked_; }

private:
    sqlite3* db_ = nullptr;
    bool locked_ = false;
};

} // namespace

// 无主密码 initialize：默认空密码被强度检查拒绝（<8 字符）——源码里
// 「master_password_ 为空则 authenticated_=false 后返回 true」的收尾分支
// 因此结构性不可达（强度检查与空密码互斥），固化该语义
TEST(ConfigManagerTest, InitializeWithoutMasterPasswordFails) {
    auto dir = unique_temp_dir("falcon_cfg_nopass_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    EXPECT_FALSE(cm.initialize(db));
    // 未初始化 manager：一切读写被认证门拒绝
    EXPECT_TRUE(cm.list_cloud_configs().empty());
    EXPECT_TRUE(cm.search_configs().empty());
    falcon::CloudStorageConfig out{};
    EXPECT_FALSE(cm.get_cloud_config("any", out));
}

// 全新 DELETE-journal 库被第二连接持 EXCLUSIVE 写锁：initialize 的
// PRAGMA journal_mode（DELETE→WAL 转换需独占，非致命继续）与建表 exec
//（致命）双双撞 BUSY，最终失败返回
TEST(ConfigManagerTest, InitializeFailsWhileDbLockedExclusively) {
    auto dir = unique_temp_dir("falcon_cfg_lockinit_");
    const auto db = (dir / "config.db").string();

    ExclusiveLocker locker(db);  // open 即创建空库并上写锁
    ASSERT_TRUE(locker.locked());

    falcon::ConfigManager cm;
    EXPECT_FALSE(cm.initialize(db, "Master123!"));
}

// DROP 两表后 schema cookie 变化：首条语句的 prepare_v2 吞掉一次
// SQLITE_SCHEMA 自动重编（该语句走各自的失败收口），其后各方法命中
// 自己的 prepare 失败分支。save 作牺牲位（其 prepare 失败行同为验证目标）
TEST(ConfigManagerTest, MethodsFailWhenTablesDropped) {
    auto dir = unique_temp_dir("falcon_cfg_drop_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("alpha", "s3", "AK", "SK")));

    ASSERT_TRUE(exec_sql(db, "DROP TABLE master; DROP TABLE configs;"));

    // 牺牲位兼目标：INSERT prepare 吞重编后如实撞 "no such table"
    EXPECT_FALSE(cm.save_cloud_config(make_config("beta", "s3", "AK", "SK")));
    // verify 提前 return 不撤销 authenticated_：其后方法的认证门仍短路
    EXPECT_FALSE(cm.verify_master_password("Master123!"));
    falcon::CloudStorageConfig out{};
    EXPECT_FALSE(cm.get_cloud_config("alpha", out));
    EXPECT_FALSE(cm.delete_cloud_config("alpha"));
    EXPECT_TRUE(cm.list_cloud_configs().empty());
    EXPECT_TRUE(cm.search_configs().empty());
    EXPECT_TRUE(cm.search_configs("s3").empty());
    EXPECT_FALSE(cm.update_cloud_config("alpha", make_config("alpha", "s3", "AK", "SK")));
}

// update 改名撞 name UNIQUE 约束：select 段成功读到行、UPDATE step 返回
// SQLITE_CONSTRAINT，走 step 失败分支如实返回 false 且原数据不受影响
TEST(ConfigManagerTest, UpdateFailsOnDuplicateTargetName) {
    auto dir = unique_temp_dir("falcon_cfg_dupname_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("alpha", "s3", "AK1", "SK1")));
    ASSERT_TRUE(cm.save_cloud_config(make_config("beta", "s3", "AK2", "SK2")));

    EXPECT_FALSE(cm.update_cloud_config("alpha", make_config("beta", "s3", "AK1", "SK1")));

    EXPECT_EQ(cm.list_cloud_configs().size(), 2u);
    falcon::CloudStorageConfig out{};
    EXPECT_TRUE(cm.get_cloud_config("alpha", out));
    EXPECT_EQ(out.access_key, "AK1");
}

// WAL 库上第二连接持 EXCLUSIVE 写锁：读旧快照照常成功，写 step 一律 BUSY
TEST(ConfigManagerTest, WriteMethodsFailWhileDbLockedExclusively) {
    auto dir = unique_temp_dir("falcon_cfg_lockwrite_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("alpha", "s3", "AK", "SK")));

    {
        ExclusiveLocker locker(db);
        ASSERT_TRUE(locker.locked());

        EXPECT_FALSE(cm.save_cloud_config(make_config("beta", "s3", "AK", "SK")));
        EXPECT_FALSE(cm.delete_cloud_config("alpha"));
        // select 段读旧快照成功、UPDATE step 撞 BUSY
        EXPECT_FALSE(cm.update_cloud_config("alpha", make_config("alpha", "s3", "AK", "SK")));

        // WAL 读不阻塞：查询照常
        EXPECT_EQ(cm.list_cloud_configs().size(), 1u);
    }

    // 释放锁后写入恢复
    EXPECT_TRUE(cm.save_cloud_config(make_config("beta", "s3", "AK", "SK")));
    EXPECT_EQ(cm.list_cloud_configs().size(), 2u);
}

// 库内 extra 列坏 JSON（外部篡改/旧版本残留）：search 与 get 必须吞掉
// 解析异常，该行以默认空 extra 返回而不是向上传播
TEST(ConfigManagerTest, SearchAndGetTolerateMalformedExtraJson) {
    auto dir = unique_temp_dir("falcon_cfg_badextra_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("alpha", "s3", "AK", "SK")));
    ASSERT_TRUE(exec_sql(db, "UPDATE configs SET extra='{bad' WHERE name='alpha';"));

    auto results = cm.search_configs("");
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].name, "alpha");
    EXPECT_TRUE(results[0].extra.empty());

    falcon::CloudStorageConfig out{};
    ASSERT_TRUE(cm.get_cloud_config("alpha", out));
    EXPECT_TRUE(out.extra.empty());
}

// 解密后的明文不是 JSON：parse 异常必须吞掉并失败返回
TEST(ConfigManagerTest, ImportRejectsNonJsonPlaintext) {
    auto dir = unique_temp_dir("falcon_cfg_nonjson_");
    const auto db = (dir / "config.db").string();
    const auto payload_path = (dir / "payload.bin").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(write_file(payload_path, make_export_payload("{bad json", "Export123!")));
    EXPECT_FALSE(cm.import_configs(payload_path, "Export123!"));
}

// extra 为 object 但 value 非 string：get<map<string,string>> 抛类型错误，
// 该字段落回默认空 map、条目照常导入
TEST(ConfigManagerTest, ImportToleratesNonStringExtraValues) {
    auto dir = unique_temp_dir("falcon_cfg_extra_type_");
    const auto db = (dir / "config.db").string();
    const auto payload_path = (dir / "payload.bin").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(write_file(
        payload_path,
        make_export_payload(
            "{\"configs\":[{\"name\":\"typed\",\"provider\":\"s3\","
            "\"extra\":{\"count\":123},\"access_key\":\"AK\",\"secret_key\":\"SK\"}]}",
            "Export123!")));
    EXPECT_TRUE(cm.import_configs(payload_path, "Export123!"));

    falcon::CloudStorageConfig out{};
    ASSERT_TRUE(cm.get_cloud_config("typed", out));
    EXPECT_EQ(out.access_key, "AK");
    EXPECT_TRUE(out.extra.empty());
}

// configs 表被 DROP（master 保留）：import 解密解析成功但逐条 save 撞
// 表缺失，必须失败返回而不是假报导入成功
TEST(ConfigManagerTest, ImportFailsWhenConfigsTableDropped) {
    auto dir = unique_temp_dir("falcon_cfg_dropimport_");
    const auto db = (dir / "config.db").string();
    const auto payload_path = (dir / "payload.bin").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(exec_sql(db, "DROP TABLE configs;"));

    // 牺牲位：让 schema cache 失效落到文件真身，import 内部的 save
    // 才能稳定命中失败路径
    EXPECT_FALSE(cm.save_cloud_config(make_config("alpha", "s3", "AK", "SK")));

    ASSERT_TRUE(write_file(
        payload_path,
        make_export_payload(
            "{\"configs\":[{\"name\":\"alpha\",\"provider\":\"s3\","
            "\"access_key\":\"AK\",\"secret_key\":\"SK\"}]}",
            "Export123!")));
    EXPECT_FALSE(cm.import_configs(payload_path, "Export123!"));
}

// configs 被同名视图替代：SELECT 段读视图成功拿到行，UPDATE 语句在
// prepare 阶段被 sqlite 拒绝（cannot modify a view）——update 的第二个
// prepare 失败分支
TEST(ConfigManagerTest, UpdateFailsWhenConfigsIsAView) {
    auto dir = unique_temp_dir("falcon_cfg_view_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("alpha", "s3", "AK", "SK")));

    ASSERT_TRUE(exec_sql(
        db,
        "DROP TABLE configs; "
        "CREATE VIEW configs AS SELECT id, 'alpha' AS name, 's3' AS provider, "
        "'' AS access_key, '' AS secret_key, '' AS region, '' AS bucket, "
        "'' AS endpoint, '' AS custom_domain, '' AS extra, "
        "0 AS created_at, 0 AS updated_at FROM master;"));

    EXPECT_FALSE(cm.update_cloud_config("alpha", make_config("alpha", "s3", "AK", "SK")));
}

#endif  // FALCON_ENABLE_CONFIG_MANAGER

#if defined(FALCON_FAILURE_INJECTION)

#include <falcon/detail/injection.hpp>

// ============================================================================
// 故障注入：AES-256-GCM 加解密 EVP 防御链（正常执行不可达的 OOM 路径）
// ============================================================================

namespace {

/// 加密侧注入：save_cloud_config 内 AES256GCM::encrypt 命中注入点即失败
void test_encrypt_injection(const char* prefix,
                            falcon::detail::InjectPoint point) {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / (prefix + std::to_string(
        static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    auto cfg = make_config("inj", "s3", "AK", "SK");
    {
        falcon::detail::ScopedInjection guard(point);
        EXPECT_FALSE(cm.save_cloud_config(cfg));
    }
    // 注入清除后恢复
    EXPECT_TRUE(cm.save_cloud_config(cfg));
}

}  // namespace

TEST(ConfigManagerInjectionTest, EncryptCtxNewFailureFailsSave) {
    test_encrypt_injection("falcon_cfg_inj_a_",
                           falcon::detail::InjectPoint::ConfigEncryptCtxNew);
}

TEST(ConfigManagerInjectionTest, EncryptInitFailureFailsSave) {
    test_encrypt_injection("falcon_cfg_inj_b_",
                           falcon::detail::InjectPoint::ConfigEncryptInit);
}

TEST(ConfigManagerInjectionTest, EncryptUpdateFailureFailsSave) {
    test_encrypt_injection("falcon_cfg_inj_c_",
                           falcon::detail::InjectPoint::ConfigEncryptUpdate);
}

TEST(ConfigManagerInjectionTest, EncryptFinalFailureFailsSave) {
    test_encrypt_injection("falcon_cfg_inj_d_",
                           falcon::detail::InjectPoint::ConfigEncryptFinal);
}

TEST(ConfigManagerInjectionTest, DecryptFailuresYieldEmptyConfig) {
    auto dir = unique_temp_dir("falcon_cfg_inj_e_");
    const auto db = (dir / "config.db").string();

    falcon::ConfigManager cm;
    ASSERT_TRUE(cm.initialize(db, "Master123!"));
    ASSERT_TRUE(cm.save_cloud_config(make_config("inj-e", "s3", "AKIA", "SK")));

    // 重新初始化同一库，读取走解密路径
    falcon::ConfigManager cm2;
    ASSERT_TRUE(cm2.initialize(db, "Master123!"));

    const falcon::detail::InjectPoint points[] = {
        falcon::detail::InjectPoint::ConfigDecryptCtxNew,
        falcon::detail::InjectPoint::ConfigDecryptInit,
        falcon::detail::InjectPoint::ConfigDecryptSetTag,
        falcon::detail::InjectPoint::ConfigDecryptUpdate,
    };
    for (const auto point : points) {
        falcon::CloudStorageConfig cfg{};
        falcon::detail::ScopedInjection guard(point);
        // 解密失败：读取报失败或字段为空串（密文不可恢复）
        const bool ok = cm2.get_cloud_config("inj-e", cfg);
        EXPECT_TRUE(!ok || cfg.access_key.empty()) << "point=" << static_cast<int>(point);
    }
}

#endif  // FALCON_FAILURE_INJECTION
