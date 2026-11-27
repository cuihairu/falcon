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
        PluginInjectionGuard guard(point);
        // 解密失败：读取报失败或字段为空串（密文不可恢复）
        const bool ok = cm2.get_cloud_config("inj-e", cfg);
        EXPECT_TRUE(!ok || cfg.access_key.empty()) << "point=" << static_cast<int>(point);
    }
}

#endif  // FALCON_FAILURE_INJECTION
