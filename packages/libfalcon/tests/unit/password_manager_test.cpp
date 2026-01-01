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

//==============================================================================
// 密码强度测试
//==============================================================================

TEST(PasswordStrengthTest, EmptyPassword) {
    falcon::PasswordManager pm;
    EXPECT_EQ(pm.check_password_strength(""), 0);
}

TEST(PasswordStrengthTest, VeryShortPassword) {
    falcon::PasswordManager pm;
    EXPECT_LT(pm.check_password_strength("a"), 20);
    EXPECT_LT(pm.check_password_strength("abc"), 20);
}

TEST(PasswordStrengthTest, Exactly8Chars) {
    falcon::PasswordManager pm;
    EXPECT_GE(pm.check_password_strength("abcdefgh"), 20);
}

TEST(PasswordStrengthTest, OnlyLowercase) {
    falcon::PasswordManager pm;
    EXPECT_LT(pm.check_password_strength("abcdefgh"), 30);
}

TEST(PasswordStrengthTest, OnlyUppercase) {
    falcon::PasswordManager pm;
    EXPECT_LT(pm.check_password_strength("ABCDEFGH"), 30);
}

TEST(PasswordStrengthTest, OnlyNumbers) {
    falcon::PasswordManager pm;
    EXPECT_LT(pm.check_password_strength("12345678"), 30);
}

TEST(PasswordStrengthTest, MixedLowerUpper) {
    falcon::PasswordManager pm;
    EXPECT_GE(pm.check_password_strength("Abcdefgh"), 40);
}

TEST(PasswordStrengthTest, MixedWithNumbers) {
    falcon::PasswordManager pm;
    EXPECT_GE(pm.check_password_strength("Abcdef12"), 50);
}

TEST(PasswordStrengthTest, FullComplex) {
    falcon::PasswordManager pm;
    EXPECT_EQ(pm.check_password_strength("Abcdef12!"), 60);
}

TEST(PasswordStrengthTest, LongComplexPassword) {
    falcon::PasswordManager pm;
    EXPECT_EQ(pm.check_password_strength("Abcdef12!Ghijklm34@"), 100);
}

TEST(PasswordStrengthTest, VeryLongPassword) {
    falcon::PasswordManager pm;
    std::string very_long(100, 'A');
    very_long += "1a!";
    EXPECT_EQ(pm.check_password_strength(very_long), 100);
}

//==============================================================================
// 密码生成测试
//==============================================================================

TEST(PasswordGenerationTest, DefaultLength) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password();
    EXPECT_EQ(password.size(), 16u);
    EXPECT_NE(password.find("!"), std::string::npos);
}

TEST(PasswordGenerationTest, CustomLength) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(8);
    EXPECT_EQ(password.size(), 8u);
}

TEST(PasswordGenerationTest, VeryLongPassword) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(256);
    EXPECT_EQ(password.size(), 256u);
}

TEST(PasswordGenerationTest, ZeroLength) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(0);
    EXPECT_EQ(password.size(), 0u);
}

TEST(PasswordGenerationTest, AlphaOnly) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(32, false, false);
    EXPECT_EQ(password.size(), 32u);
    for (char c : password) {
        EXPECT_TRUE(std::isalpha(static_cast<unsigned char>(c)));
    }
}

TEST(PasswordGenerationTest, AlphaAndNumbers) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(32, false, true);
    EXPECT_EQ(password.size(), 32u);
    bool has_alpha = false;
    bool has_digit = false;
    for (char c : password) {
        if (std::isalpha(static_cast<unsigned char>(c))) has_alpha = true;
        if (std::isdigit(static_cast<unsigned char>(c))) has_digit = true;
        EXPECT_TRUE(std::isalnum(static_cast<unsigned char>(c)));
    }
    EXPECT_TRUE(has_alpha);
    EXPECT_TRUE(has_digit);
}

TEST(PasswordGenerationTest, AlphaAndSymbols) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(32, true, false);
    EXPECT_EQ(password.size(), 32u);
    bool has_alpha = false;
    bool has_symbol = false;
    for (char c : password) {
        if (std::isalpha(static_cast<unsigned char>(c))) has_alpha = true;
        if (!std::isalnum(static_cast<unsigned char>(c))) has_symbol = true;
    }
    EXPECT_TRUE(has_alpha);
    EXPECT_TRUE(has_symbol);
}

