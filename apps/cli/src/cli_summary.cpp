/**
 * @file main.cpp
 * @brief falcon-cli 主程序入口
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include "arg_parser.hpp"
#include "config_loader.hpp"
#include "terminal.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <cstdint>
#include <sstream>
#include <filesystem>
#include <map>
#include <mutex>

#ifdef _WIN32
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#endif

// P2SP 单发公告（阶段 1 增量 4，§16.5）：仅当构建链接了 falcon_swarm_client
//（根 CMakeLists 定义 FALCON_HAS_SWARM）才编译真实公告链路；缺 CURL 的
// 构建走 #else WARN 形态（--swarm-server 解析仍生效，公告禁用）
#ifdef FALCON_HAS_SWARM
#include <falcon/protocols/file_hash.hpp>
#include "client/swarm_client.hpp"
#include "client/swarm_key_store.hpp"

#include <nlohmann/json.hpp>
#endif

namespace term = falcon::cli::term;
using falcon::cli::CliArgs;
using falcon::cli::parse_args;
using falcon::cli::read_urls_from_file;

#include "cli_summary.hpp"

/**
 * @brief 生成下载摘要统计
 */
int generate_summary(const std::vector<std::shared_ptr<falcon::DownloadTask>>& tasks) {
    int ok = 0, failed = 0, cancelled = 0;
    for (const auto& task : tasks) {
        auto s = task->status();
        if (s == falcon::TaskStatus::Completed) ok++;
        else if (s == falcon::TaskStatus::Failed) {
            failed++;
            std::cerr << term::red("FAIL") << " " << task->url()
                      << "\n  " << task->error_message() << "\n";
        } else if (s == falcon::TaskStatus::Cancelled) {
            cancelled++;
            std::cerr << term::yellow("SKIP") << " " << task->url() << "\n";
        }
    }

    if (tasks.size() > 1 || failed > 0 || cancelled > 0) {
        std::cout << "\n"
                  << term::green(std::to_string(ok) + " OK");
        if (failed > 0)
            std::cout << "  " << term::red(std::to_string(failed) + " FAILED");
        if (cancelled > 0)
            std::cout << "  " << term::yellow(std::to_string(cancelled) + " CANCELLED");
        std::cout << "\n";
    }

    return (failed + cancelled == 0) ? 0 : 1;
}

//==============================================================================
// P2SP 单发公告（阶段 1 增量 4，§16.5，CLI 尽力而为形态）
//==============================================================================

