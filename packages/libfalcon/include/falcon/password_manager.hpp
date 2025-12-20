/**
 * @file password_manager.hpp
 * @brief 密码管理器（本地密码保护机制）
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <string>
#include <functional>
#include <memory>

namespace falcon {

/**
 * @brief 密码管理器
 */
class PasswordManager {
public:
    PasswordManager();
    ~PasswordManager();

    /**
     * @brief 设置主密码
     * @param password 主密码
     * @return 是否成功
     */
    bool set_master_password(const std::string& password);

    /**
     * @brief 验证主密码
     * @param password 主密码
     * @return 是否正确
     */
    bool verify_master_password(const std::string& password);

    /**
     * @brief 检查是否已设置主密码
     * @return 是否已设置
     */
    bool has_master_password() const;

    /**
     * @brief 锁定配置（需要密码才能访问）
     * @return 是否成功
     */
    bool lock_configs();

    /**
     * @brief 解锁配置
     * @param password 主密码
     * @return 是否成功
     */
    bool unlock_configs(const std::string& password);

    /**
     * @brief 检查是否已解锁
     * @return 是否已解锁
     */
    bool is_unlocked() const;

    /**
     * @brief 设置密码超时时间（秒）
     * @param seconds 超时时间，0表示永不过时
     */
    void set_timeout(int seconds);

    /**
     * @brief 获取密码输入回调
     * @return 输入回调函数
     */
    std::function<std::string()> get_password_callback() const;

    /**
     * @brief 设置密码输入回调
     * @param callback 回调函数
     */
    void set_password_callback(std::function<std::string()> callback);

    /**
     * @brief 请求密码输入
     * @param prompt 提示信息
     * @return 输入的密码
     */
    std::string prompt_password(const std::string& prompt = "请输入密码: ");

    /**
     * @brief 确认密码
     * @param password 密码
     * @param prompt 提示信息
     * @return 是否确认
     */
    bool confirm_password(const std::string& password,
                          const std::string& prompt = "请再次输入密码: ");

    /**
     * @brief 检查密码强度
     * @param password 密码
     * @return 密码强度评分（0-100）
     */
    int check_password_strength(const std::string& password);

    /**
     * @brief 生成密码
     * @param length 密码长度
     * @param use_symbols 是否使用特殊字符
     * @param use_numbers 是否使用数字
     * @return 生成的密码
     */
    std::string generate_password(int length = 16,
                                 bool use_symbols = true,
                                 bool use_numbers = true);

private:
    class Impl;
    std::unique_ptr<Impl> p_impl_;
};

} // namespace falcon