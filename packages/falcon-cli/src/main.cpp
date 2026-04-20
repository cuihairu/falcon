/**
 * @file main.cpp
 * @brief falcon-cli 主程序入口
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/event_listener.hpp>
#include "config_loader.hpp"
#include "terminal.hpp"

#include <iostream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>
#include <sstream>
#include <fstream>
#include <cctype>
#include <algorithm>
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

namespace term = falcon::cli::term;

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

        int i = 0;
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
            ++i;
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

/**
 * @brief 简单命令行参数解析器 (aria2 兼容)
 */
struct CliArgs {
    std::vector<std::string> urls;
    std::string input_file;
    std::string output_file;
    std::string output_dir;
    std::string config_file;       // 配置文件路径
    int connections = 4;          // 并发连接数 (aria2 风格)
    int max_concurrent_downloads = 1;  // aria2: -j
    falcon::Bytes speed_limit = 0;  // 速度限制
    std::size_t min_segment_size = 1024 * 1024;  // 最小分块大小 (1MB)
    bool continue_download = true;
    int timeout = 30;
    int max_retries = 3;
    int retry_wait = 5;            // aria2: --retry-wait
    bool adaptive_sizing = true;   // 自适应分块大小
    bool verify_ssl = true;
    std::string proxy;
    std::string proxy_user;
    std::string proxy_passwd;
    std::string user_agent = "Falcon/0.2.0";
    std::string referer;           // aria2: --referer
    std::vector<std::pair<std::string, std::string>> headers;
    std::string cookie_file;       // aria2: --load-cookies
    std::string save_cookies;      // aria2: --save-cookies
    std::string http_user;         // aria2: --http-user
    std::string http_passwd;       // aria2: --http-passwd
    bool use_head = false;         // aria2: --use-head
    bool conditional_download = false;  // aria2: --conditional-download
    bool auto_renaming = false;    // aria2: --auto-file-renaming
    std::string rpc_secret;        // aria2: --rpc-secret
    int rpc_listen_port = 6800;    // aria2: --rpc-listen-port
    bool rpc_allow_origin_all = false;  // aria2: --rpc-allow-origin-all
    bool verbose = false;
    bool quiet = false;
    bool show_help = false;
    bool show_version = false;
    bool show_config_path = false; // 显示配置文件路径
    bool create_default_config = false; // 创建默认配置文件
    bool no_color = false; // 禁用彩色输出
};

static falcon::Bytes parse_size_bytes(std::string size_str) {
    if (size_str.empty()) return 0;

    std::size_t multiplier = 1;
    char suffix = size_str.back();
    if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024;
        size_str.pop_back();
    } else if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024 * 1024;
        size_str.pop_back();
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024 * 1024 * 1024;
        size_str.pop_back();
    } else if (suffix == 'T' || suffix == 't') {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        size_str.pop_back();
    }

    return static_cast<falcon::Bytes>(std::stoull(size_str) * multiplier);
}

static bool parse_bool(std::string value, bool default_value) {
    if (value.empty()) return default_value;
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (value == "1" || value == "true" || value == "yes" || value == "y" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "n" || value == "off") return false;
    return default_value;
}

