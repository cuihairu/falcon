/**
 * @file config_manager.cpp
 * @brief 配置管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/config_manager.hpp>
#include <falcon/logger.hpp>
#include <sqlite3.h>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <ctime>

using json = nlohmann::json;

namespace falcon {

namespace {
constexpr char kExportMagic[] = "FALCONCFG1";
constexpr size_t kExportMagicLen = sizeof(kExportMagic) - 1;

bool read_file_binary(const std::string& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    std::streamoff len = in.tellg();
    if (len < 0) return false;
    in.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(len));
    if (!out.empty()) {
        in.read(&out[0], static_cast<std::streamsize>(len));
        if (!in) return false;
    }
    return true;
}

bool write_file_binary(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!data.empty()) {
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) return false;
    }
    return true;
}
} // namespace

/**
 * @brief AES-256-GCM加密工具
 */
class AES256GCM {
public:
    static std::string encrypt(const std::string& plaintext, const std::string& key) {
        // 生成随机IV
        unsigned char iv[12];
        RAND_bytes(iv, 12);

        // 派生密钥
        unsigned char derived_key[32];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, key.c_str(), key.length());
        SHA256_Final(derived_key, &sha256);

        // 加密
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, derived_key, iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }

        int len;
        std::string ciphertext;
        ciphertext.resize(plaintext.size() + 16); // GCM tag

        if (EVP_EncryptUpdate(ctx, (unsigned char*)&ciphertext[0], &len,
                             (unsigned char*)plaintext.c_str(), plaintext.length()) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }

        int ciphertext_len = len;

        if (EVP_EncryptFinal_ex(ctx, (unsigned char*)&ciphertext[len], &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }
        ciphertext_len += len;

        // 获取tag
        unsigned char tag[16];
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);

        EVP_CIPHER_CTX_free(ctx);

        // 组合：IV + ciphertext + tag
        std::string result;
        result.append((char*)iv, 12);
        result.append(ciphertext.substr(0, ciphertext_len));
        result.append((char*)tag, 16);

        return result;
    }

    static std::string decrypt(const std::string& ciphertext, const std::string& key) {
        if (ciphertext.length() < 28) { // 12 IV + at least 1 byte data + 16 tag
            return "";
        }

        // 提取IV、加密数据和tag
        const unsigned char* iv = (unsigned char*)ciphertext.c_str();
        const unsigned char* enc_data = iv + 12;
        size_t enc_len = ciphertext.length() - 28;
        const unsigned char* tag = enc_data + enc_len;

        // 派生密钥
        unsigned char derived_key[32];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, key.c_str(), key.length());
        SHA256_Final(derived_key, &sha256);

        // 解密
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, derived_key, iv) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }

        // 设置tag
        if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, (void*)tag) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }

        std::string plaintext;
        plaintext.resize(enc_len);
        int len;

        if (EVP_DecryptUpdate(ctx, (unsigned char*)&plaintext[0], &len,
                             enc_data, enc_len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }

        int plaintext_len = len;

        if (EVP_DecryptFinal_ex(ctx, (unsigned char*)&plaintext[len], &len) != 1) {
            EVP_CIPHER_CTX_free(ctx);
            return "";
        }
        plaintext_len += len;

        EVP_CIPHER_CTX_free(ctx);

        plaintext.resize(plaintext_len);
        return plaintext;
    }
};

/**
 * @brief 配置管理器实现细节
 */
class ConfigManager::Impl {
public:
    Impl() : db_(nullptr) {}

    ~Impl() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    bool initialize(const std::string& db_path, const std::string& master_password) {
        // 打开数据库
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            log_error(std::string("Cannot open database: ") + sqlite3_errmsg(db_));
            return false;
        }

        // 设置主密码（如果提供）
        if (!master_password.empty()) {
            master_password_ = master_password;
        }

