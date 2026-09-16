// Falcon Daemon - Main Entry Point

#include <falcon/download_engine.hpp>
#include <falcon/logger.hpp>
#include <falcon/protocols/v2_engine_host.hpp>

#include "rpc/json_rpc_server.hpp"
#include "daemon/daemon.hpp"
#include "daemon/config.hpp"

#ifdef FALCON_HAS_SQLITE3
#include "storage/task_storage.hpp"
#include "storage/task_storage_listener.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool parse_bool(const std::string& s, bool default_value) {
    if (s.empty()) return default_value;
    if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "off") return false;
    return default_value;
}

void show_help() {
    std::cout
        << "Falcon Daemon (aria2-compatible JSON-RPC)\n\n"
        << "Usage:\n"
        << "  falcon-daemon [OPTIONS]\n\n"
        << "Options:\n"
        << "  -h, --help                  Show this help\n"
        << "  --conf-path <file>          Config file (JSON); default: try\n"
        << "                              ~/.config/falcon/daemon.json\n"
        << "  --no-conf                   Do not load any config file\n"
        << "  --enable-rpc[=true|false]   Enable JSON-RPC server (default: false)\n"
        << "  --rpc-listen-port <port>    Listen port (default: 6800)\n"
        << "  --rpc-secret <token>        Require token:<token> in JSON-RPC params\n"
        << "  --rpc-allow-origin-all      Add CORS headers (Access-Control-Allow-Origin: *)\n"
        << "  --rpc-listen-host <ip>      Bind address (default: 127.0.0.1)\n\n"
        << "Daemon Options:\n"
        << "  -d, --daemon                Run as background daemon\n"
        << "  --pid-file <path>           PID file path\n"
        << "  --working-dir <dir>         Working directory\n"
        << "  --log-file <path>           Log file path (redirects stdout/stderr)\n"
        << "  --task-db <path>            Task database path (default: ~/.config/falcon/tasks.db)\n"
        << "  --http-engine <v1|v2>       HTTP download engine (default: v1;\n"
        << "                              v2 = experimental, restart to change)\n\n"
#ifdef _WIN32
        << "Windows Service Options:\n"
        << "  --install-service           Install as Windows service\n"
        << "  --uninstall-service         Uninstall Windows service\n"
        << "  --service-name <name>       Service name (default: falcon-daemon)\n\n"
#endif
        << "Examples:\n"
        << "  falcon-daemon --enable-rpc --rpc-listen-port 6800\n"
        << "  falcon-daemon --enable-rpc --rpc-secret mytoken\n"
        << "  falcon-daemon --conf-path /etc/falcon/daemon.json\n"
        << "  falcon-daemon -d --enable-rpc --pid-file /var/run/falcon.pid\n";
}

} // namespace

