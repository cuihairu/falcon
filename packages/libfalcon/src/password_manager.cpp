/**
 * @file password_manager.cpp
 * @brief 密码管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/password_manager.hpp>
#include <falcon/logger.hpp>
#include <iostream>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdlib>

#ifdef _WIN32
// Windows 头文件定义了 min/max 宏，会影响 std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#if __has_include(<openssl/rand.h>) && __has_include(<openssl/sha256.h>)
#include <openssl/rand.h>
#include <openssl/sha256.h>
#define FALCON_PASSWORD_USE_OPENSSL 1
#else
#define FALCON_PASSWORD_USE_OPENSSL 0
#endif

namespace falcon {

namespace {

bool random_bytes(unsigned char* out, size_t len) {
#if FALCON_PASSWORD_USE_OPENSSL
    return RAND_bytes(out, static_cast<int>(len)) == 1;
#else
    std::random_device rd;
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<unsigned char>(rd());
    }
    return true;
#endif
}

void hash_32(const unsigned char* data, size_t len, unsigned char out[32]) {
#if FALCON_PASSWORD_USE_OPENSSL
    SHA256(data, len, out);
#else
    // Non-cryptographic fallback: expand std::hash into 32 bytes.
    std::hash<std::string> hasher;
    std::string s(reinterpret_cast<const char*>(data), len);
    size_t h1 = hasher(s);
    size_t h2 = hasher("salt:" + s);
    size_t h3 = hasher(s + ":more");
    size_t h4 = hasher("more:" + s);
    std::memcpy(out + 0, &h1, (sizeof(h1) < 8 ? sizeof(h1) : 8));
    std::memcpy(out + 8, &h2, (sizeof(h2) < 8 ? sizeof(h2) : 8));
    std::memcpy(out + 16, &h3, (sizeof(h3) < 8 ? sizeof(h3) : 8));
    std::memcpy(out + 24, &h4, (sizeof(h4) < 8 ? sizeof(h4) : 8));
#endif
}

std::string password_hash_path() {
    const char* home = std::getenv("HOME");
    if (!home || std::string(home).empty()) {
        return ".falcon/.password_hash";
    }
    return std::string(home) + "/.falcon/.password_hash";
}

} // namespace

/**
 * @brief 密码管理器实现细节
 */
class PasswordManager::Impl {
public:
    Impl() : is_locked_(true), timeout_seconds_(300), last_access_time_(0) {
        // 尝试从文件加载主密码哈希
        load_master_password_hash();
    }

    ~Impl() {
        // 保存主密码哈希
        save_master_password_hash();
    }

    bool set_master_password(const std::string& password) {
        if (password.length() < 8) {
            log_error("密码长度至少8位");
            return false;
        }

        // 生成随机盐
        unsigned char salt[32];
        if (!random_bytes(salt, sizeof(salt))) {
            log_error("生成随机盐失败");
            return false;
        }

        // 计算密码哈希
        unsigned char hash_input[32 + 1024];
        size_t pw_len = std::min(password.size(), sizeof(hash_input) - 32);
        std::memcpy(hash_input, salt, 32);
        std::memcpy(hash_input + 32, password.data(), pw_len);
        unsigned char hash[32];
        hash_32(hash_input, 32 + pw_len, hash);

        // 存储盐和哈希
        memcpy(salt_, salt, 32);
        memcpy(password_hash_, hash, 32);
        has_password_ = true;
        is_locked_ = false;
        last_access_time_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return true;
    }

    bool verify_master_password(const std::string& password) {
        if (!has_password_) {
            return false;
        }

        // 计算输入密码的哈希
        unsigned char hash_input[32 + 1024];
        size_t pw_len = std::min(password.size(), sizeof(hash_input) - 32);
        std::memcpy(hash_input, salt_, 32);
        std::memcpy(hash_input + 32, password.data(), pw_len);
        unsigned char hash[32];
        hash_32(hash_input, 32 + pw_len, hash);

        // 比较
        return memcmp(hash, password_hash_, 32) == 0;
    }

    bool has_master_password() const {
        return has_password_;
    }

    bool lock_configs() {
        is_locked_ = true;
        return true;
    }

    bool unlock_configs(const std::string& password) {
        if (!has_password_) {
            return false;
        }
        if (!verify_master_password(password)) {
            return false;
        }

        is_locked_ = false;
        last_access_time_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return true;
    }

    bool is_unlocked() const {
        if (is_locked_) {
            return false;
        }

        // 检查超时
        if (timeout_seconds_ > 0) {
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now - last_access_time_ > timeout_seconds_) {
                is_locked_ = true;
                return false;
            }
        }