        // 创建表
        const char* create_table_sql = R"(
            CREATE TABLE IF NOT EXISTS configs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                name TEXT UNIQUE NOT NULL,
                provider TEXT NOT NULL,
                access_key TEXT,
                secret_key TEXT,
                region TEXT,
                bucket TEXT,
                endpoint TEXT,
                custom_domain TEXT,
                extra TEXT,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS master (
                id INTEGER PRIMARY KEY,
                password_hash TEXT NOT NULL,
                salt TEXT NOT NULL
            );
        )";

        char* err_msg = nullptr;
        rc = sqlite3_exec(db_, create_table_sql, nullptr, nullptr, &err_msg);
        if (rc != SQLITE_OK) {
            log_error(std::string("SQL error: ") + (err_msg ? err_msg : ""));
            sqlite3_free(err_msg);
            return false;
        }

        // 初始化主密码
        if (!master_password_.empty()) {
            return set_master_password_internal(master_password_);
        }

        return true;
    }

    bool set_master_password(const std::string& password) {
        master_password_ = password;
        return set_master_password_internal(password);
    }

    bool set_master_password_internal(const std::string& password) {
        // 生成随机盐
        unsigned char salt[16];
        RAND_bytes(salt, 16);

        // 计算密码哈希（PBKDF2）
        std::string salt_str((char*)salt, 16);
        std::string hash = pbkdf2_sha256(password, salt_str, 100000);

        // 存储到数据库
        std::string sql = "INSERT OR REPLACE INTO master (id, password_hash, salt) VALUES (1, ?, ?)";
        sqlite3_stmt* stmt = nullptr;

        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, hash.c_str(), hash.length(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 2, salt, 16, SQLITE_TRANSIENT);

        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return result == SQLITE_DONE;
    }

    bool verify_master_password(const std::string& password) {
        if (!db_) return false;
        // 从数据库获取存储的哈希和盐
        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT password_hash, salt FROM master WHERE id = 1";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return false;
        }

        const char* stored_hash = (const char*)sqlite3_column_text(stmt, 0);
        const void* salt_blob = sqlite3_column_blob(stmt, 1);
        int salt_len = sqlite3_column_bytes(stmt, 1);

        std::string stored_hash_str(stored_hash);
        std::string salt_str((char*)salt_blob, salt_len);
        sqlite3_finalize(stmt);

        // 计算提供的密码的哈希
        std::string computed_hash = pbkdf2_sha256(password, salt_str, 100000);

        return computed_hash == stored_hash_str;
    }

    bool save_cloud_config(const CloudStorageConfig& config) {
        // 验证主密码
        if (!verify_master_password(master_password_)) {
            return false;
        }

        // 加密敏感信息
        std::string encrypted_access = AES256GCM::encrypt(config.access_key, master_password_);
        std::string encrypted_secret = AES256GCM::encrypt(config.secret_key, master_password_);

        // 序列化额外配置
        std::string extra_json;
        if (!config.extra.empty()) {
            extra_json = json(config.extra).dump();
        }

        time_t now = time(nullptr);
        time_t updated_at = config.updated_at ? config.updated_at : now;

        sqlite3_stmt* stmt = nullptr;
        const char* sql = R"(
            INSERT OR REPLACE INTO configs
            (name, provider, access_key, secret_key, region, bucket,
             endpoint, custom_domain, extra, created_at, updated_at)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        )";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, config.name.c_str(), config.name.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, config.provider.c_str(), config.provider.length(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, encrypted_access.data(), encrypted_access.length(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 4, encrypted_secret.data(), encrypted_secret.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, config.region.c_str(), config.region.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, config.bucket.c_str(), config.bucket.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, config.endpoint.c_str(), config.endpoint.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, config.custom_domain.c_str(), config.custom_domain.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, extra_json.c_str(), extra_json.length(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 10, config.created_at ? config.created_at : now);
        sqlite3_bind_int64(stmt, 11, updated_at);

        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        return result == SQLITE_DONE;
    }

    bool get_cloud_config(const std::string& name, CloudStorageConfig& config) {
        if (!verify_master_password(master_password_)) {
            return false;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = R"(
            SELECT provider, access_key, secret_key, region, bucket,
                   endpoint, custom_domain, extra, created_at, updated_at
            FROM configs WHERE name = ?
        )";

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, name.c_str(), name.length(), SQLITE_TRANSIENT);

        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            return false;
        }

        config.name = name;
        config.provider = (const char*)sqlite3_column_text(stmt, 0);

        // 解密敏感信息
        const void* access_blob = sqlite3_column_blob(stmt, 1);
        int access_len = sqlite3_column_bytes(stmt, 1);
        if (access_blob && access_len > 0) {
            std::string encrypted_access((char*)access_blob, access_len);
            config.access_key = AES256GCM::decrypt(encrypted_access, master_password_);
        }

        const void* secret_blob = sqlite3_column_blob(stmt, 2);
        int secret_len = sqlite3_column_bytes(stmt, 2);
        if (secret_blob && secret_len > 0) {
            std::string encrypted_secret((char*)secret_blob, secret_len);
            config.secret_key = AES256GCM::decrypt(encrypted_secret, master_password_);
        }

        config.region = (const char*)sqlite3_column_text(stmt, 3);
        config.bucket = (const char*)sqlite3_column_text(stmt, 4);
        config.endpoint = (const char*)sqlite3_column_text(stmt, 5);
        config.custom_domain = (const char*)sqlite3_column_text(stmt, 6);
        config.created_at = sqlite3_column_int64(stmt, 8);
        config.updated_at = sqlite3_column_int64(stmt, 9);

        // 解析额外配置
        const char* extra_json = (const char*)sqlite3_column_text(stmt, 7);
        if (extra_json) {
            try {
                json j = json::parse(extra_json);
                config.extra = j.get<std::map<std::string, std::string>>();
            } catch (...) {
                // 忽略解析错误
            }
        }

        sqlite3_finalize(stmt);
        return true;
    }

    bool delete_cloud_config(const std::string& name) {
        if (!verify_master_password(master_password_)) {
            return false;
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "DELETE FROM configs WHERE name = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, name.c_str(), name.length(), SQLITE_TRANSIENT);
        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            return false;
        }
        return sqlite3_changes(db_) > 0;
    }

    std::vector<std::string> list_cloud_configs() {
        if (!verify_master_password(master_password_)) {
            return {};
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql = "SELECT name FROM configs ORDER BY name ASC";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return {};
        }

        std::vector<std::string> names;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const unsigned char* name_text = sqlite3_column_text(stmt, 0);
            if (name_text) {
                names.emplace_back(reinterpret_cast<const char*>(name_text));
            }
        }

        sqlite3_finalize(stmt);
        return names;
    }

    std::vector<CloudStorageConfig> search_configs(const std::string& provider) {
        if (!verify_master_password(master_password_)) {
            return {};
        }

        sqlite3_stmt* stmt = nullptr;
        const char* sql_all = R"(
            SELECT name, provider, access_key, secret_key, region, bucket,
                   endpoint, custom_domain, extra, created_at, updated_at
            FROM configs ORDER BY name ASC
        )";
        const char* sql_filtered = R"(
            SELECT name, provider, access_key, secret_key, region, bucket,
                   endpoint, custom_domain, extra, created_at, updated_at
            FROM configs WHERE provider = ? ORDER BY name ASC
        )";

        const char* sql = provider.empty() ? sql_all : sql_filtered;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return {};
        }
        if (!provider.empty()) {
            sqlite3_bind_text(stmt, 1, provider.c_str(), provider.length(), SQLITE_TRANSIENT);
        }

        std::vector<CloudStorageConfig> results;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            CloudStorageConfig cfg{};

            const unsigned char* name_text = sqlite3_column_text(stmt, 0);
            const unsigned char* provider_text = sqlite3_column_text(stmt, 1);
            cfg.name = name_text ? reinterpret_cast<const char*>(name_text) : "";
            cfg.provider = provider_text ? reinterpret_cast<const char*>(provider_text) : "";

            const void* access_blob = sqlite3_column_blob(stmt, 2);
            int access_len = sqlite3_column_bytes(stmt, 2);
            if (access_blob && access_len > 0) {
                std::string encrypted_access(reinterpret_cast<const char*>(access_blob), access_len);
                cfg.access_key = AES256GCM::decrypt(encrypted_access, master_password_);
            }

            const void* secret_blob = sqlite3_column_blob(stmt, 3);
            int secret_len = sqlite3_column_bytes(stmt, 3);
            if (secret_blob && secret_len > 0) {
                std::string encrypted_secret(reinterpret_cast<const char*>(secret_blob), secret_len);
                cfg.secret_key = AES256GCM::decrypt(encrypted_secret, master_password_);
            }

            const unsigned char* region_text = sqlite3_column_text(stmt, 4);
            const unsigned char* bucket_text = sqlite3_column_text(stmt, 5);
            const unsigned char* endpoint_text = sqlite3_column_text(stmt, 6);
            const unsigned char* custom_domain_text = sqlite3_column_text(stmt, 7);
            cfg.region = region_text ? reinterpret_cast<const char*>(region_text) : "";
            cfg.bucket = bucket_text ? reinterpret_cast<const char*>(bucket_text) : "";
            cfg.endpoint = endpoint_text ? reinterpret_cast<const char*>(endpoint_text) : "";
            cfg.custom_domain = custom_domain_text ? reinterpret_cast<const char*>(custom_domain_text) : "";

            const unsigned char* extra_text = sqlite3_column_text(stmt, 8);
            if (extra_text) {
                try {
                    json j = json::parse(reinterpret_cast<const char*>(extra_text));
                    cfg.extra = j.get<std::map<std::string, std::string>>();
                } catch (...) {
                }
            }

            cfg.created_at = static_cast<time_t>(sqlite3_column_int64(stmt, 9));
            cfg.updated_at = static_cast<time_t>(sqlite3_column_int64(stmt, 10));

            results.push_back(std::move(cfg));
        }

        sqlite3_finalize(stmt);
        return results;
    }

    bool update_cloud_config(const std::string& name, const CloudStorageConfig& config) {
        if (!verify_master_password(master_password_)) {
            return false;
        }

        // 保留原 created_at
        sqlite3_stmt* select_stmt = nullptr;
        const char* select_sql = "SELECT created_at FROM configs WHERE name = ?";
        if (sqlite3_prepare_v2(db_, select_sql, -1, &select_stmt, nullptr) != SQLITE_OK) {
            return false;
        }
        sqlite3_bind_text(select_stmt, 1, name.c_str(), name.length(), SQLITE_TRANSIENT);
        if (sqlite3_step(select_stmt) != SQLITE_ROW) {
            sqlite3_finalize(select_stmt);
            return false;
        }
        time_t created_at = static_cast<time_t>(sqlite3_column_int64(select_stmt, 0));
        sqlite3_finalize(select_stmt);

        std::string encrypted_access = AES256GCM::encrypt(config.access_key, master_password_);
        std::string encrypted_secret = AES256GCM::encrypt(config.secret_key, master_password_);

        std::string extra_json;
        if (!config.extra.empty()) {
            extra_json = json(config.extra).dump();
        }

        const std::string new_name = config.name.empty() ? name : config.name;
        time_t now = time(nullptr);
        time_t updated_at = config.updated_at ? config.updated_at : now;

        sqlite3_stmt* stmt = nullptr;
        const char* update_sql = R"(
            UPDATE configs
            SET name = ?, provider = ?, access_key = ?, secret_key = ?, region = ?, bucket = ?,
                endpoint = ?, custom_domain = ?, extra = ?, created_at = ?, updated_at = ?
            WHERE name = ?
        )";

        if (sqlite3_prepare_v2(db_, update_sql, -1, &stmt, nullptr) != SQLITE_OK) {
            return false;
        }

        sqlite3_bind_text(stmt, 1, new_name.c_str(), new_name.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, config.provider.c_str(), config.provider.length(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 3, encrypted_access.data(), encrypted_access.length(), SQLITE_TRANSIENT);
        sqlite3_bind_blob(stmt, 4, encrypted_secret.data(), encrypted_secret.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, config.region.c_str(), config.region.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, config.bucket.c_str(), config.bucket.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, config.endpoint.c_str(), config.endpoint.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, config.custom_domain.c_str(), config.custom_domain.length(), SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, extra_json.c_str(), extra_json.length(), SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 10, created_at);
        sqlite3_bind_int64(stmt, 11, updated_at);
        sqlite3_bind_text(stmt, 12, name.c_str(), name.length(), SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc != SQLITE_DONE) {
            return false;
        }
        return sqlite3_changes(db_) > 0;
    }

    bool export_configs(const std::string& export_path, const std::string& export_password) {
        if (export_password.empty()) {
            return false;
        }

        // 需要主密码解密数据库内容
        if (!verify_master_password(master_password_)) {
            return false;
        }

        auto configs = search_configs("");
        json root;
        root["version"] = 1;
        root["exported_at"] = static_cast<int64_t>(time(nullptr));
        root["configs"] = json::array();

        for (const auto& cfg : configs) {
            json j;
            j["name"] = cfg.name;
            j["provider"] = cfg.provider;
            j["access_key"] = cfg.access_key;
            j["secret_key"] = cfg.secret_key;
            j["region"] = cfg.region;
            j["bucket"] = cfg.bucket;
            j["endpoint"] = cfg.endpoint;
            j["custom_domain"] = cfg.custom_domain;
            j["extra"] = cfg.extra;
            j["created_at"] = static_cast<int64_t>(cfg.created_at);
            j["updated_at"] = static_cast<int64_t>(cfg.updated_at);
            root["configs"].push_back(std::move(j));
        }

        std::string plaintext = root.dump();
        std::string encrypted = AES256GCM::encrypt(plaintext, export_password);
        if (encrypted.empty()) {
            return false;
        }

        std::string payload;
        payload.reserve(kExportMagicLen + encrypted.size());
        payload.append(kExportMagic, kExportMagicLen);
        payload.append(encrypted);

        return write_file_binary(export_path, payload);
    }

    bool import_configs(const std::string& import_path, const std::string& import_password) {
        if (import_password.empty()) {
            return false;
        }
        if (!verify_master_password(master_password_)) {
            return false;
        }

        std::string payload;
        if (!read_file_binary(import_path, payload)) {
            return false;
        }
        if (payload.size() < kExportMagicLen) {
            return false;
        }
        if (payload.compare(0, kExportMagicLen, kExportMagic, kExportMagicLen) != 0) {
            return false;
        }

        std::string encrypted = payload.substr(kExportMagicLen);
        std::string plaintext = AES256GCM::decrypt(encrypted, import_password);
        if (plaintext.empty()) {
            return false;
        }

        json root;
        try {
            root = json::parse(plaintext);
        } catch (...) {
            return false;
        }

        if (!root.contains("configs") || !root["configs"].is_array()) {
            return false;
        }

        for (const auto& j : root["configs"]) {
            CloudStorageConfig cfg{};
            cfg.name = j.value("name", "");
            cfg.provider = j.value("provider", "");
            cfg.access_key = j.value("access_key", "");
            cfg.secret_key = j.value("secret_key", "");
            cfg.region = j.value("region", "");
            cfg.bucket = j.value("bucket", "");
            cfg.endpoint = j.value("endpoint", "");
            cfg.custom_domain = j.value("custom_domain", "");
            if (j.contains("extra") && j["extra"].is_object()) {
                try {
                    cfg.extra = j["extra"].get<std::map<std::string, std::string>>();
                } catch (...) {
                }
            }
            cfg.created_at = static_cast<time_t>(j.value("created_at", 0LL));
            cfg.updated_at = static_cast<time_t>(j.value("updated_at", 0LL));

            if (cfg.name.empty() || cfg.provider.empty()) {
                continue;
            }

            if (!save_cloud_config(cfg)) {
                return false;
            }
        }

        return true;
    }

    std::string pbkdf2_sha256(const std::string& password, const std::string& salt, int iterations) {
        unsigned char hash[32];
        PKCS5_PBKDF2_HMAC(password.c_str(), password.length(),
                         (unsigned char*)salt.c_str(), salt.length(),
                         iterations, EVP_sha256(), 32, hash);

        std::string result;
        result.reserve(64);
        for (int i = 0; i < 32; ++i) {
            char buf[3];
            sprintf(buf, "%02x", hash[i]);
            result += buf;
        }
        return result;
    }

    sqlite3* db_;
    std::string master_password_;
};