int main(int argc, char* argv[]) {
    bool enable_rpc = false;
    bool run_as_daemon = false;
    std::string task_db_path;
    falcon::daemon::rpc::JsonRpcServerConfig rpc_config;
    falcon::daemon::DaemonConfig daemon_config;
    falcon::daemon::DownloadConfig download_config;

#ifdef _WIN32
    bool install_service = false;
    bool uninstall_service = false;
    std::string service_name = "falcon-daemon";
#endif

    // 先扫出配置文件参数并加载（优先级：CLI 显式参数 > 配置文件 > 默认值；
    // 下面的解析循环只在参数显式给出时写入配置结构，天然覆盖文件值）。
    // 必须在 daemonize 之前完成：pid_file/working_dir/log_file 影响守护化行为。
    // active_conf_path 记录实际生效的配置文件，SIGHUP 重载时重读它。
    std::string active_conf_path;
    {
        std::string conf_path;
        bool no_conf = false;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--conf-path" && i + 1 < argc) {
                conf_path = argv[++i];
            } else if (arg == "--no-conf") {
                no_conf = true;
            }
        }

        if (!no_conf) {
            const std::string path =
                conf_path.empty() ? falcon::daemon::get_default_config_file() : conf_path;
            // 显式指定必须存在；默认路径存在才加载（aria2 语义）
            if (!conf_path.empty() || std::filesystem::exists(path)) {
                auto result = falcon::daemon::apply_config_file(
                    path, rpc_config, daemon_config, task_db_path,
                    enable_rpc, run_as_daemon, download_config);
                if (!result.ok) {
                    std::cerr << "Error loading config file: " << result.error << "\n";
                    return 1;
                }
                for (const auto& warning : result.warnings) {
                    std::cerr << "Warning: " << warning << "\n";
                }
                active_conf_path = path;
            }
        }
    }

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help();
            return 0;
        }

        // RPC options
        if (arg == "--conf-path" && i + 1 < argc) {
            ++i;  // 已在上方配置文件加载阶段消费
            continue;
        }
        if (arg == "--no-conf") {
            continue;
        }
        if (arg == "--enable-rpc") {
            enable_rpc = true;
            continue;
        }
        if (arg.rfind("--enable-rpc=", 0) == 0) {
            enable_rpc = parse_bool(arg.substr(std::strlen("--enable-rpc=")), true);
            continue;
        }
        if (arg == "--rpc-listen-port" && i + 1 < argc) {
            rpc_config.listen_port = static_cast<uint16_t>(std::stoi(argv[++i]));
            continue;
        }
        if (arg == "--rpc-secret" && i + 1 < argc) {
            rpc_config.secret = argv[++i];
            continue;
        }
        if (arg == "--rpc-allow-origin-all") {
            rpc_config.allow_origin_all = true;
            continue;
        }
        if (arg == "--rpc-listen-host" && i + 1 < argc) {
            rpc_config.bind_address = argv[++i];
            continue;
        }

        // Daemon options
        if (arg == "-d" || arg == "--daemon") {
            run_as_daemon = true;
            continue;
        }
        if (arg == "--pid-file" && i + 1 < argc) {
            daemon_config.pid_file = argv[++i];
            daemon_config.create_pid_file = true;
            continue;
        }
        if (arg == "--working-dir" && i + 1 < argc) {
            daemon_config.working_dir = argv[++i];
            continue;
        }
        if (arg == "--log-file" && i + 1 < argc) {
            daemon_config.log_file = argv[++i];
            continue;
        }
        if (arg == "--task-db" && i + 1 < argc) {
            task_db_path = argv[++i];
            continue;
        }
        if (arg == "--http-engine" && i + 1 < argc) {
            const std::string value = argv[++i];
            if (value != "v1" && value != "v2") {
                std::cerr << "Invalid value for --http-engine: " << value
                          << " (expected \"v1\" or \"v2\")\n";
                return 1;
            }
            download_config.http_engine = value;
            continue;
        }

#ifdef _WIN32
        // Windows Service options
        if (arg == "--install-service") {
            install_service = true;
            continue;
        }
        if (arg == "--uninstall-service") {
            uninstall_service = true;
            continue;
        }
        if (arg == "--service-name" && i + 1 < argc) {
            service_name = argv[++i];
            continue;
        }
#endif

        std::cerr << "Unknown argument: " << arg << "\n";
        std::cerr << "Use --help to see options.\n";
        return 1;
    }

#ifdef _WIN32
    // Windows Service management
    if (install_service) {
        falcon::daemon::DaemonManager dm(daemon_config);
        // Get current executable path
        char exe_path[MAX_PATH];
        GetModuleFileNameA(nullptr, exe_path, MAX_PATH);
        if (dm.install_service(exe_path, "Falcon Download Daemon",
                               "High-performance multi-protocol download daemon")) {
            std::cout << "Service installed successfully\n";
            return 0;
        }
        std::cerr << "Failed to install service: " << dm.get_last_error() << "\n";
        return 1;
    }

    if (uninstall_service) {
        falcon::daemon::DaemonManager dm(daemon_config);
        if (dm.uninstall_service()) {
            std::cout << "Service uninstalled successfully\n";
            return 0;
        }
        std::cerr << "Failed to uninstall service: " << dm.get_last_error() << "\n";
        return 1;
    }
