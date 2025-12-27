// Falcon Daemon - Main Entry Point

#include <falcon/download_engine.hpp>
#include <falcon/logger.hpp>

#include "rpc/json_rpc_server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop_requested{false};

void handle_signal(int signum) {
    (void)signum;
    g_stop_requested.store(true);
}

bool parse_bool(const std::string& s, bool default_value) {
    if (s.empty()) return default_value;
    if (s == "1" || s == "true" || s == "TRUE" || s == "yes" || s == "on") return true;
    if (s == "0" || s == "false" || s == "FALSE" || s == "no" || s == "off") return false;
    return default_value;
}

void show_help() {
    std::cout
        << "Falcon Daemon (aria2-compatible JSON-RPC)\n\n"
        << "Options:\n"
        << "  --help                      Show this help\n"
        << "  --enable-rpc[=true|false]   Enable JSON-RPC server (default: false)\n"
        << "  --rpc-listen-port <port>    Listen port (default: 6800)\n"
        << "  --rpc-secret <token>        Require token:<token> in JSON-RPC params\n"
        << "  --rpc-allow-origin-all      Add CORS headers (Access-Control-Allow-Origin: *)\n"
        << "  --rpc-listen-host <ip>      Bind address (default: 127.0.0.1)\n\n"
        << "Examples:\n"
        << "  falcon-daemon --enable-rpc --rpc-listen-port 6800\n"
        << "  falcon-daemon --enable-rpc --rpc-secret mytoken\n";
}

} // namespace

int main(int argc, char* argv[]) {
    bool enable_rpc = false;
    falcon::daemon::rpc::JsonRpcServerConfig rpc_config;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            show_help();
            return 0;
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

        std::cerr << "Unknown argument: " << arg << "\n";
        std::cerr << "Use --help to see options.\n";
        return 1;
    }

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    try {
        falcon::DownloadEngine engine;

        std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> rpc_server;
        if (enable_rpc) {
            rpc_server = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine, rpc_config);
            if (!rpc_server->start()) {
                std::cerr << "Failed to start JSON-RPC server\n";
                return 1;
            }
        } else {
            FALCON_LOG_INFO("RPC disabled (start with --enable-rpc)");
        }

        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        if (rpc_server) {
            rpc_server->stop();
        }
        engine.cancel_all();

        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
}
