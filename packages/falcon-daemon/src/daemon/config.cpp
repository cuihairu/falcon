/**
 * @file config.cpp
 * @brief daemon.json 配置文件加载实现
 * @author Falcon Team
 * @date 2026-09-12
 */

#include "daemon/config.hpp"

#include "rpc/json_rpc_server.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <vector>

namespace falcon::daemon {

namespace {

namespace fs = std::filesystem;

/// 读取整段对象节点里的键值。键不存在时不动目标，类型不符时报错。
template <typename T>
bool read_key(const nlohmann::json& section, const char* key, T& target,
              std::string& error) {
    if (!section.contains(key)) return true;
    try {
        target = section.at(key).get<T>();
    } catch (const std::exception&) {
        error = std::string("invalid type for key '") + key + "'";
        return false;
    }
    return true;
}

/// 路径型键：读取后展开 "~/" 前缀
bool read_path_key(const nlohmann::json& section, const char* key,
                   std::string& target, std::string& error) {
    if (!read_key(section, key, target, error)) return false;
    target = expand_home_path(target);
    return true;
}

/// 节内未知键收集为告警（拼错键名不静默）
void warn_unknown_keys(const nlohmann::json& section,
                       std::initializer_list<const char*> known_keys,
                       const std::string& section_name,
                       std::vector<std::string>& warnings) {
    for (auto it = section.begin(); it != section.end(); ++it) {
        bool known = false;
        for (const char* key : known_keys) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) {
            warnings.push_back("unknown key in '" + section_name +
                               "' section: " + it.key());
        }
    }
}

} // namespace

std::string expand_home_path(const std::string& path) {
    if (path.rfind("~/", 0) != 0) return path;
#ifdef _WIN32
    char buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
    const std::string home = n > 0 ? std::string(buf, n) : std::string();
#else
    const char* home = std::getenv("HOME");
    const std::string home_str = home ? home : std::string();
#endif
    if (home_str.empty()) return path;
    return home_str + path.substr(1);  // 保留 "~/" 之后的 "/..."
}

std::string get_default_config_file() {
    return get_default_config_dir() + "/daemon.json";
}

ConfigLoadResult apply_config_file(const std::string& path,
                                   falcon::daemon::rpc::JsonRpcServerConfig& rpc_config,
                                   DaemonConfig& daemon_config,
                                   std::string& task_db_path,
                                   bool& enable_rpc,
                                   bool& run_as_daemon,
                                   DownloadConfig& download_config) {
    ConfigLoadResult result;

    std::ifstream in(path);
    if (!in) {
        result.error = "cannot open config file: " + path;
        return result;
    }

    nlohmann::json root;
    try {
        in >> root;
    } catch (const std::exception& e) {
        result.error = std::string("failed to parse config file: ") + e.what();
        return result;
    }
    if (!root.is_object()) {
        result.error = "config file root must be a JSON object";
        return result;
    }

    // 允许的节与各节键集合（未知键告警不失败——向前兼容）
    const std::string known_sections[] = {"rpc", "daemon", "storage", "download"};

    for (auto it = root.begin(); it != root.end(); ++it) {
        bool known = false;
        for (const auto& section : known_sections) {
            if (it.key() == section) {
                known = true;
                break;
            }
        }
        if (!known) {
            result.warnings.push_back("unknown config section: " + it.key());
        }
    }

    if (root.contains("rpc")) {
        const auto& rpc = root.at("rpc");
        if (!rpc.is_object()) {
            result.error = "'rpc' section must be an object";
            return result;
        }
        warn_unknown_keys(rpc, {"enabled", "host", "port", "secret",
                                "allow_origin_all"},
                          "rpc", result.warnings);
        if (!read_key(rpc, "enabled", enable_rpc, result.error)) return result;
        if (!read_key(rpc, "host", rpc_config.bind_address, result.error)) return result;
        if (!read_key(rpc, "port", rpc_config.listen_port, result.error)) return result;
        if (!read_key(rpc, "secret", rpc_config.secret, result.error)) return result;
        if (!read_key(rpc, "allow_origin_all", rpc_config.allow_origin_all, result.error)) {
            return result;
        }
    }

    if (root.contains("daemon")) {
        const auto& daemon = root.at("daemon");
        if (!daemon.is_object()) {
            result.error = "'daemon' section must be an object";
            return result;
        }
        warn_unknown_keys(daemon, {"run_as_daemon", "pid_file", "working_dir",
                                   "log_file"},
                          "daemon", result.warnings);
        if (!read_key(daemon, "run_as_daemon", run_as_daemon, result.error)) return result;
        // pid_file 显式给出即创建（与 --pid-file 行为一致）
        if (!read_path_key(daemon, "pid_file", daemon_config.pid_file, result.error)) {
            return result;
        }
        if (!daemon_config.pid_file.empty()) {
            daemon_config.create_pid_file = true;
        }
        if (!read_path_key(daemon, "working_dir", daemon_config.working_dir, result.error)) {
            return result;
        }
        if (!read_path_key(daemon, "log_file", daemon_config.log_file, result.error)) {
            return result;
        }
    }

    if (root.contains("storage")) {
        const auto& storage = root.at("storage");
        if (!storage.is_object()) {
            result.error = "'storage' section must be an object";
            return result;
        }
        warn_unknown_keys(storage, {"task_db_path"}, "storage", result.warnings);
        if (!read_path_key(storage, "task_db_path", task_db_path, result.error)) {
            return result;
        }
    }

    if (root.contains("download")) {
        const auto& download = root.at("download");
        if (!download.is_object()) {
            result.error = "'download' section must be an object";
            return result;
        }
        warn_unknown_keys(download, {"max_concurrent_tasks",
                                     "max_overall_speed_limit"},
                          "download", result.warnings);
        // optional 语义：键出现才覆盖，未出现保持引擎默认
        if (download.contains("max_concurrent_tasks")) {
            std::size_t value = 0;
            if (!read_key(download, "max_concurrent_tasks", value, result.error)) {
                return result;
            }
            download_config.max_concurrent_tasks = value;
        }
        if (download.contains("max_overall_speed_limit")) {
            std::uint64_t value = 0;
            if (!read_key(download, "max_overall_speed_limit", value, result.error)) {
                return result;
            }
            download_config.max_overall_speed_limit = value;
        }
    }

    result.ok = true;
    return result;
}

} // namespace falcon::daemon
