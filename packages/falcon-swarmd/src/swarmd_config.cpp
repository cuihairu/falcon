// ============================================================================
// swarmd_config 实现（见 swarmd_config.hpp 头注释；形制母本 daemon/config.cpp）
// ============================================================================

#include "swarmd_config.hpp"

#include "daemon/daemon.hpp"  // get_default_config_dir（falcon_daemon_core）

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <utility>
#include <vector>

namespace falcon::swarm {

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

}  // namespace

std::string expand_home_path(const std::string& path) {
    if (path.rfind("~/", 0) != 0) return path;
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
    char buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableA("USERPROFILE", buf, MAX_PATH);
    const std::string home_str = n > 0 ? std::string(buf, n) : std::string();
#else
    const char* home = std::getenv("HOME");
    const std::string home_str = home ? home : std::string();
#endif
    if (home_str.empty()) return path;
    return home_str + path.substr(1);  // 保留 "~/" 之后的 "/..."
}

std::string get_default_config_file() {
    return ::falcon::daemon::get_default_config_dir() + "/swarm.json";
}

ConfigLoadResult apply_config_file(const std::string& path,
                                   SwarmdFileConfig& config) {
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
    const std::string known_sections[] = {"swarm", "daemon"};

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

    if (root.contains("swarm")) {
        const auto& swarm = root.at("swarm");
        if (!swarm.is_object()) {
            result.error = "'swarm' section must be an object";
            return result;
        }
        warn_unknown_keys(swarm,
                          {"host", "port", "server_token", "group_token",
                           "heartbeat_interval_s", "heartbeat_timeout_s",
                           "challenge_ttl_s", "sweep_interval_ms",
                           "rate_register_per_min", "rate_query_per_min",
                           "blacklist"},
                          "swarm", result.warnings);
        if (!read_key(swarm, "host", config.host, result.error)) return result;
        if (!read_key(swarm, "port", config.port, result.error)) return result;
        if (!read_key(swarm, "server_token", config.server_token, result.error)) {
            return result;
        }
        if (!read_key(swarm, "group_token", config.group_token, result.error)) {
            return result;
        }
        if (!read_key(swarm, "heartbeat_interval_s", config.heartbeat_interval_s,
                      result.error)) {
            return result;
        }
        if (!read_key(swarm, "heartbeat_timeout_s", config.heartbeat_timeout_s,
                      result.error)) {
            return result;
        }
        if (!read_key(swarm, "challenge_ttl_s", config.challenge_ttl_s,
                      result.error)) {
            return result;
        }
        if (!read_key(swarm, "sweep_interval_ms", config.sweep_interval_ms,
                      result.error)) {
            return result;
        }
        if (!read_key(swarm, "rate_register_per_min", config.rate_register_per_min,
                      result.error)) {
            return result;
        }
        if (!read_key(swarm, "rate_query_per_min", config.rate_query_per_min,
                      result.error)) {
            return result;
        }
        if (!read_key(swarm, "blacklist", config.blacklist, result.error)) {
            return result;
        }
    }

    if (root.contains("daemon")) {
        const auto& daemon = root.at("daemon");
        if (!daemon.is_object()) {
            result.error = "'daemon' section must be an object";
            return result;
        }
        warn_unknown_keys(daemon,
                          {"run_as_daemon", "pid_file", "working_dir",
                           "log_file"},
                          "daemon", result.warnings);
        if (!read_key(daemon, "run_as_daemon", config.run_as_daemon,
                      result.error)) {
            return result;
        }
        // pid_file 显式给出即创建（与 --pid-file 行为一致，main 装配）
        if (!read_path_key(daemon, "pid_file", config.pid_file, result.error)) {
            return result;
        }
        if (!read_path_key(daemon, "working_dir", config.working_dir,
                           result.error)) {
            return result;
        }
        if (!read_path_key(daemon, "log_file", config.log_file, result.error)) {
            return result;
        }
    }

    result.ok = true;
    return result;
}

}  // namespace falcon::swarm
