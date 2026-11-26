/**
 * @file main.cpp
 * @brief falcon-cli 主程序入口
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/protocols/v2_engine_host.hpp>
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

// 全局变量用于信号处理
std::atomic<bool> g_interrupted{false};
falcon::DownloadEngine* g_engine = nullptr;

// Non-blocking key check
inline bool kbhit_check() {
#ifdef _WIN32
    return _kbhit() != 0;
#else
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
#endif
}

inline int getch_nb() {
#ifdef _WIN32
    return _getch();
#else
    char c = 0;
    if (read(STDIN_FILENO, &c, 1) != 1) return -1;
    return static_cast<int>(c);
#endif
}

// 信号处理函数
void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_interrupted = true;
        if (g_engine) {
            g_engine->cancel_all();
        }
        std::cout << "\n\n" << term::yellow("Interrupt received, cancelling...") << "\n";
    }
}

/**
 * @brief CLI 事件监听器（支持多任务彩色进度显示）
 */
class CliEventListener : public falcon::IEventListener {
public:
    explicit CliEventListener(bool verbose = false, bool show_progress = true)
        : verbose_(verbose), show_progress_(show_progress), last_progress_(0.0) {}

    void set_task_names(const std::map<falcon::TaskId, std::string>& names) {
        task_names_ = names;
        task_progress_.clear();
        for (const auto& [id, _] : names) {
            task_progress_[id] = 0.0f;
        }
    }

    std::string format_size(falcon::Bytes bytes) const {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit = 0;
        double size = static_cast<double>(bytes);

        while (size >= 1024 && unit < 4) {
            size /= 1024;
            unit++;
        }

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1) << size << " " << units[unit];
        return oss.str();
    }

    void on_status_changed(falcon::TaskId task_id,
                           falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        if (!verbose_) return;

        using term::fg;
        using term::Color;
        std::string color;
        if (new_status == falcon::TaskStatus::Failed) color = fg(Color::Red);
        else if (new_status == falcon::TaskStatus::Completed) color = fg(Color::Green);
        else color = fg(Color::Cyan);

        std::cout << "\n" << color << "[Task " << task_id << "] "
                  << falcon::to_string(old_status)
                  << " -> " << falcon::to_string(new_status)
                  << term::reset() << "\n";
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        if (!show_progress_) return;

        std::lock_guard<std::mutex> lock(mutex_);
        task_progress_[info.task_id] = info.progress;

        // 单任务模式：覆盖当前行
        if (task_names_.size() <= 1) {
            if (info.progress - last_progress_ < 0.005 && info.progress < 1.0) return;
            last_progress_ = info.progress;
            display_single_progress(info);
            return;
        }

        // 多任务模式：更新所有行
        // 节流更新频率
        auto now = std::chrono::steady_clock::now();
        if (now - last_update_ < std::chrono::milliseconds(200)) return;
        last_update_ = now;

        display_multi_progress(info);
    }

    void on_error(falcon::TaskId task_id, const std::string& error_message) override {
        std::cout << "\n" << term::red("Error [Task " + std::to_string(task_id) + "]: ")
                  << error_message << "\n";
    }

    void on_completed(falcon::TaskId task_id, const std::string& output_path) override {
        std::cout << "\n" << term::green("OK [Task " + std::to_string(task_id) + "]: ")
                  << output_path << "\n";
    }

    void on_file_info(falcon::TaskId task_id, const falcon::FileInfo& info) override {
        if (!verbose_) return;

        std::cout << "\n" << term::cyan("FileInfo [Task " + std::to_string(task_id) + "]:")
                  << "\n  Size: " << format_size(info.total_size)
                  << "\n  Type: " << info.content_type << "\n";
    }

