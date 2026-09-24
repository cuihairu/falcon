#pragma once

// ============================================================================
// swarmd_config：swarm.json 配置文件加载
// （形制照抄 daemon/config.cpp：read_key / warn_unknown_keys / expand_home_path）
//
// 优先级 CLI > 配置文件 > 默认值——实现方式与 daemon 同：main 先加载配置
// 文件写入本聚合 struct，再解析 argv 在其上覆盖。键缺省不动目标（optional
// 语义）；未知节/未知键告警不失败（向前兼容）；类型错误报错退出（daemonize
// 之前，fail-fast）。阶段 0 无 SIGHUP 热更：配置只在启动读取一次。
// ============================================================================

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace falcon::swarm {

/// 进程级配置聚合（"swarm" + "daemon" 两节；CLI 覆盖在此之上）。
/// daemon 节四键与 daemon 包同名同语义，main 装配进 DaemonManager。
struct SwarmdFileConfig {
    // ---- "swarm" 节 --------------------------------------------------------
    std::string host = "127.0.0.1";
    std::uint16_t port = 7800;              // 0 = 内核分配临时端口（测试形态）
    std::string server_token;               // 空 = 不鉴权（main 启动时告警）
    std::string group_token;                // 空 = 不校验群组准入
    int heartbeat_interval_s = 60;          // 注册 result 回显给节点的续租间隔
    int heartbeat_timeout_s = 180;          // 超时未续租 → sweep 摘除 + onPeerLeft
    int challenge_ttl_s = 60;               // 注册挑战有效期
    int sweep_interval_ms = 1000;           // 清扫线程周期
    std::size_t rate_register_per_min = 5;  // per-IP 限频；0 = 不限
    std::size_t rate_query_per_min = 120;   // per-IP 限频；0 = 不限
    std::vector<std::string> blacklist;     // node_id 指纹清单（-32005 拒绝）

    // ---- "daemon" 节（falcon_daemon_core 的 DaemonManager 消费）------------
    bool run_as_daemon = false;
    std::string pid_file;     // 显式给出即创建（main 装配 create_pid_file）
    std::string working_dir;  // 空 = 不切换
    std::string log_file;     // 空 = 不重定向
};

struct ConfigLoadResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

/// 加载 JSON 配置文件，覆盖 config 的对应键（键缺省不动目标）。
/// 文件打不开 / 非法 JSON / 根非 object / 键类型不符 → ok=false + error。
ConfigLoadResult apply_config_file(const std::string& path,
                                   SwarmdFileConfig& config);

/// "~/" 前缀展开为 $HOME（Windows: %USERPROFILE%）；无 HOME 原样返回
std::string expand_home_path(const std::string& path);

/// 默认配置文件路径（get_default_config_dir() + "/swarm.json"）；
/// 默认路径存在才加载，--conf-path 显式指定则必须存在（daemon 同语义）
std::string get_default_config_file();

}  // namespace falcon::swarm
