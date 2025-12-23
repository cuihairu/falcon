// Falcon Password Manager Unit Tests

#include <falcon/password_manager.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

class ScopedEnvVar {
public:
    ScopedEnvVar(const char* name, const std::string& value)
        : name_(name) {
        const char* old = std::getenv(name);
        if (old) {
            had_old_ = true;
            old_ = old;
        }
        set(value);
    }

    ~ScopedEnvVar() {
        if (had_old_) {
            set(old_);
        } else {
            unset();
        }
    }

    ScopedEnvVar(const ScopedEnvVar&) = delete;
    ScopedEnvVar& operator=(const ScopedEnvVar&) = delete;

private:
    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        unsetenv(name_.c_str());
#endif
    }

    std::string name_;
    bool had_old_ = false;
    std::string old_;
};

std::filesystem::path unique_temp_dir(const std::string& prefix) {
    auto base = std::filesystem::temp_directory_path();
    auto dir = base / (prefix + std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count())));
    std::filesystem::create_directories(dir);
    return dir;
}

} // namespace

TEST(PasswordManagerTest, SetVerifyLockUnlock) {
    auto home = unique_temp_dir("falcon_pw_home_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_FALSE(pm.has_master_password());
    EXPECT_FALSE(pm.verify_master_password("anything"));

    EXPECT_FALSE(pm.set_master_password("short"));
    EXPECT_TRUE(pm.set_master_password("GoodPass1!"));
    EXPECT_TRUE(pm.has_master_password());

    EXPECT_TRUE(pm.verify_master_password("GoodPass1!"));
    EXPECT_FALSE(pm.verify_master_password("WrongPass1!"));

    EXPECT_TRUE(pm.lock_configs());
    EXPECT_FALSE(pm.is_unlocked());

    EXPECT_FALSE(pm.unlock_configs("WrongPass1!"));
    EXPECT_TRUE(pm.unlock_configs("GoodPass1!"));
    EXPECT_TRUE(pm.is_unlocked());
}

TEST(PasswordManagerTest, StrengthAndGeneration) {
    falcon::PasswordManager pm;

    EXPECT_LT(pm.check_password_strength("abcdefg"), 40);
    EXPECT_GT(pm.check_password_strength("Abcdef12!"), 40);

    auto alpha_only = pm.generate_password(32, false, false);
    ASSERT_EQ(alpha_only.size(), 32u);
    for (char c : alpha_only) {
        EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(c)));
    }

    auto with_numbers = pm.generate_password(32, false, true);
    ASSERT_EQ(with_numbers.size(), 32u);
    for (char c : with_numbers) {
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)));
    }
}

TEST(PasswordManagerTest, PersistsToHomeDirectory) {
    auto home = unique_temp_dir("falcon_pw_home_persist_");
    ScopedEnvVar scoped_home("HOME", home.string());

    {
        falcon::PasswordManager pm;
        EXPECT_TRUE(pm.set_master_password("GoodPass1!"));
    }

    falcon::PasswordManager pm2;
    EXPECT_TRUE(pm2.has_master_password());
    EXPECT_TRUE(pm2.verify_master_password("GoodPass1!"));
    EXPECT_FALSE(pm2.verify_master_password("WrongPass1!"));
}
