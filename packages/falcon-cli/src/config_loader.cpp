/**
 * @file config_loader.cpp
 * @brief CLI 配置文件加载器实现
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "config_loader.hpp"
#include <falcon/logger.hpp>

#ifdef FALCON_USE_JSON
#include <nlohmann/json.hpp>
#endif

#include <fstream>
#include <sstream>
#include <filesystem>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace falcon::cli {

std::string ConfigLoader::last_error_;

// ============================================================================
// CliConfig 实现
// ============================================================================

DownloadOptions CliConfig::to_download_options() const {
    DownloadOptions opts;

    opts.max_connections = static_cast<size_t>(max_connections);
    opts.timeout_seconds = static_cast<size_t>(timeout_seconds);
    opts.max_retries = static_cast<size_t>(max_retries);
    opts.retry_delay_seconds = static_cast<size_t>(retry_delay_seconds);
    opts.output_directory = default_download_dir;
    opts.output_filename = default_output_filename;
    opts.speed_limit = speed_limit;
    opts.resume_enabled = resume_enabled;
    opts.user_agent = user_agent;
    opts.proxy = proxy;
    opts.proxy_type = proxy_type;
    opts.proxy_username = proxy_username;
    opts.proxy_password = proxy_password;
    opts.verify_ssl = verify_ssl;
    opts.referer = referer;
    opts.cookie_file = cookie_file;
    opts.cookie_jar = cookie_jar;
    opts.http_username = http_username;
    opts.http_password = http_password;
    opts.min_segment_size = min_segment_size;
    opts.adaptive_segment_sizing = adaptive_segment_sizing;
    opts.create_directory = create_directory;
    opts.overwrite_existing = overwrite_existing;

    // Copy headers
    for (const auto& [k, v] : headers) {
        opts.headers[k] = v;
    }

    return opts;
}

// ============================================================================
// ConfigLoader 实现
// ============================================================================

std::string ConfigLoader::get_config_dir() {
#ifdef _WIN32
    char path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::string(path) + "\\falcon";
    }
    // Fallback to environment variable
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::string(appdata) + "\\falcon";
    }
    return "C:\\ProgramData\\falcon";
#else
    const char* home = std::getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) {
            home = pw->pw_dir;
        }
    }
    if (home) {
        return std::string(home) + "/.config/falcon";
    }
    return "/etc/falcon";
#endif
}

std::string ConfigLoader::get_default_config_path() {
    return get_config_dir() +
#ifdef _WIN32
           "\\"
#else
           "/"
#endif
           "config.json";
}

std::vector<std::string> ConfigLoader::get_config_search_paths() {
    std::vector<std::string> paths;

    // 1. 当前目录
    paths.push_back(
#ifdef _WIN32
        ".\\falcon.json"
#else
        "./falcon.json"
#endif
    );

    // 2. 用户配置目录
    paths.push_back(get_default_config_path());

    // 3. 系统配置目录
#ifdef _WIN32
    const char* programdata = std::getenv("PROGRAMDATA");
    if (programdata) {
        paths.push_back(std::string(programdata) + "\\falcon\\config.json");
    }
#else
    paths.push_back("/etc/falcon/config.json");
#endif

    return paths;
}

std::optional<CliConfig> ConfigLoader::load(const std::string& config_path) {
    // 如果指定了路径，直接加载
    if (!config_path.empty()) {
        auto config = load_from_file(config_path);
        if (config.has_value()) {
            return config;
        }
        // 指定了路径但加载失败，返回空
        return std::nullopt;
    }

    // 搜索配置文件
    for (const auto& path : get_config_search_paths()) {
        auto config = load_from_file(path);
        if (config.has_value()) {
            FALCON_LOG_DEBUG_STREAM("Loaded config from: " << path);
            return config;
        }
    }

    // 尝试从环境变量加载
    return load_from_env();
}

CliConfig ConfigLoader::load_or_default(const std::string& config_path) {
    auto config = load(config_path);
    return config.value_or(CliConfig{});
}

bool ConfigLoader::save(const CliConfig& config, const std::string& config_path) {
    std::string path = config_path.empty() ? get_default_config_path() : config_path;

    // 确保目录存在
    if (!create_config_directories(path)) {
        last_error_ = "Failed to create config directory";
        return false;
    }

#ifdef FALCON_USE_JSON
    try {
        nlohmann::json j;

        // 下载选项
        j["max_connections"] = config.max_connections;
        j["max_concurrent_downloads"] = config.max_concurrent_downloads;
        j["timeout_seconds"] = config.timeout_seconds;
        j["max_retries"] = config.max_retries;
        j["retry_delay_seconds"] = config.retry_delay_seconds;
        if (!config.default_download_dir.empty()) {
            j["default_download_dir"] = config.default_download_dir;
        }
        if (!config.default_output_filename.empty()) {
            j["default_output_filename"] = config.default_output_filename;
        }
        if (config.speed_limit > 0) {
            j["speed_limit"] = config.speed_limit;
        }
        j["min_segment_size"] = config.min_segment_size;
        j["resume_enabled"] = config.resume_enabled;
        j["adaptive_segment_sizing"] = config.adaptive_segment_sizing;
        j["verify_ssl"] = config.verify_ssl;

        // 网络选项
        j["user_agent"] = config.user_agent;
        if (!config.proxy.empty()) {
            j["proxy"] = config.proxy;
        }
        if (!config.proxy_type.empty()) {
            j["proxy_type"] = config.proxy_type;
        }
        if (!config.proxy_username.empty()) {
            j["proxy_username"] = config.proxy_username;
        }
        if (!config.proxy_password.empty()) {
            j["proxy_password"] = config.proxy_password;
        }
        if (!config.referer.empty()) {
            j["referer"] = config.referer;
        }
        if (!config.cookie_file.empty()) {
            j["cookie_file"] = config.cookie_file;
        }
        if (!config.cookie_jar.empty()) {
            j["cookie_jar"] = config.cookie_jar;
        }
        if (!config.http_username.empty()) {
            j["http_username"] = config.http_username;
        }
        if (!config.http_password.empty()) {
            j["http_password"] = config.http_password;
        }
        if (!config.headers.empty()) {
            j["headers"] = config.headers;
        }

        // RPC 选项
        j["enable_rpc"] = config.enable_rpc;
        if (config.enable_rpc) {
            j["rpc_listen_port"] = config.rpc_listen_port;
            if (!config.rpc_secret.empty()) {
                j["rpc_secret"] = config.rpc_secret;
            }
            j["rpc_allow_origin_all"] = config.rpc_allow_origin_all;
        }

        // 输出选项
        j["verbose"] = config.verbose;
        j["quiet"] = config.quiet;
        j["log_level"] = config.log_level;
        j["show_progress"] = config.show_progress;

        // 高级选项
        j["use_head"] = config.use_head;
        j["conditional_download"] = config.conditional_download;
        j["auto_renaming"] = config.auto_renaming;
        j["create_directory"] = config.create_directory;
        j["overwrite_existing"] = config.overwrite_existing;

        std::ofstream file(path);
        if (!file.is_open()) {
            last_error_ = "Failed to open file for writing: " + path;
            return false;
        }

        file << j.dump(4) << std::endl;
        FALCON_LOG_INFO_STREAM("Config saved to: " << path);
        return true;
    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to serialize config: ") + e.what();
        FALCON_LOG_ERROR_STREAM(last_error_);
        return false;
    }
#else
    last_error_ = "JSON support not available";
    return false;
#endif
}

bool ConfigLoader::create_default_config(const std::string& config_path) {
    CliConfig default_config;

    // 设置一些合理的默认值
#ifdef _WIN32
    char downloads[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, downloads))) {
        default_config.default_download_dir = std::string(downloads) + "\\Downloads";
    }
#else
    const char* home = std::getenv("HOME");
    if (home) {
        default_config.default_download_dir = std::string(home) + "/Downloads";
    }
#endif

    return save(default_config, config_path);
}

std::vector<std::string> ConfigLoader::validate(const CliConfig& config) {
    std::vector<std::string> errors;

    if (config.max_connections < 1 || config.max_connections > 64) {
        errors.push_back("max_connections must be between 1 and 64");
    }

    if (config.max_concurrent_downloads < 1 || config.max_concurrent_downloads > 64) {
        errors.push_back("max_concurrent_downloads must be between 1 and 64");
    }

    if (config.timeout_seconds < 0) {
        errors.push_back("timeout_seconds must be non-negative");
    }

    if (config.max_retries < 0 || config.max_retries > 100) {
        errors.push_back("max_retries must be between 0 and 100");
    }

    if (config.rpc_listen_port < 1 || config.rpc_listen_port > 65535) {
        errors.push_back("rpc_listen_port must be between 1 and 65535");
    }

    if (!config.proxy.empty() && config.proxy_type.empty()) {
        // Auto-detect proxy type
    }

    return errors;
}

std::string ConfigLoader::get_last_error() {
    return last_error_;
}

std::optional<CliConfig> ConfigLoader::load_from_file(const std::string& path) {
#ifdef FALCON_USE_JSON
    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return std::nullopt;
        }

        nlohmann::json j;
        file >> j;
        file.close();

        CliConfig config;

        // 下载选项
        if (j.contains("max_connections")) {
            config.max_connections = j["max_connections"].get<int>();
        }
        if (j.contains("max_concurrent_downloads")) {
            config.max_concurrent_downloads = j["max_concurrent_downloads"].get<int>();
        }
        if (j.contains("timeout_seconds")) {
            config.timeout_seconds = j["timeout_seconds"].get<int>();
        }
        if (j.contains("max_retries")) {
            config.max_retries = j["max_retries"].get<int>();
        }
        if (j.contains("retry_delay_seconds")) {
            config.retry_delay_seconds = j["retry_delay_seconds"].get<int>();
        }
        if (j.contains("default_download_dir")) {
            config.default_download_dir = j["default_download_dir"].get<std::string>();
        }
        if (j.contains("default_output_filename")) {
            config.default_output_filename = j["default_output_filename"].get<std::string>();
        }
        if (j.contains("speed_limit")) {
            config.speed_limit = j["speed_limit"].get<std::size_t>();
        }
        if (j.contains("min_segment_size")) {
            config.min_segment_size = j["min_segment_size"].get<std::size_t>();
        }
        if (j.contains("resume_enabled")) {
            config.resume_enabled = j["resume_enabled"].get<bool>();
        }
        if (j.contains("adaptive_segment_sizing")) {
            config.adaptive_segment_sizing = j["adaptive_segment_sizing"].get<bool>();
        }
        if (j.contains("verify_ssl")) {
            config.verify_ssl = j["verify_ssl"].get<bool>();
        }

        // 网络选项
        if (j.contains("user_agent")) {
            config.user_agent = j["user_agent"].get<std::string>();
        }
        if (j.contains("proxy")) {
            config.proxy = j["proxy"].get<std::string>();
        }
        if (j.contains("proxy_type")) {
            config.proxy_type = j["proxy_type"].get<std::string>();
        }
        if (j.contains("proxy_username")) {
            config.proxy_username = j["proxy_username"].get<std::string>();
        }
        if (j.contains("proxy_password")) {
            config.proxy_password = j["proxy_password"].get<std::string>();
        }
        if (j.contains("referer")) {
            config.referer = j["referer"].get<std::string>();
        }
        if (j.contains("cookie_file")) {
            config.cookie_file = j["cookie_file"].get<std::string>();
        }
        if (j.contains("cookie_jar")) {
            config.cookie_jar = j["cookie_jar"].get<std::string>();
        }
        if (j.contains("http_username")) {
            config.http_username = j["http_username"].get<std::string>();
        }
        if (j.contains("http_password")) {
            config.http_password = j["http_password"].get<std::string>();
        }
        if (j.contains("headers")) {
            for (auto& [k, v] : j["headers"].items()) {
                config.headers[k] = v.get<std::string>();
            }
        }

        // RPC 选项
        if (j.contains("enable_rpc")) {
            config.enable_rpc = j["enable_rpc"].get<bool>();
        }
        if (j.contains("rpc_listen_port")) {
            config.rpc_listen_port = j["rpc_listen_port"].get<int>();
        }
        if (j.contains("rpc_secret")) {
            config.rpc_secret = j["rpc_secret"].get<std::string>();
        }
        if (j.contains("rpc_allow_origin_all")) {
            config.rpc_allow_origin_all = j["rpc_allow_origin_all"].get<bool>();
        }

        // 输出选项
        if (j.contains("verbose")) {
            config.verbose = j["verbose"].get<bool>();
        }
        if (j.contains("quiet")) {
            config.quiet = j["quiet"].get<bool>();
        }
        if (j.contains("log_level")) {
            config.log_level = j["log_level"].get<std::string>();
        }
        if (j.contains("show_progress")) {
            config.show_progress = j["show_progress"].get<bool>();
        }

        // 高级选项
        if (j.contains("use_head")) {
            config.use_head = j["use_head"].get<bool>();
        }
        if (j.contains("conditional_download")) {
            config.conditional_download = j["conditional_download"].get<bool>();
        }
        if (j.contains("auto_renaming")) {
            config.auto_renaming = j["auto_renaming"].get<bool>();
        }
        if (j.contains("create_directory")) {
            config.create_directory = j["create_directory"].get<bool>();
        }
        if (j.contains("overwrite_existing")) {
            config.overwrite_existing = j["overwrite_existing"].get<bool>();
        }

        return config;
    } catch (const std::exception& e) {
        last_error_ = std::string("Failed to parse config file: ") + e.what();
        FALCON_LOG_ERROR_STREAM(last_error_);
        return std::nullopt;
    }
#else
    last_error_ = "JSON support not available";
    return std::nullopt;
#endif
}

std::optional<CliConfig> ConfigLoader::load_from_env() {
    CliConfig config;
    bool has_env_config = false;

    // 从环境变量读取配置
    const char* env_val = nullptr;

    if ((env_val = std::getenv("FALCON_MAX_CONNECTIONS"))) {
        config.max_connections = std::stoi(env_val);
        has_env_config = true;
    }

    if ((env_val = std::getenv("FALCON_PROXY"))) {
        config.proxy = env_val;
        has_env_config = true;
    }

    if ((env_val = std::getenv("FALCON_USER_AGENT"))) {
        config.user_agent = env_val;
        has_env_config = true;
    }

    if ((env_val = std::getenv("FALCON_DOWNLOAD_DIR"))) {
        config.default_download_dir = env_val;
        has_env_config = true;
    }

    if ((env_val = std::getenv("FALCON_RPC_SECRET"))) {
        config.rpc_secret = env_val;
        has_env_config = true;
    }

    if ((env_val = std::getenv("FALCON_LOG_LEVEL"))) {
        config.log_level = env_val;
        has_env_config = true;
    }

    return has_env_config ? std::optional<CliConfig>(config) : std::nullopt;
}

bool ConfigLoader::create_config_directories(const std::string& path) {
    try {
        std::filesystem::path p(path);
        std::filesystem::path dir = p.parent_path();
        if (!dir.empty() && !std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }
        return true;
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR_STREAM("Failed to create directories: " << e.what());
        return false;
    }
}

// ============================================================================
// merge_configs 实现
// ============================================================================

CliConfig merge_configs(const CliConfig& file_config, const CliConfig& cli_args) {
    CliConfig merged = file_config;

    // 命令行参数优先
    if (cli_args.max_connections != 4) {  // 4 是默认值
        merged.max_connections = cli_args.max_connections;
    }
    if (cli_args.max_concurrent_downloads != 1) {
        merged.max_concurrent_downloads = cli_args.max_concurrent_downloads;
    }
    if (cli_args.timeout_seconds != 30) {
        merged.timeout_seconds = cli_args.timeout_seconds;
    }
    if (cli_args.max_retries != 3) {
        merged.max_retries = cli_args.max_retries;
    }
    if (!cli_args.default_download_dir.empty()) {
        merged.default_download_dir = cli_args.default_download_dir;
    }
    if (!cli_args.default_output_filename.empty()) {
        merged.default_output_filename = cli_args.default_output_filename;
    }
    if (cli_args.speed_limit != 0) {
        merged.speed_limit = cli_args.speed_limit;
    }
    if (!cli_args.user_agent.empty() && cli_args.user_agent != "Falcon/0.2.0") {
        merged.user_agent = cli_args.user_agent;
    }
    if (!cli_args.proxy.empty()) {
        merged.proxy = cli_args.proxy;
    }
    if (!cli_args.proxy_type.empty()) {
        merged.proxy_type = cli_args.proxy_type;
    }
    if (!cli_args.proxy_username.empty()) {
        merged.proxy_username = cli_args.proxy_username;
    }
    if (!cli_args.proxy_password.empty()) {
        merged.proxy_password = cli_args.proxy_password;
    }
    if (!cli_args.referer.empty()) {
        merged.referer = cli_args.referer;
    }
    if (!cli_args.cookie_file.empty()) {
        merged.cookie_file = cli_args.cookie_file;
    }
    if (!cli_args.cookie_jar.empty()) {
        merged.cookie_jar = cli_args.cookie_jar;
    }
    if (!cli_args.http_username.empty()) {
        merged.http_username = cli_args.http_username;
    }
    if (!cli_args.http_password.empty()) {
        merged.http_password = cli_args.http_password;
    }
    if (!cli_args.headers.empty()) {
        // 合并 headers
        for (const auto& [k, v] : cli_args.headers) {
            merged.headers[k] = v;
        }
    }
    if (!cli_args.rpc_secret.empty()) {
        merged.rpc_secret = cli_args.rpc_secret;
    }
    if (cli_args.rpc_listen_port != 6800) {
        merged.rpc_listen_port = cli_args.rpc_listen_port;
    }

    // 布尔值：命令行设置则使用
    if (cli_args.verbose) {
        merged.verbose = true;
    }
    if (cli_args.quiet) {
        merged.quiet = true;
        merged.show_progress = false;
    }
    if (!cli_args.resume_enabled) {
        merged.resume_enabled = false;
    }
    if (!cli_args.verify_ssl) {
        merged.verify_ssl = false;
    }
    if (cli_args.enable_rpc) {
        merged.enable_rpc = true;
    }
    if (cli_args.rpc_allow_origin_all) {
        merged.rpc_allow_origin_all = true;
    }

    return merged;
}

} // namespace falcon::cli