        return true;
    }

    void set_timeout(int seconds) {
        timeout_seconds_ = seconds;
    }

    std::function<std::string()> get_password_callback() const {
        return password_callback_;
    }

    void set_password_callback(std::function<std::string()> callback) {
        password_callback_ = callback;
    }

    std::string prompt_password(const std::string& prompt) {
        if (password_callback_) {
            return password_callback_();
        }

        // 默认控制台输入
        std::cout << prompt;
        std::cout.flush();

#ifdef _WIN32
        // Windows 实现
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode = 0;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));

        std::string password;
        std::getline(std::cin, password);

        SetConsoleMode(hStdin, mode);
        std::cout << std::endl;
#else
        // Unix/Linux 实现
        termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= static_cast<tcflag_t>(~ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        std::string password;
        std::getline(std::cin, password);

        // 恢复回显
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cout << std::endl;
#endif

        return password;
    }

    bool confirm_password(const std::string& password, const std::string& prompt) {
        std::string confirm = prompt_password(prompt);
        return password == confirm;
    }

    int check_password_strength(const std::string& password) {
        int score = 0;

        // 长度评分
        if (password.length() >= 8) score += 20;
        if (password.length() >= 12) score += 20;
        if (password.length() >= 16) score += 20;

        // 包含小写字母
        if (std::any_of(password.begin(), password.end(),
                       [](char c) { return std::islower(c); })) {
            score += 10;
        }

        // 包含大写字母
        if (std::any_of(password.begin(), password.end(),
                       [](char c) { return std::isupper(c); })) {
            score += 10;
        }

        // 包含数字
        if (std::any_of(password.begin(), password.end(),
                       [](char c) { return std::isdigit(c); })) {
            score += 10;
        }

        // 包含特殊字符
        if (std::any_of(password.begin(), password.end(),
                       [](char c) { return !std::isalnum(c); })) {
            score += 10;
        }

        return std::min(score, 100);
    }

    std::string generate_password(int length, bool use_symbols, bool use_numbers) {
        std::string chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
        if (use_numbers) {
            chars += "0123456789";
        }
        if (use_symbols) {
            chars += "!@#$%^&*()_+-=[]{}|;:,.<>?";
        }

        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<std::size_t> dis(0, chars.length() - 1);

        std::string password;
        password.reserve(static_cast<std::size_t>(length));
        for (int i = 0; i < length; ++i) {
            password += chars[static_cast<std::size_t>(dis(gen))];
        }

        return password;
    }

    void update_access_time() {
        last_access_time_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

private:
    void load_master_password_hash() {
        std::ifstream file(password_hash_path(), std::ios::binary);
        if (file.is_open()) {
            file.read((char*)salt_, 32);
            file.read((char*)password_hash_, 32);
            has_password_ = file.good();
        }
    }

    void save_master_password_hash() {
        if (!has_password_) return;

        const std::string file_path = password_hash_path();
        const std::filesystem::path dir = std::filesystem::path(file_path).parent_path();
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        std::ofstream file(file_path, std::ios::binary);
        if (file.is_open()) {
            file.write((char*)salt_, 32);
            file.write((char*)password_hash_, 32);
        }
    }

    mutable bool is_locked_;
    int timeout_seconds_;
    mutable time_t last_access_time_;
    std::function<std::string()> password_callback_;
    unsigned char salt_[32];
    unsigned char password_hash_[32];
    bool has_password_ = false;
};

// PasswordManager 公共接口实现
PasswordManager::PasswordManager() : p_impl_(std::make_unique<Impl>()) {}

PasswordManager::~PasswordManager() = default;

bool PasswordManager::set_master_password(const std::string& password) {
    return p_impl_->set_master_password(password);
}

bool PasswordManager::verify_master_password(const std::string& password) {
    return p_impl_->verify_master_password(password);
}

bool PasswordManager::has_master_password() const {
    return p_impl_->has_master_password();
}

bool PasswordManager::lock_configs() {
    return p_impl_->lock_configs();
}

bool PasswordManager::unlock_configs(const std::string& password) {
    return p_impl_->unlock_configs(password);
}

bool PasswordManager::is_unlocked() const {
    return p_impl_->is_unlocked();
}

void PasswordManager::set_timeout(int seconds) {
    p_impl_->set_timeout(seconds);
}

std::function<std::string()> PasswordManager::get_password_callback() const {
    return p_impl_->get_password_callback();
}

void PasswordManager::set_password_callback(std::function<std::string()> callback) {
    p_impl_->set_password_callback(callback);
}

std::string PasswordManager::prompt_password(const std::string& prompt) {
    return p_impl_->prompt_password(prompt);
}

bool PasswordManager::confirm_password(const std::string& password, const std::string& prompt) {
    return p_impl_->confirm_password(password, prompt);
}

int PasswordManager::check_password_strength(const std::string& password) {
    return p_impl_->check_password_strength(password);
}

std::string PasswordManager::generate_password(int length, bool use_symbols, bool use_numbers) {
    return p_impl_->generate_password(length, use_symbols, use_numbers);
}

} // namespace falcon