static std::vector<std::string> read_urls_from_file(const std::string& path) {
    std::vector<std::string> urls;
    std::istream* in = nullptr;
    std::ifstream file;

    if (path == "-") {
        in = &std::cin;
    } else {
        file.open(path);
        if (!file.is_open()) {
            return urls;
        }
        in = &file;
    }

    std::string line;
    while (std::getline(*in, line)) {
        // Trim whitespace
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        std::size_t start = 0;
        while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
            ++start;
        }
        if (start > 0) {
            line = line.substr(start);
        }
        if (line.empty() || line[0] == '#') continue;
        urls.push_back(line);
    }

    return urls;
}

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            args.show_help = true;
        } else if (arg == "-V" || arg == "--version") {
            args.show_version = true;
        } else if (arg == "-i" || arg == "--input-file" || arg == "--input") {
            if (i + 1 < argc) {
                args.input_file = argv[++i];
            }
        } else if (arg == "-j" || arg == "--max-concurrent-downloads") {
            if (i + 1 < argc) {
                args.max_concurrent_downloads = std::max(1, std::min(64, std::stoi(argv[++i])));
            }
        } else if (arg == "-o" || arg == "--output") {
            if (i + 1 < argc) {
                args.output_file = argv[++i];
            }
        } else if (arg == "-d" || arg == "--directory" || arg == "--dir") {
            if (i + 1 < argc) {
                args.output_dir = argv[++i];
            }
        } else if (arg == "-c" || arg == "--connections") {
            if (i + 1 < argc) {
                args.connections = std::max(1, std::min(64, std::stoi(argv[++i])));
            }
        } else if (arg == "-x" || arg == "--max-connections") {
            if (i + 1 < argc) {
                args.connections = std::max(1, std::min(64, std::stoi(argv[++i])));
            }
        } else if (arg == "--max-connection-per-server") {
            if (i + 1 < argc) {
                args.connections = std::max(1, std::min(64, std::stoi(argv[++i])));
            }
        } else if (arg == "-s" || arg == "--split") {
            if (i + 1 < argc) {
                args.connections = std::max(1, std::min(64, std::stoi(argv[++i])));
            }
        } else if (arg == "-k" || arg == "--min-split-size") {
            if (i + 1 < argc) {
                args.min_segment_size = static_cast<std::size_t>(parse_size_bytes(argv[++i]));
            }
        } else if (arg == "--min-segment-size") {
            if (i + 1 < argc) {
                args.min_segment_size = static_cast<std::size_t>(parse_size_bytes(argv[++i]));
            }
        } else if (arg == "--no-continue") {
            args.continue_download = false;
        } else if (arg == "--continue") {
            if (i + 1 < argc) {
                args.continue_download = parse_bool(argv[++i], true);
            } else {
                args.continue_download = true;
            }
        } else if (arg == "--no-adaptive") {
            args.adaptive_sizing = false;
        } else if (arg == "--limit" || arg == "--max-download-limit") {
            if (i + 1 < argc) {
                args.speed_limit = parse_size_bytes(argv[++i]);
            }
        } else if (arg == "-t" || arg == "--timeout") {
            if (i + 1 < argc) {
                args.timeout = std::stoi(argv[++i]);
            }
        } else if (arg == "-r" || arg == "--retry") {
            if (i + 1 < argc) {
                args.max_retries = std::max(0, std::min(10, std::stoi(argv[++i])));
            }
        } else if (arg == "--no-verify-ssl") {
            args.verify_ssl = false;
        } else if (arg == "--check-certificate") {
            if (i + 1 < argc) {
                args.verify_ssl = parse_bool(argv[++i], true);
            } else {
                args.verify_ssl = true;
            }
        } else if (arg == "--proxy") {
            if (i + 1 < argc) {
                args.proxy = argv[++i];
            }
        } else if (arg == "-U" || arg == "--user-agent") {
            if (i + 1 < argc) {
                args.user_agent = argv[++i];
            }
        } else if (arg == "-H" || arg == "--header") {
            if (i + 1 < argc) {
                std::string header = argv[++i];
                auto pos = header.find(':');
                if (pos != std::string::npos && pos < header.length() - 1) {
                    std::string key = header.substr(0, pos);
                    std::string value = header.substr(pos + 1);
                    // Trim leading whitespace from value
                    while (!value.empty() && value[0] == ' ') {
                        value = value.substr(1);
                    }
                    args.headers.push_back({key, value});
                }
            }
        } else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        } else if (arg == "-q" || arg == "--quiet") {
            args.quiet = true;
        } else if (arg == "--retry-wait") {
            // aria2: --retry-wait
            if (i + 1 < argc) {
                args.retry_wait = std::max(0, std::stoi(argv[++i]));
            }
        } else if (arg == "--referer") {
            // aria2: --referer
            if (i + 1 < argc) {
                args.referer = argv[++i];
            }
        } else if (arg == "--load-cookies") {
            // aria2: --load-cookies
            if (i + 1 < argc) {
                args.cookie_file = argv[++i];
            }
        } else if (arg == "--save-cookies") {
            // aria2: --save-cookies
            if (i + 1 < argc) {
                args.save_cookies = argv[++i];
            }
        } else if (arg == "--http-user") {
            // aria2: --http-user
            if (i + 1 < argc) {
                args.http_user = argv[++i];
            }
        } else if (arg == "--http-passwd") {
            // aria2: --http-passwd
            if (i + 1 < argc) {
                args.http_passwd = argv[++i];
            }
        } else if (arg == "--proxy-user") {
            // aria2: --proxy-user
            if (i + 1 < argc) {
                args.proxy_user = argv[++i];
            }
        } else if (arg == "--proxy-passwd") {
            // aria2: --proxy-passwd
            if (i + 1 < argc) {
                args.proxy_passwd = argv[++i];
            }
        } else if (arg == "--use-head") {
            // aria2: --use-head
            args.use_head = true;
        } else if (arg == "--conditional-download") {
            // aria2: --conditional-download
            args.conditional_download = true;
        } else if (arg == "--auto-file-renaming") {
            // aria2: --auto-file-renaming
            args.auto_renaming = true;
        } else if (arg == "--rpc-secret") {
            // aria2: --rpc-secret
            if (i + 1 < argc) {
                args.rpc_secret = argv[++i];
            }
        } else if (arg == "--rpc-listen-port") {
            // aria2: --rpc-listen-port
            if (i + 1 < argc) {
                args.rpc_listen_port = std::stoi(argv[++i]);
            }
        } else if (arg == "--rpc-allow-origin-all") {
            // aria2: --rpc-allow-origin-all
            args.rpc_allow_origin_all = true;
        } else if (arg == "--config" || arg == "-C") {
            // 配置文件路径
            if (i + 1 < argc) {
                args.config_file = argv[++i];
            }
        } else if (arg == "--show-config-path") {
            // 显示配置文件路径
            args.show_config_path = true;
        } else if (arg == "--create-default-config") {
            // 创建默认配置文件
            args.create_default_config = true;
        } else if (arg == "--no-color") {
            args.no_color = true;
        } else if (!arg.empty() && arg[0] != '-') {
            args.urls.push_back(arg);
        }
    }

    return args;
}