#endif

    // Set default PID file if daemon mode and not specified
    if (run_as_daemon && daemon_config.pid_file.empty()) {
        daemon_config.pid_file = falcon::daemon::get_default_pid_file();
    }

    try {
        // Create daemon manager
        falcon::daemon::DaemonManager daemon_manager(daemon_config);

        // Daemonize if requested
        if (run_as_daemon) {
            if (!daemon_manager.daemonize()) {
                std::cerr << "Failed to daemonize: " << daemon_manager.get_last_error() << "\n";
                return 1;
            }
            // After daemonize, we're in the background
        } else {
            // 前台模式：daemonize() 未被调用，需显式安装信号处理器，
            // 否则 SIGTERM 按默认行为直接杀死进程，无法优雅退出（exit 0）。
            daemon_manager.setup_signal_handlers();
        }

        // Create download engine
        falcon::DownloadEngine engine;

        // 应用配置文件的下载参数（引擎运行时可调，SIGHUP 重载同样走这里）
        if (download_config.max_concurrent_tasks) {
            engine.set_max_concurrent_tasks(*download_config.max_concurrent_tasks);
        }
        if (download_config.max_overall_speed_limit) {
            engine.set_global_speed_limit(*download_config.max_overall_speed_limit);
        }

        // HTTP 数据面引擎开关（默认 v1，--http-engine / daemon.json
        // download.http_engine 选择）。必须在任务恢复之前设置：恢复的
        // Downloading 任务 start_task 即进入下载路径。开关翻转只在启动
        // 时生效——V2 多段稀疏临时文件与 V1 前缀续传布局不兼容，运行中
        // 切换会让既有任务以错误的续传布局恢复（SIGHUP 仅告警需重启）
        auto& v2_host = falcon::V2EngineHost::instance();
        const bool v2_http_enabled = download_config.http_engine == "v2";
        v2_host.set_v2_http_enabled(v2_http_enabled);
        if (v2_http_enabled) {
            v2_host.configure(falcon::EngineConfigV2{});
        }

        // Initialize task storage if SQLite3 is available
#ifdef FALCON_HAS_SQLITE3
        std::unique_ptr<falcon::daemon::TaskStorage> task_storage;
        falcon::daemon::TaskStorageConfig storage_config;
        if (task_db_path.empty()) {
            storage_config.db_path = falcon::daemon::get_default_config_dir() + "/tasks.db";
        } else {
            storage_config.db_path = task_db_path;
        }
        storage_config.auto_create_tables = true;
        storage_config.enable_wal_mode = true;

        // Ensure config directory exists
        std::filesystem::path db_path(storage_config.db_path);
        if (db_path.has_parent_path()) {
            falcon::daemon::create_directories(db_path.parent_path().string());
        }

        task_storage = std::make_unique<falcon::daemon::TaskStorage>(storage_config);
        if (task_storage->initialize()) {
            FALCON_LOG_INFO_STREAM("Task storage initialized: " << task_storage->get_db_path());

            // 引擎任务 id 计数器越过持久化记录的最大 id：RPC 按任务 id 写库，
            // 若重启后计数器从 1 重新开始，新任务会改写同 id 的历史记录
            if (auto max_id = task_storage->get_max_task_id()) {
                engine.set_next_task_id(*max_id);
            }

            // Load saved tasks：取全部记录，循环内跳过终态；
            // Paused 任务恢复但不自动启动
            auto saved_tasks = task_storage->list_tasks();
            FALCON_LOG_INFO_STREAM("Loading " << saved_tasks.size() << " saved tasks...");

            for (const auto& record : saved_tasks) {
                // Skip completed or failed tasks
                if (record.status == falcon::TaskStatus::Completed ||
                    record.status == falcon::TaskStatus::Failed ||
                    record.status == falcon::TaskStatus::Cancelled) {
                    continue;
                }

                // Re-create the task in the engine under its persisted id:
                // RPC 按该 id 寻址（gid ↔ TaskId），若 add_task 重新分配，
                // 重启后 pause/unpause/remove 等操作全部落到错误的 id 上
                auto task = engine.add_task_as_id(record.id, record.url, record.options);
                if (task) {
                    // Restore the output path recorded before shutdown
                    if (!record.output_path.empty()) {
                        task->set_output_path(record.output_path);
                    }
                    // Only start tasks that were active (not paused)
                    if (record.status == falcon::TaskStatus::Downloading ||
                        record.status == falcon::TaskStatus::Preparing) {
                        engine.start_task(task->id());
                    } else if (record.status == falcon::TaskStatus::Paused) {
                        // add_task_as_id 重建的任务是初始 Pending；暂停
                        // 任务必须还原为 Paused，resume_task 才会接受
                        // （aria2.unpause / resume_all 均按 Paused 判定）
                        task->set_status(falcon::TaskStatus::Paused);
                    }
                    FALCON_LOG_INFO_STREAM("Restored task: " << record.url);
                }
            }
        } else {
            FALCON_LOG_WARN_STREAM("Failed to initialize task storage: " << task_storage->get_last_error());
            task_storage.reset();
        }

        // 持久化监听器：状态变更实时落库，进度按 1 秒节流落库。
        // 声明在 task_storage 之后，逆序析构时先于 storage 销毁。
        std::unique_ptr<falcon::daemon::TaskStorageListener> task_listener;
        if (task_storage) {
            task_listener = std::make_unique<falcon::daemon::TaskStorageListener>(task_storage.get());
            engine.add_listener(task_listener.get());
        }

        // 事件分发器以裸指针持有监听者且 worker 线程存活到引擎析构，
        // 摘除是注册方责任：监听者对象先死而引擎后死时，引擎停机派发的
        // 尾部事件会回调悬垂指针（V2 停机事件密度下实测 SIGSEGV）。RAII
        // 覆盖全部退出路径（正常停机/RPC 启动失败/异常）；未注册时为
        // nullptr，remove_listener 对空指针安全
        struct ListenerDetacher {
            falcon::DownloadEngine& engine;
            falcon::IEventListener* listener;
            ~ListenerDetacher() { engine.remove_listener(listener); }
        } listener_detacher{engine, task_listener.get()};
#else
        FALCON_LOG_INFO_STREAM("Task persistence disabled (SQLite3 not available)");
#endif

        // 停机排水：暂停（而非取消）所有任务并等待引擎安静。
        // 取消会把任务落库为 Cancelled 导致重启后不再恢复；
        // 暂停则让未完成任务以可恢复状态（Paused/Downloading）入库。
        auto drain_engine = [&engine]() {
            FALCON_LOG_INFO_STREAM("Shutting down...");
            engine.pause_all();
            for (int i = 0; i < 50 && engine.get_active_task_count() > 0; ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            // 活动任务归零（V2 组已随各任务 pause 固化断点为 PAUSED）后
            // 收 V2 引擎宿主：排水 + 停 run 线程，避免进程退出挂在孤儿线程上
            falcon::V2EngineHost::instance().shutdown_and_join();
        };

#ifdef FALCON_HAS_SQLITE3
        // shutdown() 返回后监听器不再访问 storage，之后可安全析构 task_storage。
        // 坏库降级路径 task_storage 已 reset，listener 未创建，停机回调必须判空
        // ——否则空 unique_ptr 裸调 shutdown() 使 daemon 停机挂死/崩溃
        auto stop_persistence = [&task_listener]() {
            if (task_listener) task_listener->shutdown();
        };
#else
        auto stop_persistence = []() {};
#endif

        // Start RPC server if enabled
        std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> rpc_server;
        if (enable_rpc) {
#ifdef FALCON_HAS_SQLITE3
            rpc_server = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine, rpc_config, task_storage.get());
#else
            rpc_server = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine, rpc_config, nullptr);
#endif
            // aria2.forceShutdown/shutdown → 走正常停机流程（排水 + 落库）
            rpc_server->set_shutdown_handler([&daemon_manager]() { daemon_manager.request_stop(); });
            if (!rpc_server->start()) {
                std::cerr << "Failed to start JSON-RPC server\n";
                return 1;
            }
            FALCON_LOG_INFO_STREAM("JSON-RPC server started on "
                          << rpc_config.bind_address << ":" << rpc_config.listen_port);
        } else {
            FALCON_LOG_INFO_STREAM("RPC disabled (start with --enable-rpc)");
        }

        // Run main loop；stop 回调在收到停止信号/服务停止时执行
        daemon_manager.run(
            [&rpc_server, &drain_engine, &stop_persistence]() {
                // Stop callback
                if (rpc_server) {
                    rpc_server->stop();
                }
                drain_engine();
                stop_persistence();
            },
            [&]() {
                // Reload callback：重读 daemon.json，热更认证项，其余告警需重启。
                // 失败时保持现有配置继续运行（区别于启动时的硬失败）。
                if (active_conf_path.empty()) {
                    FALCON_LOG_INFO_STREAM("Config reload skipped: no config file in use");
                    return;
                }

                falcon::daemon::rpc::JsonRpcServerConfig new_rpc = rpc_config;
                falcon::daemon::DaemonConfig new_daemon = daemon_config;
                falcon::daemon::DownloadConfig new_download = download_config;
                std::string new_task_db = task_db_path;
                bool new_enable_rpc = enable_rpc;
                bool new_run_as_daemon = run_as_daemon;
                const auto result = falcon::daemon::apply_config_file(
                    active_conf_path, new_rpc, new_daemon, new_task_db,
                    new_enable_rpc, new_run_as_daemon, new_download);
                if (!result.ok) {
                    FALCON_LOG_WARN_STREAM("Config reload failed, keeping current config: "
                                         << result.error);
                    return;
                }
                for (const auto& warning : result.warnings) {
                    FALCON_LOG_WARN_STREAM("Config reload: " << warning);
                }

                // 可热更：RPC 认证（对后续请求立即生效；secret 不落日志）
                if (rpc_server &&
                    (new_rpc.secret != rpc_config.secret ||
                     new_rpc.allow_origin_all != rpc_config.allow_origin_all)) {
                    rpc_server->update_auth(new_rpc.secret, new_rpc.allow_origin_all);
                    FALCON_LOG_INFO_STREAM("RPC auth settings updated (applied immediately)");
                }

                // 可热更：下载参数（引擎 setter 运行时可调；optional 语义下
                // 节值未变则不动引擎，变了的键重新应用）
                if (new_download.max_concurrent_tasks != download_config.max_concurrent_tasks ||
                    new_download.max_overall_speed_limit != download_config.max_overall_speed_limit) {
                    if (new_download.max_concurrent_tasks) {
                        engine.set_max_concurrent_tasks(*new_download.max_concurrent_tasks);
                    }
                    if (new_download.max_overall_speed_limit) {
                        engine.set_global_speed_limit(*new_download.max_overall_speed_limit);
                    }
                    FALCON_LOG_INFO_STREAM("Download settings updated (applied immediately)");
                }

                // 需重启：监听、存储、守护化相关
                if (new_enable_rpc != enable_rpc ||
                    new_rpc.listen_port != rpc_config.listen_port ||
                    new_rpc.bind_address != rpc_config.bind_address) {
                    FALCON_LOG_WARN_STREAM("Config reload: RPC listen settings changed, "
                                         "restart required to apply");
                }
                // 引擎切换必须在停机窗口：运行中翻转会让既有任务以错误
                // 的续传布局恢复（V2 稀疏临时文件 vs V1 前缀布局）
                if (new_download.http_engine != download_config.http_engine) {
                    FALCON_LOG_WARN_STREAM("Config reload: download.http_engine changed, "
                                         "restart required to apply");
                }
                if (new_task_db != task_db_path) {
                    FALCON_LOG_WARN_STREAM("Config reload: task_db_path changed, "
                                         "restart required to apply");
                }
                if (new_run_as_daemon != run_as_daemon ||
                    new_daemon.pid_file != daemon_config.pid_file ||
                    new_daemon.working_dir != daemon_config.working_dir ||
                    new_daemon.log_file != daemon_config.log_file) {
                    FALCON_LOG_WARN_STREAM("Config reload: daemon settings changed, "
                                         "restart required to apply");
                }

                // 采纳为新基线，下一次重载据此对比
                rpc_config = new_rpc;
                daemon_config = new_daemon;
                download_config = new_download;
                task_db_path = new_task_db;
                enable_rpc = new_enable_rpc;
                run_as_daemon = new_run_as_daemon;
                FALCON_LOG_INFO_STREAM("Configuration reloaded from " << active_conf_path);
            }
        );

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
