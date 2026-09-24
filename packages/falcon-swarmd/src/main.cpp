// ============================================================================
// falcon-swarmd main：P2SP swarm 目录服务器入口（阶段 0，§12）
//
// 启动时序（daemon main.cpp 同形制）：配置文件预载（--conf-path/--no-conf，
// daemonize 之前完成全部校验 fail-fast）→ 主解析循环在其上覆盖（优先级
// CLI > 文件 > 默认）→ DaemonManager 守护化 → SwarmRpcServer start() →
// run() 主循环等待停止信号。
//
// 停机语义：SIGTERM/SIGINT 经 DaemonManager 信号处理器置停止标志，run()
// 返回前执行 stop 回调（server.stop() 排水），main 正常 return 0——
// 绝不走 _exit（gcda 覆盖率数据依赖正常退出 flush）。
//
// 阶段 0 无 SIGHUP 热更：配置只在启动读取一次，reload 回调传 nullptr。
// ============================================================================

#include "swarmd_config.hpp"
#include "daemon/daemon.hpp"
#include "server/swarm_rpc_server.hpp"
#include "server/swarm_server_state.hpp"

#include <falcon/logger.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>

namespace {

using falcon::swarm::SwarmdFileConfig;

void show_help() {
    std::cout << "Falcon Swarm Directory Server (P2SP stage 0)\n\n";
    std::cout << "Usage:\n";
    std::cout << "  falcon-swarmd [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -h, --help                    Show this help message and exit\n";
    std::cout << "      --conf-path <file>        Config file (default: ~/.config/falcon/swarm.json if present)\n";
    std::cout << "      --no-conf                 Do not load any config file\n\n";
    std::cout << "Swarm:\n";
    std::cout << "      --swarm-host <host>       Bind address (default: 127.0.0.1)\n";
    std::cout << "      --swarm-port <port>       Listen port, 0 = ephemeral (default: 7800)\n";
    std::cout << "      --server-token <token>    Bearer token for RPC auth (default: empty = unauthenticated)\n";
    std::cout << "      --group-token <token>     Group admission token (default: empty = open)\n";
    std::cout << "      --heartbeat-interval-s <s>   Heartbeat interval advertised to nodes (default: 60)\n";
    std::cout << "      --heartbeat-timeout-s <s>    Session timeout without heartbeat (default: 180)\n";
    std::cout << "      --challenge-ttl-s <s>     Register challenge validity (default: 60)\n";
    std::cout << "      --sweep-interval-ms <ms>  Sweeper period (default: 1000)\n";
    std::cout << "      --rate-register-per-min <n>  Register rate limit per IP (default: 5, 0 = unlimited)\n";
    std::cout << "      --rate-query-per-min <n>     Query rate limit per IP (default: 120, 0 = unlimited)\n\n";
    std::cout << "Daemon:\n";
    std::cout << "  -d, --daemon                   Run as background daemon\n";
    std::cout << "      --pid-file <file>          PID file (created only if specified)\n";
    std::cout << "      --working-dir <dir>        Working directory after daemonizing\n";
    std::cout << "      --log-file <file>          Log file for daemon mode\n\n";
    std::cout << "Examples:\n";
    std::cout << "  falcon-swarmd --server-token secret\n";
    std::cout << "  falcon-swarmd --conf-path /etc/falcon/swarm.json\n";
}

/// 正整数 CLI 参数解析（heartbeat/challenge/sweep 三个语义：<1 无意义）
bool parse_positive_int(const std::string& name, const std::string& value,
                        int& target) {
    try {
        const long long parsed = std::stoll(value);
        if (parsed <= 0 || parsed > std::numeric_limits<int>::max()) {
            throw std::out_of_range("range");
        }
        target = static_cast<int>(parsed);
        return true;
    } catch (const std::exception&) {
        std::cerr << "Invalid " << name << ": " << value << "\n";
        return false;
    }
}

/// 非负整数 CLI 参数解析（rate 两个语义：0 = 不限）
bool parse_size(const std::string& name, const std::string& value,
                std::size_t& target) {
    try {
        const long long parsed = std::stoll(value);
        if (parsed < 0 || parsed > static_cast<long long>(
                                         std::numeric_limits<std::size_t>::max())) {
            throw std::out_of_range("range");
        }
        target = static_cast<std::size_t>(parsed);
        return true;
    } catch (const std::exception&) {
        std::cerr << "Invalid " << name << ": " << value << "\n";
        return false;
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    SwarmdFileConfig config;

    // ------------------------------------------------------------------
    // 配置文件预载（主解析之前）：--conf-path 显式指定必须存在；默认路径
    // 存在才加载（daemon 同语义）。校验失败在 daemonize 之前报错退出。
    // ------------------------------------------------------------------
    std::string conf_path_arg;
    bool no_conf = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--conf-path" && i + 1 < argc) {
            conf_path_arg = argv[++i];
        } else if (arg == "--no-conf") {
            no_conf = true;
        }
    }

