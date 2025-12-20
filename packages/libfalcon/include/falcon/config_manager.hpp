/**
 * @file config_manager.hpp
 * @brief 配置管理器（支持SQLite存储和加密）
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <string>
#include <map>
#include <memory>
#include <vector>

namespace falcon {

/**
 * @brief 云存储配置信息
 */
struct CloudStorageConfig {
    std::string name;              // 配置名称
    std::string provider;          // 提供商：oss, cos, kodo, upyun, s3
    std::string access_key;        // 访问密钥（加密存储）
    std::string secret_key;        // 密钥（加密存储）
    std::string region;           // 区域
    std::string bucket;           // 存储桶名称
    std::string endpoint;         // 自定义endpoint
    std::string custom_domain;    // 自定义域名
    std::map<std::string, std::string> extra;  // 额外配置
    time_t created_at;            // 创建时间
    time_t updated_at;            // 更新时间
};

/**
 * @brief 配置管理器
 */
class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    /**
     * @brief 初始化配置管理器
     * @param db_path 数据库文件路径
     * @param master_password 主密码
     * @return 是否成功
     */
    bool initialize(const std::string& db_path, const std::string& master_password = "");

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
     * @brief 保存云存储配置
     * @param config 配置信息
     * @return 是否成功
     */
    bool save_cloud_config(const CloudStorageConfig& config);

    /**
     * @brief 获取云存储配置
     * @param name 配置名称
     * @param config 输出配置
     * @return 是否成功
     */
    bool get_cloud_config(const std::string& name, CloudStorageConfig& config);

    /**
     * @brief 删除云存储配置
     * @param name 配置名称
     * @return 是否成功
     */
    bool delete_cloud_config(const std::string& name);

    /**
     * @brief 列出所有云存储配置名称
     * @return 配置名称列表
     */
    std::vector<std::string> list_cloud_configs();

    /**
     * @brief 搜索配置
     * @param provider 提供商过滤（可选）
     * @return 匹配的配置列表
     */
    std::vector<CloudStorageConfig> search_configs(const std::string& provider = "");

    /**
     * @brief 更新配置
     * @param name 配置名称
     * @param config 新配置
     * @return 是否成功
     */
    bool update_cloud_config(const std::string& name, const CloudStorageConfig& config);

    /**
     * @brief 导出配置（加密）
     * @param export_path 导出文件路径
     * @param export_password 导出密码
     * @return 是否成功
     */
    bool export_configs(const std::string& export_path, const std::string& export_password);

    /**
     * @brief 导入配置（加密）
     * @param import_path 导入文件路径
     * @param import_password 导入密码
     * @return 是否成功
     */
    bool import_configs(const std::string& import_path, const std::string& import_password);

private:
    class Impl;
    std::unique_ptr<Impl> p_impl_;
};

} // namespace falcon