void show_help() {
    using term::bold;
    using term::fg;
    using term::Color;
    auto R = term::reset();

    std::cout << bold() << "Falcon CLI v0.2.0" << R << " - aria2-style multi-thread downloader\n\n";
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
    std::cout << "  -j, --max-concurrent-downloads <N>  最大并发下载任务数 [默认: 1]\n\n";

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
    std::cout << "      --use-head             aria2: 使用 HEAD 方法获取文件信息\n";
    std::cout << "      --conditional-download aria2: 条件下载（仅当远程文件更新时）\n";
    std::cout << "      --auto-file-renaming   aria2: 自动重命名文件\n\n";

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
    new_tio.c_lflag &= ~(ICANON | ECHO);
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
// 主函数
//==============================================================================

int main(int argc, char* argv[]) {
    // Initialize terminal ANSI support
    term::enable_ansi();

    // 设置信号处理
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

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
        std::cout << term::bold() << "Falcon CLI" << term::reset() << " v0.2.0\n";
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

    // 创建默认配置文件
    if (args.create_default_config) {
#ifdef FALCON_USE_JSON
        std::string path = falcon::cli::ConfigLoader::get_default_config_path();
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

        if (g_interrupted || !success) {
            std::cerr << "\n" << term::yellow("Download cancelled") << "\n";
            return 1;
        }

        // 生成摘要统计
        return generate_summary(tasks);

    } catch (const std::exception& e) {
        std::cerr << term::red("Fatal: ") << e.what() << "\n";
        return 1;
    }
}
