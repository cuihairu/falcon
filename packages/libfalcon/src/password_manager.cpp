/**
 * @file password_manager.cpp
 * @brief 密码管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/password_manager.hpp>
#include <falcon/logger.hpp>
#include <iostream>
#include <termios.h>
#include <unistd.h>
#include <chrono>
#include <random>
#include <algorithm>
#include <cstring>
#include <openssl/rand.h>
#include <openssl/sha256.h>
#include <fstream>

namespace falcon {

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
            Logger::error("密码长度至少8位");
            return false;
        }

        // 生成随机盐
        unsigned char salt[32];
        RAND_bytes(salt, 32);

        // 计算密码哈希
        unsigned char hash[32];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, password.c_str(), password.length());
        SHA256_Update(&sha256, salt, 32);
        SHA256_Final(hash, &sha256);

        // 存储盐和哈希
        memcpy(salt_, salt, 32);
        memcpy(password_hash_, hash, 32);
        is_locked_ = false;
        last_access_time_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        return true;
    }

    bool verify_master_password(const std::string& password) {
        // 计算输入密码的哈希
        unsigned char hash[32];
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        SHA256_Update(&sha256, password.c_str(), password.length());
        SHA256_Update(&sha256, salt_, 32);
        SHA256_Final(hash, &sha256);

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

        // 禁用回显
        termios oldt, newt;
        tcgetattr(STDIN_FILENO, &oldt);
        newt = oldt;
        newt.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &newt);

        std::string password;
        std::getline(std::cin, password);

        // 恢复回显
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        std::cout << std::endl;

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
        std::uniform_int_distribution<> dis(0, chars.length() - 1);

        std::string password;
        password.reserve(length);
        for (int i = 0; i < length; ++i) {
            password += chars[dis(gen)];
        }

        return password;
    }

    void update_access_time() {
        last_access_time_ = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

private:
    void load_master_password_hash() {
        std::ifstream file("~/.falcon/.password_hash", std::ios::binary);
        if (file.is_open()) {
            file.read((char*)salt_, 32);
            file.read((char*)password_hash_, 32);
            has_password_ = true;
        }
    }

    void save_master_password_hash() {
        if (!has_password_) return;

        std::string home = std::getenv("HOME");
        std::string dir = home + "/.falcon";

        // 创建目录
        system(("mkdir -p " + dir).c_str());

        std::ofstream file(dir + "/.password_hash", std::ios::binary);
        if (file.is_open()) {
            file.write((char*)salt_, 32);
            file.write((char*)password_hash_, 32);
        }
    }

    bool is_locked_;
    int timeout_seconds_;
    time_t last_access_time_;
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