    if (!no_conf) {
        const std::string conf_file = conf_path_arg.empty()
            ? falcon::swarm::get_default_config_file()
            : conf_path_arg;
        std::error_code ec;
        const bool exists = std::filesystem::exists(conf_file, ec);
        if (!conf_path_arg.empty() && !exists) {
            std::cerr << "Config file not found: " << conf_file << "\n";
            return 1;
        }
        if (exists) {
            const auto result = falcon::swarm::apply_config_file(conf_file, config);
            if (!result.ok) {
                std::cerr << "Error loading config file: " << result.error << "\n";
                return 1;
            }
            for (const auto& warning : result.warnings) {
                std::cerr << "Warning: " << warning << "\n";
            }
            FALCON_LOG_INFO_STREAM("Loaded config file: " << conf_file);
        }
    }

    // ------------------------------------------------------------------
    // 主解析循环：CLI 参数显式给出时覆盖文件值（优先级 CLI > 文件 > 默认）
    // ------------------------------------------------------------------
    bool run_as_daemon = config.run_as_daemon;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            show_help();
            return 0;
        }
        if (arg == "--conf-path" && i + 1 < argc) {
            ++i;  // 已在预载段消费
            continue;
        }
        if (arg == "--no-conf") {
            continue;
        }
        if (arg == "--swarm-host" && i + 1 < argc) {
            config.host = argv[++i];
            continue;
        }
        if (arg == "--swarm-port" && i + 1 < argc) {
            const std::string value = argv[++i];
            try {
                const int port = std::stoi(value);
                if (port < 0 || port > 65535) throw std::out_of_range("range");
                config.port = static_cast<std::uint16_t>(port);
            } catch (const std::exception&) {
                std::cerr << "Invalid --swarm-port: " << value << "\n";
                return 1;
            }
            continue;
        }
        if (arg == "--server-token" && i + 1 < argc) {
            config.server_token = argv[++i];
            continue;
        }
        if (arg == "--group-token" && i + 1 < argc) {
            config.group_token = argv[++i];
            continue;
        }
        if (arg == "--heartbeat-interval-s" && i + 1 < argc) {
            if (!parse_positive_int("--heartbeat-interval-s", argv[++i],
                                    config.heartbeat_interval_s)) {
                return 1;
            }
            continue;
        }
        if (arg == "--heartbeat-timeout-s" && i + 1 < argc) {
            if (!parse_positive_int("--heartbeat-timeout-s", argv[++i],
                                    config.heartbeat_timeout_s)) {
                return 1;
            }
            continue;
        }
        if (arg == "--challenge-ttl-s" && i + 1 < argc) {
            if (!parse_positive_int("--challenge-ttl-s", argv[++i],
                                    config.challenge_ttl_s)) {
                return 1;
            }
            continue;
        }
        if (arg == "--sweep-interval-ms" && i + 1 < argc) {
            if (!parse_positive_int("--sweep-interval-ms", argv[++i],
                                    config.sweep_interval_ms)) {
                return 1;
            }
            continue;
        }
        if (arg == "--rate-register-per-min" && i + 1 < argc) {
            if (!parse_size("--rate-register-per-min", argv[++i],
                            config.rate_register_per_min)) {
                return 1;
            }
            continue;
        }
        if (arg == "--rate-query-per-min" && i + 1 < argc) {
            if (!parse_size("--rate-query-per-min", argv[++i],
                            config.rate_query_per_min)) {
                return 1;
            }
            continue;
        }
        if (arg == "-d" || arg == "--daemon") {
            run_as_daemon = true;
            continue;
        }
        if (arg == "--pid-file" && i + 1 < argc) {
            config.pid_file = falcon::swarm::expand_home_path(argv[++i]);
            continue;
        }
        if (arg == "--working-dir" && i + 1 < argc) {
            config.working_dir = falcon::swarm::expand_home_path(argv[++i]);
            continue;
        }
        if (arg == "--log-file" && i + 1 < argc) {
            config.log_file = falcon::swarm::expand_home_path(argv[++i]);
            continue;
        }

        std::cerr << "Unknown argument: " << arg << "\n";
        std::cerr << "Use --help to see options.\n";
        return 1;
    }

    // 无鉴权明示（配置文件/CLI 均未给 server_token）：空 = 不鉴权
    if (config.server_token.empty()) {
        std::cerr << "Warning: server_token is empty; the RPC API accepts "
                     "unauthenticated requests\n";
    }

    // ------------------------------------------------------------------
    // DaemonManager 装配与守护化（server start 之前——工作目录/stdio 重定向
    // 必须先于任何监听动作；配置校验已完成，daemonize 后零 fail-fast 路径）
    // ------------------------------------------------------------------
    falcon::daemon::DaemonConfig daemon_config;
    daemon_config.pid_file = config.pid_file;
    daemon_config.create_pid_file = !config.pid_file.empty();
    daemon_config.working_dir = config.working_dir;
    daemon_config.log_file = config.log_file;

    falcon::daemon::DaemonManager daemon_manager(daemon_config);
    if (run_as_daemon) {
        if (!daemon_manager.daemonize()) {
            std::cerr << "Failed to daemonize: " << daemon_manager.get_last_error()
                      << "\n";
            return 1;
        }
    } else {
        // 前台模式：daemonize() 未被调用，需显式安装信号处理器，
        // 否则 SIGTERM 按默认行为直接杀死进程，无法优雅退出（exit 0）。
        daemon_manager.setup_signal_handlers();
    }

    // ------------------------------------------------------------------
    // 服务器装配与启动
    // ------------------------------------------------------------------
    falcon::swarm::SwarmServerState::Config state_config;
    state_config.group_token = config.group_token;
    state_config.blacklist = config.blacklist;
    state_config.heartbeat_interval =
        std::chrono::seconds(config.heartbeat_interval_s);
    state_config.heartbeat_timeout =
        std::chrono::seconds(config.heartbeat_timeout_s);
    state_config.challenge_ttl = std::chrono::seconds(config.challenge_ttl_s);

    falcon::swarm::SwarmServerState state(state_config);

    falcon::swarm::SwarmServerOptions server_options;
    server_options.host = config.host;
    server_options.port = config.port;
    server_options.server_token = config.server_token;
    server_options.sweep_interval =
        std::chrono::milliseconds(config.sweep_interval_ms);
    server_options.rate_register_per_min = config.rate_register_per_min;
    server_options.rate_query_per_min = config.rate_query_per_min;

    falcon::swarm::SwarmRpcServer server(server_options, state);
    if (!server.start()) {
        std::cerr << "Failed to start swarm server: " << server.last_error()
                  << "\n";
        return 1;
    }
    if (!run_as_daemon) {
        std::cout << "falcon-swarmd listening on " << config.host << ":"
                  << server.port() << "\n";
    }

    // 主循环：阻塞至停止信号（SIGTERM/SIGINT → stop 回调 → run 返回）。
    // 阶段 0 无 SIGHUP 热更，reload 回调传 nullptr。
    daemon_manager.run(
        [&server]() {
            server.stop();
        },
        nullptr);
    server.stop();  // 幂等双保险（run 内部回调已停过则无操作）

    FALCON_LOG_INFO_STREAM("swarmd stopped");
    return 0;
}
