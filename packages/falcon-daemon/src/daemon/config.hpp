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

#include <cstdint>
#include <optional>
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

/// 下载参数（daemon.json 的 "download" 节）。
/// optional 语义：文件中出现对应键才有值，未出现的键保持引擎默认
struct DownloadConfig {
    std::optional<std::size_t> max_concurrent_tasks;        ///< 全局并发任务数
    std::optional<std::uint64_t> max_overall_speed_limit;   ///< 全局总限速（字节/秒，0=不限）
    /// HTTP 数据面引擎："v1"（libcurl，默认）|"v2"（实验性 V2 引擎）。
    /// 变更需在停机窗口重启生效——V2 多段稀疏临时文件与 V1 前缀续传
    /// 布局不兼容，运行中的进程不允许切换引擎续传既有任务
    std::string http_engine = "v1";
};

/// P2SP 共享与 Rendezvous 接入配置（daemon.json 的 "p2sp" 节，设计
/// 文档 docs/p2sp_network_design.md §16.5）。默认整体缺省 = share
/// 关闭（行为零变化）。`share.*` 全部可 SIGHUP 热更；`rendezvous.*`
/// 变化需重启（告警 restart required）。
struct P2spConfig {
    struct Share {
        bool enabled = false;             ///< 完成后公告开关（默认关）
        std::string mode = "standard";    ///< "standard"|"hash_only"（非法值告警保留 standard）
        bool one_way = false;             ///< 只消费不公告（§16.4 过滤链第 1 环）
        std::uint64_t ttl_s = 86400;      ///< 公告 TTL（秒，服务器侧钳 [3600, 604800]）
        std::uint64_t hash_delay_s = 0;   ///< 完成后延迟哈希（秒，错峰可选）
        bool announce_mirrors = true;     ///< 是否随 R1 公告 R2 镜像 URL
    };
    struct Rendezvous {
        std::string host = "127.0.0.1";   ///< Rendezvous 地址
        std::uint16_t port = 7800;        ///< Rendezvous 端口
        std::string server_token;         ///< 传输层 Bearer 令牌（空 = 服务器不鉴权）
        std::string group_token;          ///< 群组准入令牌（空 = 服务器不校验）
        std::string advertise_addr;       ///< 数据服务公告地址 ip:port（空 = direct=false）
        bool advertise_direct = true;     ///< 公告 direct 标记（NAT 后节点置 false）
    };
    Share share;
    Rendezvous rendezvous;
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
 * - download: max_concurrent_tasks / max_overall_speed_limit / http_engine
 * - mcp: enabled
 * - p2sp: share{enabled/mode/one_way/ttl_s/hash_delay_s/announce_mirrors} +
 *         rendezvous{host/port/server_token/group_token/advertise_addr/advertise_direct}
 *
 * @param path 配置文件路径
 * @param rpc_config RPC 服务器配置（就地更新）
 * @param daemon_config 守护进程配置（就地更新）
 * @param task_db_path 任务数据库路径（就地更新）
 * @param enable_rpc 是否启用 RPC（就地更新）
 * @param run_as_daemon 是否守护化运行（就地更新）
 * @param download_config 下载参数（就地更新，optional 语义）
 * @param p2sp_config P2SP 共享/接入参数（就地更新，默认缺省 = 共享关）
 * @return ConfigLoadResult 加载结果；warnings 含未知键提示
 */
ConfigLoadResult apply_config_file(const std::string& path,
                                   falcon::daemon::rpc::JsonRpcServerConfig& rpc_config,
                                   DaemonConfig& daemon_config,
                                   std::string& task_db_path,
                                   bool& enable_rpc,
                                   bool& run_as_daemon,
                                   DownloadConfig& download_config,
                                   P2spConfig& p2sp_config);

/**
 * @brief 展开 "~/" 前缀为用户主目录（无前缀时原样返回）
 */
std::string expand_home_path(const std::string& path);

/**
 * @brief 默认配置文件路径（get_default_config_dir() + "/daemon.json"）
 */
std::string get_default_config_file();

} // namespace falcon::daemon