private:
    void display_single_progress(const falcon::ProgressInfo& info) {
        std::cout << "\r";

        // 进度条（30字符宽度）
        const int bar_width = 30;
        int filled = static_cast<int>(info.progress * bar_width);

        std::cout << "[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) {
                std::cout << term::fg(term::Color::Green) << "=" << term::reset();
            } else if (i == filled) {
                std::cout << term::fg(term::Color::Green) << ">" << term::reset();
            } else {
                std::cout << " ";
            }
        }
        std::cout << "] ";

        // 百分比（彩色）
        std::cout << term::bold()
                  << std::fixed << std::setprecision(1) << (info.progress * 100) << "%"
                  << term::reset() << " ";

        // 大小
        std::cout << "(" << format_size(info.downloaded_bytes);
        if (info.total_bytes > 0) {
            std::cout << "/" << format_size(info.total_bytes);
        }
        std::cout << ") ";

        // 速度
        if (info.speed > 0) {
            std::cout << term::fg(term::Color::Cyan)
                      << format_size(info.speed) << "/s"
                      << term::reset();
        }

        // ETA
        if (info.speed > 0 && info.total_bytes > info.downloaded_bytes) {
            auto remaining = (info.total_bytes - info.downloaded_bytes) / info.speed;
            int secs = static_cast<int>(remaining);
            int mins = secs / 60;
            secs = secs % 60;
            std::cout << " ETA " << mins << ":" << std::setfill('0') << std::setw(2) << secs;
        }

        std::cout.flush();
    }

    void display_multi_progress(const falcon::ProgressInfo& /*info*/) {
        // 移动光标到第一行进度
        int lines = static_cast<int>(task_names_.size());
        std::cout << term::move_up(lines);

        for (const auto& [id, name] : task_names_) {
            std::cout << term::clear_line();

            float prog = 0.0f;
            auto it = task_progress_.find(id);
            if (it != task_progress_.end()) prog = it->second;

            // 截断文件名显示
            std::string display_name = name;
            if (display_name.size() > 30) {
                display_name = "..." + display_name.substr(display_name.size() - 27);
            }

            // 迷你进度条（15字符）
            const int bar_w = 15;
            int filled = static_cast<int>(prog * bar_w);
            std::cout << "[";
            for (int j = 0; j < bar_w; ++j) {
                if (j < filled) std::cout << "=";
                else if (j == filled) std::cout << ">";
                else std::cout << " ";
            }

            int pct = static_cast<int>(prog * 100);
            std::cout << "] " << term::bold() << std::setw(3) << pct << "%" << term::reset()
                      << " " << display_name << "\n";
        }
        std::cout.flush();
    }

    bool verbose_;
    bool show_progress_;
    float last_progress_;
    std::mutex mutex_;
    std::map<falcon::TaskId, std::string> task_names_;
    std::map<falcon::TaskId, float> task_progress_;
    std::chrono::steady_clock::time_point last_update_ = std::chrono::steady_clock::now();
};

