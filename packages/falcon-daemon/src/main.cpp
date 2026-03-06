// Falcon Daemon - Main Entry Point

#include <falcon/download_engine.hpp>
#include <falcon/logger.hpp>

#include "rpc/json_rpc_server.hpp"
#include "daemon/daemon.hpp"

#include <atomic>
#include <chrono>
#include <cstring>
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
        << "  --enable-rpc[=true|false]   Enable JSON-RPC server (default: false)\n"
        << "  --rpc-listen-port <port>    Listen port (default: 6800)\n"
        << "  --rpc-secret <token>        Require token:<token> in JSON-RPC params\n"
        << "  --rpc-allow-origin-all      Add CORS headers (Access-Control-Allow-Origin: *)\n"
        << "  --rpc-listen-host <ip>      Bind address (default: 127.0.0.1)\n\n"
        << "Daemon Options:\n"
        << "  -d, --daemon                Run as background daemon\n"
        << "  --pid-file <path>           PID file path\n"
        << "  --working-dir <dir>         Working directory\n"
        << "  --log-file <path>           Log file path (redirects stdout/stderr)\n\n"
#ifdef _WIN32
        << "Windows Service Options:\n"
        << "  --install-service           Install as Windows service\n"
        << "  --uninstall-service         Uninstall Windows service\n"
        << "  --service-name <name>       Service name (default: falcon-daemon)\n\n"
#endif
        << "Examples:\n"
        << "  falcon-daemon --enable-rpc --rpc-listen-port 6800\n"
        << "  falcon-daemon --enable-rpc --rpc-secret mytoken\n"
        << "  falcon-daemon -d --enable-rpc --pid-file /var/run/falcon.pid\n";
}

} // namespace

int main(int argc, char* argv[]) {
    bool enable_rpc = false;
    bool run_as_daemon = false;
    falcon::daemon::rpc::JsonRpcServerConfig rpc_config;
    falcon::daemon::DaemonConfig daemon_config;

#ifdef _WIN32
    bool install_service = false;
    bool uninstall_service = false;
    std::string service_name = "falcon-daemon";
#endif

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help();
            return 0;
        }

        // RPC options
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
        }

        // Create download engine
        falcon::DownloadEngine engine;

        // Setup stop callback
        daemon_manager.set_stop_callback([&engine]() {
            FALCON_LOG_INFO("Shutting down...");
            engine.cancel_all();
        });

        // Setup reload callback
        daemon_manager.set_reload_callback([]() {
            FALCON_LOG_INFO("Reloading configuration...");
            // TODO: Reload configuration file
        });

        // Start RPC server if enabled
        std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> rpc_server;
        if (enable_rpc) {
            rpc_server = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine, rpc_config);
            if (!rpc_server->start()) {
                std::cerr << "Failed to start JSON-RPC server\n";
                return 1;
            }
            FALCON_LOG_INFO("JSON-RPC server started on "
                          << rpc_config.bind_address << ":" << rpc_config.listen_port);
        } else {
            FALCON_LOG_INFO("RPC disabled (start with --enable-rpc)");
        }

        // Run main loop
        daemon_manager.run(
            [&rpc_server, &engine]() {
                // Stop callback
                if (rpc_server) {
                    rpc_server->stop();
                }
                engine.cancel_all();
            },
            []() {
                // Reload callback
            }
        );

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
