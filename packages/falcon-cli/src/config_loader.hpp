/**
 * @file config_loader.hpp
 * @brief CLI 配置文件加载器
 * @author Falcon Team
 * @date 2025-12-28
 *
 * 支持从配置文件加载下载选项，并与命令行参数合并。
 * 配置文件路径（按优先级）：
 * 1. 命令行 --config 指定路径
 * 2. 当前目录 ./falcon.json
 * 3. ~/.config/falcon/config.json (Unix) / %APPDATA%/falcon/config.json (Windows)
 * 4. /etc/falcon/config.json (Unix) / %PROGRAMDATA%/falcon/config.json (Windows)
 */

#pragma once

#include <falcon/download_options.hpp>

#include <string>
#include <vector>
#include <optional>
#include <map>

namespace falcon::cli {

/**
 * @brief CLI 配置结构
 */
struct CliConfig {
    // 下载选项
    int max_connections = 4;
    int max_concurrent_downloads = 1;
    int timeout_seconds = 30;
    int max_retries = 3;
    int retry_delay_seconds = 5;
    std::string default_download_dir;
    std::string default_output_filename;
    std::size_t speed_limit = 0;
    std::size_t min_segment_size = 1024 * 1024;  // 1MB
    bool resume_enabled = true;
    bool adaptive_segment_sizing = true;
    bool verify_ssl = true;

    // 网络选项
    std::string user_agent = "Falcon/0.2.0";
    std::string proxy;
    std::string proxy_type;
    std::string proxy_username;
    std::string proxy_password;
    std::string referer;
    std::string cookie_file;
    std::string cookie_jar;
    std::string http_username;
    std::string http_password;
    std::map<std::string, std::string> headers;

    // RPC 选项
    bool enable_rpc = false;
    int rpc_listen_port = 6800;
    std::string rpc_secret;
    bool rpc_allow_origin_all = false;

    // 输出选项
    bool verbose = false;
    bool quiet = false;
    std::string log_level = "info";
    bool show_progress = true;

    // 高级选项
    bool use_head = false;
    bool conditional_download = false;
    bool auto_renaming = false;
    bool create_directory = true;
    bool overwrite_existing = false;

    /**
     * @brief 转换为 DownloadOptions
     */
    DownloadOptions to_download_options() const;
};

/**
 * @brief 配置加载器
 */
class ConfigLoader {
public:
    /**
     * @brief 加载配置文件
     * @param config_path 配置文件路径（可选，自动搜索）
     * @return 加载的配置，失败返回空
     */
    static std::optional<CliConfig> load(const std::string& config_path = "");

    /**
     * @brief 加载配置文件并返回 CliConfig
     * @param config_path 配置文件路径
     * @return 配置对象
     */
    static CliConfig load_or_default(const std::string& config_path = "");

    /**
     * @brief 保存配置到文件
     * @param config 配置对象
     * @param config_path 目标文件路径
     * @return true 成功
     */
    static bool save(const CliConfig& config, const std::string& config_path);

    /**
     * @brief 获取配置文件搜索路径列表
     * @return 路径列表（按优先级排序）
     */
    static std::vector<std::string> get_config_search_paths();

    /**
     * @brief 获取默认配置文件路径
     * @return 默认路径
     */
    static std::string get_default_config_path();

    /**
     * @brief 获取配置目录路径
     * @return 配置目录
     */
    static std::string get_config_dir();

    /**
     * @brief 创建默认配置文件
     * @param config_path 目标路径
     * @return true 成功
     */
    static bool create_default_config(const std::string& config_path = "");

    /**
     * @brief 验证配置
     * @param config 配置对象
     * @return 错误消息列表，空表示验证通过
     */
    static std::vector<std::string> validate(const CliConfig& config);

    /**
     * @brief 获取最后的错误消息
     * @return 错误消息
     */
    static std::string get_last_error();

private:
    static std::string last_error_;

    static std::optional<CliConfig> load_from_file(const std::string& path);
    static std::optional<CliConfig> load_from_env();
    static bool create_config_directories(const std::string& path);
};

/**
 * @brief 合并命令行参数和配置文件
 * @param file_config 配置文件加载的配置
 * @param cli_args 命令行参数
 * @return 合并后的配置
 *
 * 合并规则：命令行参数优先于配置文件
 */
CliConfig merge_configs(const CliConfig& file_config, const CliConfig& cli_args);

} // namespace falcon::cli