void show_help() {
    using term::bold;
    using term::fg;
    using term::Color;
    auto R = term::reset();

    std::cout << bold() << "Falcon CLI v" FALCON_CLI_VERSION << R << " - aria2-style multi-thread downloader\n\n";
    std::cout << fg(Color::Cyan) << "Usage:" << R << "\n";
    std::cout << "  falcon-cli [OPTIONS] <URL...>\n\n";
    std::cout << fg(Color::Cyan) << "Options:" << R << "\n";
    std::cout << "  -h, --help                 Show help\n";
    std::cout << "  -V, --version              Show version\n";
    std::cout << "  -i, --input-file <FILE>    Read URLs from file (- for stdin)\n";
    std::cout << "  -o, --output <FILE>        Output filename\n";
    std::cout << "  -d, --directory <DIR>      Output directory\n";
    std::cout << "  -C, --config <FILE>        Config file path\n";
    std::cout << "      --show-config-path     Show default config path\n";
    std::cout << "      --create-default-config Create default config\n";
    std::cout << "      --no-color             Disable colored output\n\n";

    std::cout << "下载队列选项 (aria2 风格):\n";
    std::cout << "  -j, --max-concurrent-downloads <N>  最大并发下载任务数 [默认: 1]\n";
    std::cout << "  -p, --priority <级别>      任务优先级: low/normal/high/critical [默认: normal]\n\n";

    std::cout << "多线程下载选项 (aria2 风格):\n";
    std::cout << "  -c, --connections <数量>   并发连接数 (1-64) [默认: 4]\n";
    std::cout << "  -x, --max-connections <N>  最大连接数 (同 -c)\n";
    std::cout << "      --max-connection-per-server <N>  aria2 兼容别名 (同 -c)\n";
    std::cout << "  -s, --split <N>            分块数量 (同 -c)\n";
    std::cout << "  -k, --min-split-size <大小>  最小分块大小（aria2 风格）[默认: 1M]\n";
    std::cout << "      --min-segment-size <大小>  最小分块大小 [默认: 1M]\n";
    std::cout << "      --no-adaptive          禁用自适应分块大小\n\n";

    std::cout << "高级选项:\n";
    std::cout << "      --no-continue          禁用断点续传\n";
    std::cout << "      --continue <true|false>  aria2 风格断点续传开关\n";
    std::cout << "      --limit <速度>         单任务限速 (如 1M)\n";
    std::cout << "      --max-download-limit <速度>  aria2 兼容别名 (同 --limit)\n";
    std::cout << "  -t, --timeout <秒>         超时时间 [默认: 30]\n";
    std::cout << "  -r, --retry <次数>        最大重试次数 [默认: 3]\n";
    std::cout << "      --retry-wait <秒>      aria2: 重试等待时间 [默认: 5]\n";
    std::cout << "      --no-verify-ssl        跳过 SSL 证书验证\n";
    std::cout << "      --check-certificate <true|false>  aria2 风格证书校验开关\n";
    std::cout << "      --proxy <URL>          设置代理服务器\n";
    std::cout << "      --proxy-user <用户>    aria2: 代理用户名\n";
    std::cout << "      --proxy-passwd <密码>   aria2: 代理密码\n";
    std::cout << "  -U, --user-agent <字符串>  自定义 User-Agent\n";
    std::cout << "      --referer <URL>        aria2: 设置 Referer\n";
    std::cout << "  -H, --header <头部>        自定义 HTTP 头 (可多次使用)\n";
    std::cout << "      --load-cookies <文件>  aria2: 从文件加载 Cookies\n";
    std::cout << "      --save-cookies <文件>  aria2: 保存 Cookies 到文件\n";
    std::cout << "      --http-user <用户>     aria2: HTTP 认证用户名\n";
    std::cout << "      --http-passwd <密码>    aria2: HTTP 认证密码\n";
    std::cout << "      --certificate <文件>   aria2: 客户端 TLS 证书 (PEM)\n";
    std::cout << "      --private-key <文件>   aria2: 客户端 TLS 私钥 (PEM)\n";
    std::cout << "      --http-engine <v1|v2>  HTTP 下载引擎（默认 v1；v2 实验性）\n";
    std::cout << "      --use-head             aria2: 使用 HEAD 方法获取文件信息\n";
    std::cout << "      --conditional-download aria2: 条件下载（仅当远程文件更新时）\n";
    std::cout << "      --auto-file-renaming   aria2: 自动重命名文件\n";
    std::cout << "      --file-allocation <模式> aria2: 文件预分配 (none|trunc|falloc|prealloc)\n";
    std::cout << "      --seed-ratio <比率>    aria2: BT 做种分享率 [默认: 1.0，0 = 下完即停]\n";
    std::cout << "      --seed-time <分钟>     aria2: BT 做种时长上限 [默认: 0 = 不限时]\n";
    std::cout << "      --p2sp-share[=true|false]  下载完成后向 swarm 公告本机资源（单发）\n";
    std::cout << "      --swarm-server <host:port> Rendezvous 服务地址 [默认端口: 7800]\n";
    std::cout << "      --p2sp-advertise <ip:port> 本机可达地址（缺省 = 服务端观测的直连地址）\n";
    std::cout << "      --swarm-fingerprint <hex>  服务器指纹钉扎（阶段 0 client 无 TLS，告警忽略）\n";
    std::cout << "                                 注: CLI 单发公告无续租无数据服务，资源可用\n";
    std::cout << "                                     窗口 = min(服务器 TTL, 进程退出前)，尽力而为\n\n";

    std::cout << "RPC 选项 (预留):\n";
    std::cout << "      --rpc-secret <令牌>    aria2: RPC 密钥\n";
    std::cout << "      --rpc-listen-port <端口> aria2: RPC 监听端口 [默认: 6800]\n";
    std::cout << "      --rpc-allow-origin-all aria2: 允许所有来源的 RPC 请求\n\n";

    std::cout << "输出选项:\n";
    std::cout << "  -v, --verbose              详细输出\n";
    std::cout << "  -q, --quiet                静默模式\n\n";

    std::cout << "示例:\n";
    std::cout << "  # 基础下载\n";
    std::cout << "  falcon-cli https://example.com/file.zip\n\n";
    std::cout << "  # 8 线程下载\n";
    std::cout << "  falcon-cli https://example.com/file.zip -c 8\n\n";
    std::cout << "  # 指定输出路径和分块大小\n";
    std::cout << "  falcon-cli https://example.com/file.zip -o /tmp/file.zip --min-segment-size 5M\n\n";
    std::cout << "  # 使用代理和自定义头\n";
    std::cout << "  falcon-cli https://example.com/file.zip --proxy http://127.0.0.1:7890 \\\n";
    std::cout << "           -H \"Authorization: Bearer token\"\n\n";
    std::cout << "  # aria2 兼容模式\n";
    std::cout << "  falcon-cli https://example.com/file.zip -x 16 -s 16 --min-segment-size 1M\n";
}