TEST(PasswordGenerationTest, AllCharTypes) {
    falcon::PasswordManager pm;
    auto password = pm.generate_password(64, true, true);
    EXPECT_EQ(password.size(), 64u);
}

//==============================================================================
// 密码回调测试
//==============================================================================

TEST(PasswordCallbackTest, SetAndGetCallback) {
    falcon::PasswordManager pm;

    bool callback_called = false;
    std::string test_password = "TestPass123!";

    pm.set_password_callback([&]() {
        callback_called = true;
        return test_password;
    });

    auto callback = pm.get_password_callback();
    ASSERT_TRUE(callback);

    std::string result = callback();
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(result, test_password);
}

TEST(PasswordCallbackTest, PromptWithCallback) {
    falcon::PasswordManager pm;

    std::string test_password = "CallbackPass123!";
    pm.set_password_callback([&]() { return test_password; });

    std::string result = pm.prompt_password("Enter password: ");
    EXPECT_EQ(result, test_password);
}

TEST(PasswordCallbackTest, ConfirmWithCallback) {
    falcon::PasswordManager pm;

    int call_count = 0;
    pm.set_password_callback([&]() {
        call_count++;
        return (call_count == 1) ? "Password123!" : "Password123!";
    });

    bool confirmed = pm.confirm_password("Password123!");
    EXPECT_TRUE(confirmed);
    EXPECT_EQ(call_count, 2);
}

TEST(PasswordCallbackTest, ConfirmWithMismatch) {
    falcon::PasswordManager pm;

    int call_count = 0;
    pm.set_password_callback([&]() {
        call_count++;
        return (call_count == 1) ? "Password123!" : "Different456!";
    });

    bool confirmed = pm.confirm_password("Password123!");
    EXPECT_FALSE(confirmed);
    EXPECT_EQ(call_count, 2);
}

//==============================================================================
// 超时测试
//==============================================================================

TEST(PasswordTimeoutTest, SetTimeout) {
    falcon::PasswordManager pm;

    pm.set_timeout(60);
    // 设置超时不会失败，只是验证
}

TEST(PasswordTimeoutTest, TimeoutZeroNeverExpires) {
    auto home = unique_temp_dir("falcon_pw_home_timeout_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_TRUE(pm.set_master_password("GoodPass1!"));

    pm.set_timeout(0);  // 永不过时

    EXPECT_TRUE(pm.is_unlocked());

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 应该仍然解锁
    EXPECT_TRUE(pm.is_unlocked());
}

TEST(PasswordTimeoutTest, ShortTimeout) {
    auto home = unique_temp_dir("falcon_pw_home_short_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_TRUE(pm.set_master_password("GoodPass1!"));

    pm.set_timeout(1);  // 1 秒超时

    EXPECT_TRUE(pm.is_unlocked());

    // 等待超过超时时间
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // 应该自动锁定
    EXPECT_FALSE(pm.is_unlocked());
}

//==============================================================================
// 边界条件测试
//==============================================================================

TEST(PasswordBoundary, SetPasswordExactly8Chars) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_TRUE(pm.set_master_password("12345678"));
    EXPECT_TRUE(pm.has_master_password());
}

TEST(PasswordBoundary, SetPassword7Chars) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_FALSE(pm.set_master_password("1234567"));
}

TEST(PasswordBoundary, SetPasswordVeryLong) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    std::string very_long(2000, 'A');
    very_long += "1!";  // 确保符合强度要求

    EXPECT_TRUE(pm.set_master_password(very_long));
    EXPECT_TRUE(pm.verify_master_password(very_long));
}

TEST(PasswordBoundary, VerifyWithoutSetPassword) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_FALSE(pm.verify_master_password("anything"));
}

TEST(PasswordBoundary, LockWithoutPassword) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_TRUE(pm.lock_configs());
    EXPECT_FALSE(pm.is_unlocked());
}

TEST(PasswordBoundary, UnlockWithoutPassword) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_FALSE(pm.unlock_configs("anypassword"));
}

TEST(PasswordBoundary, MultipleLockUnlock) {
    auto home = unique_temp_dir("falcon_pw_boundary_");
    ScopedEnvVar scoped_home("HOME", home.string());

    falcon::PasswordManager pm;
    EXPECT_TRUE(pm.set_master_password("GoodPass1!"));

    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(pm.lock_configs());
        EXPECT_FALSE(pm.is_unlocked());
        EXPECT_TRUE(pm.unlock_configs("GoodPass1!"));
        EXPECT_TRUE(pm.is_unlocked());
    }
}

