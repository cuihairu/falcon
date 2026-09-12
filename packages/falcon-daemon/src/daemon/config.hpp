/**
 * @file config.hpp
 * @brief daemon.json 配置文件加载
 * @author Falcon Team
 * @date 2026-09-12
 *
 * 从 JSON 配置文件读取 daemon 配置并与命令行参数合并：
 * - 文件结构为 {"rpc": {...}, "daemon": {...}, "storage": {...}} 三节
 * - 只覆盖文件中出现的键，未出现的键保持调用方传入的值
 * - 优先级：命令行显式参数 > 配置文件 > 内置默认值
 *   （实现方式：先加载配置文件，再走既有的 argv 解析——CLI 分支只在
 *   参数显式给出时写入配置结构，天然覆盖文件值）
 */

#pragma once

#include "daemon/daemon.hpp"

#include <string>
#include <vector>

namespace falcon::daemon::rpc {
struct JsonRpcServerConfig;  // 前置声明：本头文件不拖入 RPC/引擎头
}

namespace falcon::daemon {

/// 配置文件加载结果
struct ConfigLoadResult {
    bool ok = false;                     ///< 是否加载成功
    std::string error;                   ///< ok=false 时的失败原因
    std::vector<std::string> warnings;   ///< 非致命提示（未知键等）
};

/**
 * @brief 解析 JSON 配置文件并应用到配置结构上
 *
 * 文件中的值只覆盖对应键存在时才写入；路径值支持 "~/" 前缀展开。
 * 文件不存在或 JSON 非法时返回 ok=false（显式指定的配置文件出错应
 * 让进程退出，静默继续会掩盖错误）。
 *
 * 识别的键：
 * - rpc: enabled / host / port / secret / allow_origin_all
 * - daemon: run_as_daemon / pid_file / working_dir / log_file
 * - storage: task_db_path
 *
 * @param path 配置文件路径
 * @param rpc_config RPC 服务器配置（就地更新）
 * @param daemon_config 守护进程配置（就地更新）
 * @param task_db_path 任务数据库路径（就地更新）
 * @param enable_rpc 是否启用 RPC（就地更新）
 * @param run_as_daemon 是否守护化运行（就地更新）
 * @return ConfigLoadResult 加载结果；warnings 含未知键提示
 */
ConfigLoadResult apply_config_file(const std::string& path,
                                   falcon::daemon::rpc::JsonRpcServerConfig& rpc_config,
                                   DaemonConfig& daemon_config,
                                   std::string& task_db_path,
                                   bool& enable_rpc,
                                   bool& run_as_daemon);

/**
 * @brief 展开 "~/" 前缀为用户主目录（无前缀时原样返回）
 */
std::string expand_home_path(const std::string& path);

/**
 * @brief 默认配置文件路径（get_default_config_dir() + "/daemon.json"）
 */
std::string get_default_config_file();

} // namespace falcon::daemon