//==============================================================================
// 辅助函数：拆分 main() 的职责
//==============================================================================

/**
 * @brief 合并配置文件和命令行参数
 */
static void merge_config_with_file(CliArgs& args) {
#ifdef FALCON_USE_JSON
    // 加载配置文件
    auto file_config = falcon::cli::ConfigLoader::load_or_default(args.config_file);

    // 合并配置（命令行优先）
    if (file_config.max_connections != 4 && args.connections == 4) {
        args.connections = file_config.max_connections;
    }
    if (file_config.timeout_seconds != 30 && args.timeout == 30) {
        args.timeout = file_config.timeout_seconds;
    }
    if (file_config.max_retries != 3 && args.max_retries == 3) {
        args.max_retries = file_config.max_retries;
    }
    if (!file_config.default_download_dir.empty() && args.output_dir.empty()) {
        args.output_dir = file_config.default_download_dir;
    }
    if (file_config.speed_limit > 0 && args.speed_limit == 0) {
        args.speed_limit = file_config.speed_limit;
    }
    if (file_config.user_agent != "Falcon/0.2.0" && args.user_agent == "Falcon/0.2.0") {
        args.user_agent = file_config.user_agent;
    }
    if (!file_config.proxy.empty() && args.proxy.empty()) {
        args.proxy = file_config.proxy;
    }
    if (!file_config.proxy_username.empty() && args.proxy_user.empty()) {
        args.proxy_user = file_config.proxy_username;
    }
    if (!file_config.proxy_password.empty() && args.proxy_passwd.empty()) {
        args.proxy_passwd = file_config.proxy_password;
    }
    if (!file_config.referer.empty() && args.referer.empty()) {
        args.referer = file_config.referer;
    }
    if (!file_config.cookie_file.empty() && args.cookie_file.empty()) {
        args.cookie_file = file_config.cookie_file;
    }
    if (!file_config.http_username.empty() && args.http_user.empty()) {
        args.http_user = file_config.http_username;
    }
    if (!file_config.http_password.empty() && args.http_passwd.empty()) {
        args.http_passwd = file_config.http_password;
    }
    if (!file_config.client_cert.empty() && args.client_cert.empty()) {
        args.client_cert = file_config.client_cert;
    }
    if (!file_config.client_key.empty() && args.client_key.empty()) {
        args.client_key = file_config.client_key;
    }
    if (!file_config.rpc_secret.empty() && args.rpc_secret.empty()) {
        args.rpc_secret = file_config.rpc_secret;
    }
    if (file_config.rpc_listen_port != 6800 && args.rpc_listen_port == 6800) {
        args.rpc_listen_port = file_config.rpc_listen_port;
    }
    // 布尔值
    if (file_config.verbose) args.verbose = true;
    if (file_config.quiet) args.quiet = true;
    if (!file_config.resume_enabled && args.continue_download) {
        args.continue_download = false;
    }
    if (file_config.auto_renaming) args.auto_renaming = true;
    if (file_config.conditional_download) args.conditional_download = true;
    if (!file_config.file_allocation.empty() && args.file_allocation.empty()) {
        args.file_allocation = file_config.file_allocation;
    }
    if (file_config.seed_ratio != 1.0 && args.seed_ratio == 1.0) {
        args.seed_ratio = file_config.seed_ratio;
    }
    if (file_config.seed_time_minutes != 0 && args.seed_time_minutes == 0) {
        args.seed_time_minutes = file_config.seed_time_minutes;
    }
    // P2SP 公告三件套（阶段 1 增量 4）：CLI 非空胜出，空 = 保留配置
    // 文件值（p2sp_share 为三态字符串，"" 即未指定）
    if (!file_config.p2sp_share.empty() && args.p2sp_share.empty()) {
        args.p2sp_share = file_config.p2sp_share;
    }
    if (!file_config.swarm_server.empty() && args.swarm_server.empty()) {
        args.swarm_server = file_config.swarm_server;
    }
    if (!file_config.p2sp_advertise.empty() && args.p2sp_advertise.empty()) {
        args.p2sp_advertise = file_config.p2sp_advertise;
    }
    if (!file_config.swarm_fingerprint.empty() &&
        args.swarm_fingerprint.empty()) {
        args.swarm_fingerprint = file_config.swarm_fingerprint;
    }
    if (!file_config.verify_ssl) args.verify_ssl = false;
    // 合并 headers
    for (const auto& [k, v] : file_config.headers) {
        bool exists = false;
        for (const auto& h : args.headers) {
            if (h.first == k) { exists = true; break; }
        }
        if (!exists) {
            args.headers.push_back({k, v});
        }
    }
#else
    (void)args; // 避免未使用参数警告
#endif
}