//==============================================================================
// 特殊字符测试
//==============================================================================

TEST(PasswordSpecial, UnicodeCharacters) {
    falcon::PasswordManager pm;

    // Unicode 字符密码
    std::string unicode_pass = "Password123!中文";
    int strength = pm.check_password_strength(unicode_pass);
    EXPECT_GT(strength, 0);
}

TEST(PasswordSpecial, AllSpecialCharacters) {
    falcon::PasswordManager pm;

    std::string special_only = "!@#$%^&*()_+-=[]{}|;:,.<>?";
    int strength = pm.check_password_strength(special_only);
    EXPECT_GT(strength, 0);
}

TEST(PasswordSpecial, SpacesInPassword) {
    falcon::PasswordManager pm;

    std::string with_spaces = "Pass word 123!";
    int strength = pm.check_password_strength(with_spaces);
    EXPECT_GE(strength, 50);
}

//==============================================================================
// 并发测试
//==============================================================================

TEST(PasswordConcurrency, MultipleManagers) {
    auto home = unique_temp_dir("falcon_pw_concur_");
    ScopedEnvVar scoped_home("HOME", home.string());

    {
        falcon::PasswordManager pm1;
        falcon::PasswordManager pm2;

        EXPECT_TRUE(pm1.set_master_password("GoodPass1!"));
        EXPECT_TRUE(pm1.has_master_password());

        // pm2 应该能看到相同的密码
        EXPECT_TRUE(pm2.has_master_password());
        EXPECT_TRUE(pm2.verify_master_password("GoodPass1!"));
    }
}

//==============================================================================
// 密码持久化测试
//==============================================================================

TEST(PasswordPersistence, LoadExistingHash) {
    auto home = unique_temp_dir("falcon_pw_load_");
    ScopedEnvVar scoped_home("HOME", home.string());

    // 创建密码管理器并设置密码
    {
        falcon::PasswordManager pm;
        EXPECT_TRUE(pm.set_master_password("PersistPass1!"));
    }

    // 新实例应该能加载密码
    falcon::PasswordManager pm2;
    EXPECT_TRUE(pm2.has_master_password());
    EXPECT_TRUE(pm2.verify_master_password("PersistPass1!"));
    EXPECT_FALSE(pm2.verify_master_password("WrongPass!"));
}

TEST(PasswordPersistence, ChangePassword) {
    auto home = unique_temp_dir("falcon_pw_change_");
    ScopedEnvVar scoped_home("HOME", home.string());

    {
        falcon::PasswordManager pm;
        EXPECT_TRUE(pm.set_master_password("OldPass123!"));
        EXPECT_TRUE(pm.verify_master_password("OldPass123!"));

        // 更改密码
        EXPECT_TRUE(pm.set_master_password("NewPass456!"));
        EXPECT_TRUE(pm.verify_master_password("NewPass456!"));
        EXPECT_FALSE(pm.verify_master_password("OldPass123!"));
    }

    // 新实例应该使用新密码
    falcon::PasswordManager pm2;
    EXPECT_TRUE(pm2.verify_master_password("NewPass456!"));
    EXPECT_FALSE(pm2.verify_master_password("OldPass123!"));
}

//==============================================================================
// 生成密码唯一性测试
//==============================================================================

TEST(PasswordGenerationUniqueness, DifferentPasswords) {
    falcon::PasswordManager pm;

    std::vector<std::string> passwords;
    for (int i = 0; i < 10; ++i) {
        passwords.push_back(pm.generate_password(16));
    }

    // 所有生成的密码应该不同（极小概率重复）
    std::set<std::string> unique(passwords.begin(), passwords.end());
    EXPECT_EQ(unique.size(), 10u);
}

TEST(PasswordGenerationUniqueness, UniqueWithDifferentParams) {
    falcon::PasswordManager pm;

    auto p1 = pm.generate_password(16, true, true);
    auto p2 = pm.generate_password(16, false, false);
    auto p3 = pm.generate_password(16, false, true);

    // 参数不同，密码应该不同
    EXPECT_NE(p1, p2);
    EXPECT_NE(p2, p3);
}

//==============================================================================
// 主函数
//==============================================================================