/// 解析 --swarm-server <host:port>：host 必填，端口缺省 7800。IPv6
/// 字面量须带括号（"[::1]:7800"，括号保留交 URL 构造）；裸 "[::1]"
/// （无端口）同样接受。返回 false = 无法解析。
#ifdef FALCON_HAS_SWARM
static bool parse_swarm_server(const std::string& spec,
                               std::string* host, int* port) {
    if (spec.empty()) {
        return false;
    }
    if (spec.front() == '[') {
        const auto close = spec.find(']');
        if (close == std::string::npos) {
            return false;
        }
        *host = spec.substr(0, close + 1);
        if (close + 1 == spec.size()) {
            *port = 7800;
            return true;
        }
        if (spec[close + 1] != ':') {
            return false;
        }
        const std::string port_str = spec.substr(close + 2);
        if (port_str.empty()) {
            *port = 7800;
            return true;
        }
        try {
            std::size_t pos = 0;
            const long value = std::stol(port_str, &pos);
            if (pos != port_str.size() || value <= 0 || value > 65535) {
                return false;
            }
            *port = static_cast<int>(value);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
    const auto colon = spec.rfind(':');
    if (colon == std::string::npos) {
        *host = spec;
        *port = 7800;
        return true;
    }
    *host = spec.substr(0, colon);
    if (host->empty()) {
        return false;
    }
    const std::string port_str = spec.substr(colon + 1);
    if (port_str.empty()) {
        *port = 7800;
        return true;
    }
    try {
        std::size_t pos = 0;
        const long value = std::stol(port_str, &pos);
        if (pos != port_str.size() || value <= 0 || value > 65535) {
            return false;
        }
        *port = static_cast<int>(value);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}
#endif // FALCON_HAS_SWARM

/**
 * @brief 下载完成后的单发 swarm 公告（阻塞、无续租、无数据服务）
 *
 * CLI 单发形态（§16.5）：成功完成且有真实落盘产物的任务同步算
 * sha256 → announce 一次 → 随进程退出失效（无心跳续租，Rendezvous 侧
 * TTL 到期摘除；announce 失败 WARN 不失败下载）。daemon 是共享的一等
 * 形态（SwarmAnnouncer 常驻续租），CLI 公告属尽力而为，绝不改变退出码。
 */
static void announce_p2sp_resources(
    const CliArgs& args,
    const std::vector<std::shared_ptr<falcon::DownloadTask>>& tasks) {
    if (args.swarm_server.empty()) {
        return;
    }
#ifndef FALCON_HAS_SWARM
    (void)tasks;
    std::cerr << term::yellow("WARN: built without swarm support, "
                              "--swarm-server ignored") << "\n";
#else
    // 显式 --p2sp-share=false：本次下载不分享（单发公告的 CLI 消费点）
    if (args.p2sp_share == "false") {
        return;
    }

    // 收集成功完成且有真实落盘产物的任务
    struct AnnounceItem {
        std::string path;
        std::string name;
        std::uintmax_t size = 0;
    };
    std::vector<AnnounceItem> items;
    for (const auto& task : tasks) {
        if (task->status() != falcon::TaskStatus::Completed) {
            continue;
        }
        const std::string& path = task->output_path();
        if (path.empty()) {
            continue;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            continue;
        }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) {
            continue;
        }
        items.push_back({path, std::filesystem::path(path).filename().string(),
                         size});
    }
    if (items.empty()) {
        std::cerr << term::yellow("WARN: no completed downloads to announce, "
                                  "swarm announce skipped") << "\n";
        return;
    }

    // 同步哈希（CLI 无后台宿主，阻塞计算；失败跳过该条目）
    std::vector<falcon::swarm::AnnounceResource> resources;
    resources.reserve(items.size());
    for (const auto& item : items) {
        const std::string sha256 = falcon::FileHasher::calculate_streaming(
            item.path, falcon::HashAlgorithm::SHA256);
        if (sha256.empty()) {
            std::cerr << term::yellow("WARN: sha256 unavailable for ")
                      << item.path << ", skipped from swarm announce\n";
            continue;
        }
        falcon::swarm::AnnounceResource res;
        res.kind = "file";
        res.sha256 = sha256;
        res.name = item.name;
        res.has_size = true;
        res.size = item.size;
        // 服务器缺省 TTL 档（服务端钳 [3600, 604800]）；CLI 无续租，
        // 实际可用窗口 = min(TTL, 进程退出前)
        res.ttl_s = std::chrono::seconds(86400);
        resources.push_back(std::move(res));
    }
    if (resources.empty()) {
        return;
    }

    // --swarm-server <host:port>
    std::string host;
    int port = 0;
    if (!parse_swarm_server(args.swarm_server, &host, &port)) {
        std::cerr << term::yellow("WARN: invalid --swarm-server value '")
                  << args.swarm_server << "' (expected host:port), "
                  << "swarm announce skipped\n";
        return;
    }

    // 节点身份：与 daemon 同一密钥文件（同机共存时共享身份，Rendezvous
    // 侧会话互踢由节点 -32003 重注册自愈收敛——设计文档披露的已知形态）
    const std::string key_file =
        falcon::cli::ConfigLoader::get_config_dir() + "/swarm_key.pem";
    std::string key_error;
    auto key = falcon::swarm::load_or_create_swarm_key(key_file, &key_error);
    if (!key.valid()) {
        std::cerr << term::yellow("WARN: swarm identity key unavailable (")
                  << key_error << ")\n";
        return;
    }

    falcon::swarm::SwarmClientConfig client_config;
    client_config.host = host;
    client_config.port = port;
    client_config.agent = "falcon-cli";
    client_config.key_file = key_file;
    if (!args.p2sp_advertise.empty()) {
        // 显式可达地址 = 非直连形态（服务端不再以注册连接的源地址观测）
        client_config.advertise_addr = args.p2sp_advertise;
        client_config.advertise_direct = false;
    }

    falcon::swarm::SwarmClient client(client_config, std::move(key));
    std::string start_error;
    if (!client.start(&start_error)) {
        std::cerr << term::yellow("WARN: swarm announce failed: ")
                  << start_error << "\n";
        return;
    }

    nlohmann::json result;
    const falcon::swarm::SwarmError err = client.announce(resources, &result);
    if (!err.ok()) {
        std::cerr << term::yellow("WARN: swarm announce failed: ")
                  << err.code << " " << err.message << "\n";
    } else if (result.is_object() && result.value("rejected", 0) > 0) {
        // rejected 非错误（服务器逐条拒收形态，如哈希格式不符）
        std::cerr << term::yellow("WARN: swarm announce: server rejected ")
                  << result.value("rejected", 0) << " of "
                  << resources.size() << " entries\n";
    } else if (!args.quiet) {
        std::cout << term::green("Announced ") << resources.size()
                  << " resource(s) to swarm " << args.swarm_server
                  << " (availability ends at process exit)\n";
    }
    client.stop();
#endif
}

//==============================================================================
// 主函数
//==============================================================================