/**
 * @brief 收集 URL 列表（命令行 + 输入文件）
 */
static std::vector<std::string> collect_urls(const CliArgs& args) {
    std::vector<std::string> urls = args.urls;
    if (!args.input_file.empty()) {
        auto file_urls = read_urls_from_file(args.input_file);
        urls.insert(urls.end(), file_urls.begin(), file_urls.end());
        if (file_urls.empty() && urls.empty()) {
            std::cerr << term::red("Error: ") << "cannot read URLs from: " << args.input_file << "\n";
        }
    }
    return urls;
}

/**
 * @brief 设置下载选项
 */
static falcon::DownloadOptions setup_download_options(const CliArgs& args) {
    falcon::DownloadOptions options;
    options.max_connections = static_cast<size_t>(args.connections);
    options.timeout_seconds = static_cast<size_t>(args.timeout);
    options.max_retries = static_cast<size_t>(args.max_retries);
    options.retry_delay_seconds = static_cast<size_t>(std::max(0, args.retry_wait));
    options.resume_enabled = args.continue_download;
    options.auto_file_renaming = args.auto_renaming;
    options.conditional_get = args.conditional_download;
    // file_allocation：CLI/config 空值保持引擎默认 "none"；非法值由
    // 引擎侧 parse_file_allocation 按 none 处理（稀疏，不告警不打扰）
    if (!args.file_allocation.empty()) {
        options.file_allocation = args.file_allocation;
    }
    options.seed_ratio = args.seed_ratio;
    options.seed_time_minutes = static_cast<std::size_t>(
        std::max(0, args.seed_time_minutes));
    options.speed_limit = args.speed_limit;
    options.min_segment_size = args.min_segment_size;
    options.adaptive_segment_sizing = args.adaptive_sizing;
    options.user_agent = args.user_agent;
    options.verify_ssl = args.verify_ssl;
    options.proxy = args.proxy;
    options.proxy_username = args.proxy_user;
    options.proxy_password = args.proxy_passwd;
    options.referer = args.referer;
    options.cookie_file = args.cookie_file;
    options.cookie_jar = args.save_cookies;
    options.http_username = args.http_user;
    options.http_password = args.http_passwd;
    options.client_certificate = args.client_cert;
    options.client_private_key = args.client_key;
    // P2SP 公告三态透传（""/"true"/"false"）——daemon SwarmAnnouncer
    // 消费，下载引擎侧零消费
    options.p2sp_share = args.p2sp_share;

    // 添加自定义 HTTP 头
    for (const auto& header : args.headers) {
        options.headers[header.first] = header.second;
    }

    // 设置输出路径
    if (!args.output_dir.empty()) {
        options.output_directory = args.output_dir;
    }
    if (!args.output_file.empty()) {
        options.output_filename = args.output_file;
    }

    return options;
}

/**
 * @brief 交互式控制循环
 */