// ConfigManager 公共接口实现
ConfigManager::ConfigManager() : p_impl_(std::make_unique<Impl>()) {}

ConfigManager::~ConfigManager() = default;

bool ConfigManager::initialize(const std::string& db_path, const std::string& master_password) {
    return p_impl_->initialize(db_path, master_password);
}

bool ConfigManager::set_master_password(const std::string& password) {
    return p_impl_->set_master_password(password);
}

bool ConfigManager::verify_master_password(const std::string& password) {
    return p_impl_->verify_master_password(password);
}

bool ConfigManager::save_cloud_config(const CloudStorageConfig& config) {
    return p_impl_->save_cloud_config(config);
}

bool ConfigManager::get_cloud_config(const std::string& name, CloudStorageConfig& config) {
    return p_impl_->get_cloud_config(name, config);
}

bool ConfigManager::delete_cloud_config(const std::string& name) {
    return p_impl_->delete_cloud_config(name);
}

std::vector<std::string> ConfigManager::list_cloud_configs() {
    return p_impl_->list_cloud_configs();
}

std::vector<CloudStorageConfig> ConfigManager::search_configs(const std::string& provider) {
    return p_impl_->search_configs(provider);
}

bool ConfigManager::update_cloud_config(const std::string& name, const CloudStorageConfig& config) {
    return p_impl_->update_cloud_config(name, config);
}

bool ConfigManager::export_configs(const std::string& export_path, const std::string& export_password) {
    return p_impl_->export_configs(export_path, export_password);
}

bool ConfigManager::import_configs(const std::string& import_path, const std::string& import_password) {
    return p_impl_->import_configs(import_path, import_password);
}

} // namespace falcon