static bool interactive_control_loop(
    falcon::DownloadEngine& engine,
    const std::vector<std::shared_ptr<falcon::DownloadTask>>& tasks
) {
#ifndef _WIN32
    struct termios old_tio, new_tio;
    tcgetattr(STDIN_FILENO, &old_tio);
    new_tio = old_tio;
    new_tio.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    tcsetattr(STDIN_FILENO, TCSANOW, &new_tio);
#endif

    std::cout << term::fg(term::Color::Yellow) << "\n  [p] Pause  [r] Resume  [q] Quit\n"
              << term::reset();

    while (!g_interrupted) {
        // 检查是否所有任务完成
        bool all_done = true;
        for (const auto& task : tasks) {
            auto s = task->status();
            if (s != falcon::TaskStatus::Completed &&
                s != falcon::TaskStatus::Failed &&
                s != falcon::TaskStatus::Cancelled) {
                all_done = false;
                break;
            }
        }
        if (all_done) break;

        // 检查键盘输入
        if (kbhit_check()) {
            int ch = getch_nb();
            if (ch == 'q' || ch == 'Q' || ch == 27) { // q or Esc
                g_interrupted = true;
                if (g_engine) g_engine->cancel_all();
                break;
            } else if (ch == 'p' || ch == 'P') {
                for (const auto& task : tasks) {
                    if (task->status() == falcon::TaskStatus::Downloading) {
                        engine.pause_task(task->id());
                    }
                }
                std::cout << "\n" << term::yellow("Paused all active tasks") << "\n";
            } else if (ch == 'r' || ch == 'R') {
                for (const auto& task : tasks) {
                    if (task->status() == falcon::TaskStatus::Paused) {
                        engine.resume_task(task->id());
                    }
                }
                std::cout << "\n" << term::green("Resumed all paused tasks") << "\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSANOW, &old_tio);
#endif

    return !g_interrupted;
}

/**
 * @brief 生成下载摘要统计
 */
static int generate_summary(const std::vector<std::shared_ptr<falcon::DownloadTask>>& tasks) {
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

int main(int argc, char* argv[]) {
    // Initialize terminal ANSI support
    term::enable_ansi();

    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#ifndef _WIN32
    // 对端 RST 后写 socket 默认触发 SIGPIPE 杀死进程（daemon 已同法
    // 忽略）；下载器对断连应得到 EPIPE 错误并正常收尾，而非被信号杀死
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // 解析命令行参数
    auto args = parse_args(argc, argv);

    // Handle color settings
    if (args.no_color || !term::is_terminal()) {
        term::color_enabled() = false;
    }

    // 显示帮助或版本信息
    if (args.show_help) {
        show_help();
        return 0;
    }

    if (args.show_version) {
        std::cout << term::bold() << "Falcon CLI" << term::reset() << " v" FALCON_CLI_VERSION << "\n";
        return 0;
    }

    // 显示配置文件路径
    if (args.show_config_path) {
#ifdef FALCON_USE_JSON
        std::cout << "Config search paths (in priority order):\n";
        for (const auto& path : falcon::cli::ConfigLoader::get_config_search_paths()) {
            std::cout << "  " << path;
            if (std::filesystem::exists(path)) {
                std::cout << " (exists)";
            }
            std::cout << "\n";
        }
        std::cout << "\nDefault config path: " << falcon::cli::ConfigLoader::get_default_config_path() << "\n";
#else
        std::cout << "Config file support not available (JSON library not linked)\n";
#endif
        return 0;
    }

    // 创建默认配置文件（若通过 -C/--config 显式指定路径，则写入该路径）
    if (args.create_default_config) {
#ifdef FALCON_USE_JSON
        std::string path = args.config_file.empty()
                               ? falcon::cli::ConfigLoader::get_default_config_path()
                               : args.config_file;
        if (falcon::cli::ConfigLoader::create_default_config(path)) {
            std::cout << "Default config created at: " << path << "\n";
            return 0;
        } else {
            std::cerr << "Failed to create default config: " << falcon::cli::ConfigLoader::get_last_error() << "\n";
            return 1;
        }
#else
        std::cerr << "Config file support not available\n";
        return 1;
#endif
    }

    // 合并配置文件
    merge_config_with_file(args);

    // --swarm-fingerprint：阶段 0 swarm client 无 TLS，指纹钉扎无处
    // 生效——前向声明形态，解析后 WARN 忽略（阶段 2 接线）
    if (!args.swarm_fingerprint.empty()) {
        std::cerr << term::yellow("WARN: swarm fingerprint ignored "
                                  "(phase-0 client has no TLS pinning)")
                  << "\n";
    }

    // 收集 URL 列表
    std::vector<std::string> urls = collect_urls(args);
    if (urls.empty()) {
        std::cerr << term::red("Error: ") << "missing URL argument\n"
                  << "Use --help for usage information\n";
        return 1;
    }

    if (!args.output_file.empty() && urls.size() > 1) {
        std::cerr << term::red("Error: ") << "-o/--output not supported for batch downloads\n";
        return 1;
    }

    try {
        // HTTP 数据面引擎开关（--http-engine，默认 v1）
        if (!args.http_engine.empty() && args.http_engine != "v1" && args.http_engine != "v2") {
            std::cerr << term::red("Error: ") << "invalid value for --http-engine: "
                      << args.http_engine << " (expected \"v1\" or \"v2\")\n";
            return 1;
        }
        auto& v2_host = falcon::V2EngineHost::instance();
        const bool v2_http_enabled = args.http_engine == "v2";
        v2_host.set_v2_http_enabled(v2_http_enabled);
        if (v2_http_enabled) {
            v2_host.configure(falcon::EngineConfigV2{});
        }

        // 创建下载引擎
        falcon::EngineConfig engine_config;
        engine_config.max_concurrent_tasks = static_cast<std::size_t>(args.max_concurrent_downloads);
        falcon::DownloadEngine engine(engine_config);
        g_engine = &engine;

        // 设置下载选项
        auto options = setup_download_options(args);

        // 创建事件监听器
        bool show_progress = !args.quiet;
        CliEventListener listener(args.verbose, show_progress);

        if (!args.quiet) {
            engine.add_listener(&listener);
        }

        // 事件分发器以裸指针持有监听者，摘除是注册方责任：本作用域退出
        // （含提前 return/异常）时先于 listener 析构摘除，否则引擎停机
        // 派发的尾部事件会回调悬垂对象（daemon 停机 SIGSEGV 的同型窗口）
        struct ListenerDetacher {
            falcon::DownloadEngine& engine;
            falcon::IEventListener* listener;
            ~ListenerDetacher() { engine.remove_listener(listener); }
        } listener_detacher{engine, &listener};

        // 添加下载任务
        auto tasks = engine.add_tasks(urls, options);
        if (tasks.empty()) {
            std::cerr << term::red("Error: ") << "cannot add download tasks (unsupported URL or plugin disabled)\n";
            return 1;
        }

        // Build task name map for multi-task display
        if (tasks.size() > 1 && show_progress) {
            std::map<falcon::TaskId, std::string> task_names;
            for (const auto& task : tasks) {
                task_names[task->id()] = task->url();
            }
            listener.set_task_names(task_names);

            // Reserve screen lines for multi-task progress
            std::cout << "\n"; // blank line before progress
            for (size_t i = 0; i < tasks.size(); ++i) {
                std::cout << "  Waiting...\n";
            }
        }

        if (!args.quiet) {
            if (tasks.size() == 1) {
                std::cout << term::bold() << "Downloading:" << term::reset()
                          << " " << tasks.front()->url() << "\n";
            } else {
                std::cout << term::bold() << "Downloading " << tasks.size() << " tasks"
                          << term::reset() << " (max concurrent: "
                          << args.max_concurrent_downloads << ")\n";
            }
        }

        // 启动前应用优先级，避免队列线程先按默认优先级调度任务
        if (args.priority_specified) {
            for (const auto& task : tasks) {
                engine.adjust_task_priority(task->id(), args.priority);
            }
        }

        // 启动任务
        for (const auto& task : tasks) {
            if (!engine.start_task(task->id())) {
                std::cerr << term::red("Error: ") << "cannot start download task: " << task->url() << "\n";
                return 1;
            }
        }

        // 等待任务完成
        bool success = true;
        if (term::is_terminal() && !args.quiet) {
            success = interactive_control_loop(engine, tasks);
        } else {
            engine.wait_all();
        }

        // 收 V2 引擎宿主（未启用时为无操作）：任务全部终态/暂停后桥接
        // 已返回，摘要与退出码基于最终状态生成
        falcon::V2EngineHost::instance().shutdown_and_join();

        if (g_interrupted || !success) {
            std::cerr << "\n" << term::yellow("Download cancelled") << "\n";
            return 1;
        }

        // 生成摘要统计
        const int exit_code = generate_summary(tasks);

        // P2SP 单发公告（阶段 1 增量 4）：仅在干净退出路径执行（中断
        // 路径已在上方 return）；公告失败仅 WARN，绝不改变下载退出码
        announce_p2sp_resources(args, tasks);

        return exit_code;

    } catch (const std::exception& e) {
        std::cerr << term::red("Fatal: ") << e.what() << "\n";
        return 1;
    }
}
