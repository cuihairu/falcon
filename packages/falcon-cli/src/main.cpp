// Falcon CLI - Main Entry Point
// Copyright (c) 2025 Falcon Project

#include <falcon/falcon.hpp>
#include <falcon/resource_search.hpp>
#include <falcon/resource_browser.hpp>
#include <falcon/config_manager.hpp>
#include <falcon/password_manager.hpp>

// 包含所有浏览器实现
#include <falcon/ftp_browser.hpp>
#include <falcon/s3_browser.hpp>
#include <falcon/oss_browser.hpp>
#include <falcon/cos_browser.hpp>
#include <falcon/kodo_browser.hpp>
#include <falcon/upyun_browser.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

// Global engine pointer for signal handling
falcon::DownloadEngine* g_engine = nullptr;
std::atomic<bool> g_interrupted{false};

void signal_handler(int /*signal*/) {
    g_interrupted = true;
    if (g_engine) {
        g_engine->cancel_all();
    }
}

// Format bytes to human-readable string
std::string format_bytes(falcon::Bytes bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit_index = 0;
    double size = static_cast<double>(bytes);

    while (size >= 1024.0 && unit_index < 4) {
        size /= 1024.0;
        ++unit_index;
    }

    std::ostringstream ss;
    if (unit_index == 0) {
        ss << bytes << " " << units[unit_index];
    } else {
        ss << std::fixed << std::setprecision(2) << size << " " << units[unit_index];
    }
    return ss.str();
}

// Format duration to human-readable string
std::string format_duration(std::chrono::seconds seconds) {
    auto secs = seconds.count();
    if (secs < 0) {
        return "--:--:--";
    }

    int hours = static_cast<int>(secs / 3600);
    int mins = static_cast<int>((secs % 3600) / 60);
    int s = static_cast<int>(secs % 60);

    std::ostringstream ss;
    if (hours > 0) {
        ss << std::setfill('0') << std::setw(2) << hours << ":";
    }
    ss << std::setfill('0') << std::setw(2) << mins << ":"
       << std::setfill('0') << std::setw(2) << s;
    return ss.str();
}

// Get terminal width
int get_terminal_width() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        return w.ws_col;
    }
    return 80;
#endif
}

// Progress bar event listener
class ProgressListener : public falcon::IEventListener {
public:
    void on_status_changed(falcon::TaskId task_id, falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        (void)old_status;
        if (new_status == falcon::TaskStatus::Downloading) {
            std::cout << "\r\033[K";  // Clear line
            std::cout << "[开始下载] 任务 " << task_id << std::endl;
        } else if (new_status == falcon::TaskStatus::Completed) {
            std::cout << "\r\033[K";
            std::cout << "[完成] 任务 " << task_id << " 下载完成!" << std::endl;
        } else if (new_status == falcon::TaskStatus::Failed) {
            std::cout << "\r\033[K";
            std::cout << "[失败] 任务 " << task_id << std::endl;
        } else if (new_status == falcon::TaskStatus::Cancelled) {
            std::cout << "\r\033[K";
            std::cout << "[取消] 任务 " << task_id << std::endl;
        }
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        // Calculate progress bar
        int width = get_terminal_width();
        int bar_width = std::max(20, width - 60);

        int filled = 0;
        if (info.total_bytes > 0) {
            filled = static_cast<int>(
                (static_cast<double>(info.downloaded_bytes) /
                 static_cast<double>(info.total_bytes)) *
                bar_width);
        }

        // Build progress bar
        std::ostringstream bar;
        bar << "[";
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) {
                bar << "=";
            } else if (i == filled) {
                bar << ">";
            } else {
                bar << " ";
            }
        }
        bar << "]";

        // Format output
        std::string downloaded = format_bytes(info.downloaded_bytes);
        std::string total =
            info.total_bytes > 0 ? format_bytes(info.total_bytes) : "???";
        std::string speed = format_bytes(info.speed) + "/s";

        auto remaining = std::chrono::duration_cast<std::chrono::seconds>(
            info.estimated_remaining);
        std::string eta = format_duration(remaining);

        int percent = static_cast<int>(info.progress * 100);

        // Print progress
        std::cout << "\r\033[K";  // Clear line
        std::cout << bar.str() << " " << std::setw(3) << percent << "% "
                  << downloaded << "/" << total << " | " << speed
                  << " | ETA: " << eta;
        std::cout.flush();
    }

    void on_error(falcon::TaskId task_id,
                  const std::string& error_message) override {
        std::cout << "\r\033[K";
        std::cerr << "[错误] 任务 " << task_id << ": " << error_message
                  << std::endl;
    }

    void on_completed(falcon::TaskId task_id,
                      const std::string& output_path) override {
        std::cout << "\r\033[K";
        std::cout << "[完成] 已保存到: " << output_path << std::endl;
    }
};

void print_version() {
    std::cout << "Falcon Downloader v" << falcon::VERSION_STRING << std::endl;
    std::cout << "A modern multi-protocol download library and tool"
              << std::endl;
}

void print_help(const char* program_name) {
    std::cout << "Falcon 下载器 - 现代多协议下载工具" << std::endl;
    std::cout << std::endl;
    std::cout << "使用方法:" << std::endl;
    std::cout << "  # 下载模式" << std::endl;
    std::cout << "  " << program_name << " [下载选项] <URL>" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 搜索模式" << std::endl;
    std::cout << "  " << program_name << " --search <关键词> [搜索选项]" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 浏览模式" << std::endl;
    std::cout << "  " << program_name << " --list [浏览选项] <URL>" << std::endl;
    std::cout << std::endl;
    std::cout << "下载选项:" << std::endl;
    std::cout << "  -h, --help            显示帮助信息" << std::endl;
    std::cout << "  -V, --version         显示版本信息" << std::endl;
    std::cout << "  -o, --output <FILE>   输出文件名" << std::endl;
    std::cout << "  -d, --dir <DIR>       输出目录 (默认: 当前目录)" << std::endl;
    std::cout << "  -c, --connections <N> 并发连接数 (默认: 4)" << std::endl;
    std::cout << "  -l, --limit <SPEED>   限速 (例如: 1M, 512K)" << std::endl;
    std::cout << "  -t, --timeout <SEC>   超时时间 (默认: 30秒)" << std::endl;
    std::cout << "  --retry <N>           重试次数 (默认: 3)" << std::endl;
    std::cout << "  --proxy <URL>         代理服务器" << std::endl;
    std::cout << "  --no-resume           禁用断点续传" << std::endl;
    std::cout << "  --no-verify-ssl       跳过 SSL 验证" << std::endl;
    std::cout << "  -q, --quiet           静默模式" << std::endl;
    std::cout << std::endl;
    std::cout << "搜索选项:" << std::endl;
    std::cout << "  -s, --search <KEYWORD> 搜索资源" << std::endl;
    std::cout << "  --limit-results <N>   结果数量限制 (默认: 20)" << std::endl;
    std::cout << "  --engine <NAME>       指定搜索引擎 (可多次使用)" << std::endl;
    std::cout << "  --category <TYPE>     资源类型 (video/audio/software)" << std::endl;
    std::cout << "  --min-size <SIZE>     最小文件大小 (如: 100M)" << std::endl;
    std::cout << "  --max-size <SIZE>     最大文件大小" << std::endl;
    std::cout << "  --min-seeds <N>       最小种子数" << std::endl;
    std::cout << "  --sort-by <FIELD>     排序字段 (size/seeds/date)" << std::endl;
    std::cout << "  --download <INDEX>    下载搜索结果的第INDEX项 (从1开始)" << std::endl;
    std::cout << std::endl;
    std::cout << "浏览选项:" << std::endl;
    std::cout << "  -L, --list            列出远程目录内容" << std::endl;
    std::cout << "  --long               显示详细信息 (类似ls -l)" << std::endl;
    std::cout << "  --tree              树形显示目录结构" << std::endl;
    std::cout << "  --recursive         递归列出子目录" << std::endl;
    --  --sort-by <FIELD>   排序字段 (name/size/modified_time)" << std::endl;
    std::cout << "  --filter <PATTERN>   文件过滤 (支持*通配符)" << std::endl;
    std::cout << "  --show-hidden       显示隐藏文件" << std::endl;
    std::cout << "  --json              输出JSON格式" << std::endl;
    std::cout << std::endl;
    std::cout << "认证选项:" << std::endl;
    std::cout << "  --username <USER>    用户名" << std::endl;
    std::cout << "  --password <PASS>    密码" << std::endl;
    std::cout << "  --key-id <KEY>       AWS访问密钥ID" << std::endl;
    std::cout << "  --secret-key <KEY>   AWS密钥" << std::endl;
    std::cout << "  --region <REGION>     AWS区域" << std::endl;
    std::cout << std::endl;
    std::cout << "示例:" << std::endl;
    std::cout << "  # 基础下载" << std::endl;
    std::cout << "  " << program_name << " https://example.com/file.zip" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 搜索资源" << std::endl;
    std::cout << "  " << program_name << " --search \"Ubuntu 22.04\"" << std::endl;
    std::cout << "  " << program_name << " -s \"电影\" --min-seeds 10 --download 1" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 浏览FTP目录" << std::endl;
    std::cout << "  " << program_name << " --list ftp://ftp.example.com/pub" << std::endl;
    std::cout << "  " << program_name << " -L --long ftp://user:pass@ftp.example.com/remote/path" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 浏览S3存储桶" << std::endl;
    std::cout << " " << program_name << " --list s3://my-bucket" << std::endl;
    std::cout << "  " << program_name << " -L --list s3://my-bucket --key-id AKIAIOSFODNN7EXAMPLE --secret-key wJalrXUtnFEMI/" << std::endl;
    std::cout << std::endl;
    std::cout << "  # 递归浏览并显示树形结构" << std::endl;
    std::cout << "  " << program_name << " -L --tree --recursive ftp://ftp.example.com/pub" << std::endl;
    std::cout << std::endl;
    std::cout << "注意: 浏览模式不能与其他模式同时使用" << std::endl;
    std::cout << std::endl;
}

falcon::Bytes parse_speed_limit(const std::string& limit) {
    if (limit.empty()) {
        return 0;
    }

    falcon::Bytes value = 0;
    char suffix = 0;
    std::istringstream ss(limit);
    ss >> value >> suffix;

    switch (std::toupper(suffix)) {
        case 'K':
            return value * 1024;
        case 'M':
            return value * 1024 * 1024;
        case 'G':
            return value * 1024 * 1024 * 1024;
        case 'T':
            return value * 1024ULL * 1024 * 1024 * 1024;
        default:
            return value;
    }
}

falcon::Bytes parse_size_limit(const std::string& limit) {
    return parse_speed_limit(limit); // Same parsing logic
}

// 显示搜索结果
void display_search_results(const std::vector<falcon::search::SearchResult>& results) {
    if (results.empty()) {
        std::cout << "未找到相关资源" << std::endl;
        return;
    }

    std::cout << "\n找到 " << results.size() << " 个结果:\n" << std::endl;
    std::cout << std::left
              << std::setw(3) << "#"
              << std::setw(50) << "标题"
              << std::setw(15) << "大小"
              << std::setw(8) << "种子"
              << std::setw(8) << "连接"
              << std::setw(10) << "置信度"
              << std::setw(20) << "来源" << std::endl;
    std::cout << std::string(120, '-') << std::endl;

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::string title = r.title;
        if (title.length() > 47) {
            title = title.substr(0, 44) + "...";
        }

        std::cout << std::left
                  << std::setw(3) << (i + 1)
                  << std::setw(50) << title
                  << std::setw(15) << format_bytes(r.size)
                  << std::setw(8) << r.seeds
                  << std::setw(8) << r.peers
                  << std::setw(10) << std::fixed << std::setprecision(2) << r.confidence
                  << std::setw(20) << r.source << std::endl;
    }
}

// 执行搜索
bool perform_search(const falcon::search::SearchQuery& query,
                   std::vector<falcon::search::SearchResult>& results) {
    falcon::search::ResourceSearchManager search_manager;

    // 加载搜索引擎配置
    std::string config_path = std::getenv("HOME") ?
        std::string(std::getenv("HOME")) + "/.config/falcon/search_engines.json" :
        "config/search_engines.json";

    if (!search_manager.load_config(config_path)) {
        // 尝试当前目录
        config_path = "config/search_engines.json";
        if (!search_manager.load_config(config_path)) {
            std::cerr << "警告: 无法加载搜索引擎配置文件" << std::endl;
            std::cerr << "请确保配置文件位于 ~/.config/falcon/search_engines.json 或 config/search_engines.json" << std::endl;
            return false;
        }
    }

    std::cout << "正在搜索: " << query.keyword << std::endl;
    std::cout << "使用的搜索引擎: ";
    auto providers = search_manager.get_providers();
    for (size_t i = 0; i < providers.size(); ++i) {
        if (i > 0) std::cout << ", ";
        std::cout << providers[i];
    }
    std::cout << std::endl;

    // 执行搜索
    results = search_manager.search_all(query);
    return true;
}

// 执行浏览
int perform_browse(const std::string& url) {
    falcon::ListOptions options;
    options.show_hidden = show_hidden;
    options.recursive = recursive;
    options.sort_by = sort_field;
    options.sort_desc = sort_desc;

    std::unique_ptr<falcon::IResourceBrowser> browser;

    // 根据URL协议创建浏览器
    if (url.find("ftp://") == 0 || url.find("ftps://") == 0) {
        browser = std::make_unique<falcon::FTPBrowser>();

        // 连接设置
        std::map<std::string, std::string> connect_options;
        if (!username.empty()) {
            connect_options["username"] = username;
        }
        if (!password.empty()) {
            connect_options["password"] = password;
        }

        if (!browser->connect(url, connect_options)) {
            std::cerr << "错误: 无法连接到FTP服务器" << std::endl;
            return 1;
        }
    } else if (url.find("s3://") == 0) {
        browser = std::make_unique<falcon::S3Browser>();

        // 连接设置
        std::map<std::string, std::string> connect_options;
        if (!access_key_id.empty()) {
            connect_options["access_key_id"] = access_key_id;
        }
        if (!secret_access_key.empty()) {
            connect_options["secret_access_key"] = secret_access_key;
        }
        if (!region.empty()) {
            connect_options["region"] = region;
        }

        if (!browser->connect(url, connect_options)) {
            std::cerr << "错误: 无法连接到S3服务" << std::endl;
            std::cerr << "请检查访问密钥和区域设置" << std::endl;
            return 1;
        }
    } else {
        std::cerr << "错误: 不支持的协议" << std::endl;
        std::cerr << "支持的协议: ftp, ftps, s3" << std::endl;
        return 1;
    }

    // 获取路径
    std::string path = browse_path;
    if (path.empty()) {
        // 从URL中提取路径
        if (url.find("s3://") == 0) {
            size_t bucket_end = url.find('/', 5);
            if (bucket_end != std::string::npos) {
                path = url.substr(bucket_end + 1);
            }
        } else {
            // FTP路径
            size_t proto_end = url.find("://");
            if (proto_end != std::string::npos) {
                std::string rest = url.substr(proto_end + 3);
                size_t path_start = rest.find('/');
                if (path_start != std::string::npos) {
                    path = rest.substr(path_start);
                }
            }
        }
    }

    // 切换到指定路径
    if (!path.empty() && path != "/") {
        if (!browser->change_directory(path)) {
            std::cerr << "错误: 无法切换到路径: " << path << std::endl;
            return 1;
        }
    }

    // 列出目录内容
    std::vector<falcon::RemoteResource> resources;
    try {
        resources = browser->list_directory(browser->get_current_directory(), options);
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    // 格式化输出
    std::string output;
    if (tree_format) {
        output = falcon::BrowserFormatter::format_tree(
            resources,
            browser->get_current_directory(),
            recursive ? 0 : 1
        );
    } else if (long_format) {
        output = falcon::BrowserFormatter::format_long(resources);
    } else {
        output = falcon::BrowserFormatter::format_short(resources);
    }

    std::cout << output << std::endl;

    // 显示当前路径
    if (!quiet && resources.empty()) {
        std::cout << "目录为空: " << browser->get_current_directory() << std::endl;
    }

    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Default options
    falcon::DownloadOptions options;
    std::string url;
    std::string search_keyword;
    std::vector<std::string> search_engines;
    size_t limit_results = 20;
    std::string category;
    size_t min_size = 0, max_size = 0;
    int min_seeds = 0;
    std::string sort_by;
    int download_index = -1;
    bool quiet = false;

    // 浏览模式相关变量
    bool browse_mode = false;
    bool list_format = false;
    bool long_format = false;
    bool tree_format = false;
    bool recursive = false;
    bool show_hidden = false;
    std::string sort_field = "name";
    bool sort_desc = false;
    std::string browse_path;
    std::string access_key_id;
    std::string secret_access_key;
    std::string region;
    std::string username;
    std::string password;

    // 配置管理相关
    std::string config_name;
    std::string provider_type;
    bool config_mode = false;
    bool add_config = false;
    bool list_configs = false;
    bool delete_config = false;
    bool set_master_pw = false;

    // Parse command line arguments (simple parser)
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            print_help(argv[0]);
            return 0;
        } else if (arg == "-V" || arg == "--version") {
            print_version();
            return 0;
        } else if ((arg == "-s" || arg == "--search") && i + 1 < argc) {
            search_keyword = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            options.output_filename = argv[++i];
        } else if ((arg == "-d" || arg == "--dir") && i + 1 < argc) {
            options.output_directory = argv[++i];
        } else if ((arg == "-c" || arg == "--connections") && i + 1 < argc) {
            options.max_connections = std::stoul(argv[++i]);
        } else if ((arg == "-l" || arg == "--limit") && i + 1 < argc) {
            options.speed_limit = parse_speed_limit(argv[++i]);
        } else if ((arg == "-t" || arg == "--timeout") && i + 1 < argc) {
            options.timeout_seconds = std::stoul(argv[++i]);
        } else if (arg == "--retry" && i + 1 < argc) {
            options.max_retries = std::stoul(argv[++i]);
        } else if (arg == "--proxy" && i + 1 < argc) {
            options.proxy = argv[++i];
        } else if (arg == "--proxy-username" && i + 1 < argc) {
            options.proxy_username = argv[++i];
        } else if (arg == "--proxy-password" && i + 1 < argc) {
            options.proxy_password = argv[++i];
        } else if (arg == "--proxy-type" && i + 1 < argc) {
            options.proxy_type = argv[++i];
        } else if (arg == "--no-resume") {
            options.resume_enabled = false;
        } else if (arg == "--no-verify-ssl") {
            options.verify_ssl = false;
        } else if (arg == "--limit-results" && i + 1 < argc) {
            limit_results = std::stoul(argv[++i]);
        } else if (arg == "--engine" && i + 1 < argc) {
            search_engines.push_back(argv[++i]);
        } else if (arg == "--category" && i + 1 < argc) {
            category = argv[++i];
        } else if (arg == "--min-size" && i + 1 < argc) {
            min_size = parse_size_limit(argv[++i]);
        } else if (arg == "--max-size" && i + 1 < argc) {
            max_size = parse_size_limit(argv[++i]);
        } else if (arg == "--min-seeds" && i + 1 < argc) {
            min_seeds = std::stoi(argv[++i]);
        } else if (arg == "--sort-by" && i + 1 < argc) {
            sort_by = argv[++i];
        } else if (arg == "--download" && i + 1 < argc) {
            download_index = std::stoi(argv[++i]);
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg == "--list" || arg == "-l") {
            browse_mode = true;
            list_format = true;
        } else if (arg == "-L" || arg == "--long") {
            long_format = true;
        } else if (arg == "--tree") {
            tree_format = true;
        } else if (arg == "--recursive" || arg == "-R") {
            recursive = true;
        } else if (arg == "--all" || arg == "-a") {
            show_hidden = true;
        } else if (arg == "--sort" && i + 1 < argc) {
            sort_field = argv[++i];
        } else if (arg == "--sort-desc" || arg == "-r") {
            sort_desc = true;
        } else if (arg == "--path" && i + 1 < argc) {
            browse_path = argv[++i];
        } else if (arg == "--key-id" && i + 1 < argc) {
            access_key_id = argv[++i];
        } else if (arg == "--secret-key" && i + 1 < argc) {
            secret_access_key = argv[++i];
        } else if (arg == "--region" && i + 1 < argc) {
            region = argv[++i];
        } else if (arg == "--username" && i + 1 < argc) {
            username = argv[++i];
        } else if (arg == "--password" && i + 1 < argc) {
            password = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            config_name = argv[++i];
            config_mode = true;
        } else if (arg == "--add-config" && i + 1 < argc) {
            add_config = true;
            config_name = argv[++i];
            config_mode = true;
        } else if (arg == "--list-configs") {
            list_configs = true;
            config_mode = true;
        } else if (arg == "--delete-config" && i + 1 < argc) {
            delete_config = true;
            config_name = argv[++i];
            config_mode = true;
        } else if (arg == "--provider" && i + 1 < argc) {
            provider_type = argv[++i];
        } else if (arg == "--set-master-password") {
            set_master_pw = true;
            config_mode = true;
        } else if (arg == "-q" || arg == "--quiet") {
            quiet = true;
        } else if (arg[0] != '-') {
            url = arg;
        } else {
            std::cerr << "未知选项: " << arg << std::endl;
            std::cerr << "使用 --help 查看帮助信息" << std::endl;
            return 1;
        }
    }

    // 搜索模式
    if (!search_keyword.empty()) {
        falcon::search::SearchQuery query;
        query.keyword = search_keyword;
        query.limit = limit_results;
        query.category = category;
        query.min_size = min_size;
        query.max_size = max_size;
        query.min_seeds = min_seeds;
        query.sort_by = sort_by;
        query.sort_desc = true;

        std::vector<falcon::search::SearchResult> results;
        if (!perform_search(query, results)) {
            return 1;
        }

        // 显示结果
        display_search_results(results);

        // 如果指定了下载索引
        if (download_index > 0 && download_index <= static_cast<int>(results.size())) {
            const auto& result = results[download_index - 1];
            url = result.url;

            std::cout << "\n开始下载第 " << download_index << " 个结果:" << std::endl;
            std::cout << "标题: " << result.title << std::endl;
            std::cout << "大小: " << format_bytes(result.size) << std::endl;
            std::cout << "链接: " << url << std::endl;
            std::cout << std::endl;
        } else if (download_index > 0) {
            std::cerr << "错误: 下载索引超出范围" << std::endl;
            return 1;
        } else {
            return 0; // 仅搜索，不下载
        }
    }

    // 浏览模式
    if (browse_mode) {
        if (url.empty()) {
            std::cerr << "错误: 浏览模式需要指定 URL" << std::endl;
            std::cerr << "使用 --help 查看帮助信息" << std::endl;
            return 1;
        }

        return perform_browse(url);
    }

    // 验证URL
    if (url.empty()) {
        std::cerr << "错误: 未指定下载 URL 或搜索关键词" << std::endl;
        std::cerr << "使用 --help 查看帮助信息" << std::endl;
        return 1;
    }

    // Setup signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        // Create engine
        falcon::EngineConfig config;
        config.auto_start = true;

        falcon::DownloadEngine engine(config);
        g_engine = &engine;

        // Add event listener
        ProgressListener listener;
        if (!quiet) {
            engine.add_listener(&listener);
        }

        // Note: In a real implementation, we would register the HTTP handler here
        // For now, we'll show a message about missing handlers
        auto protocols = engine.get_supported_protocols();
        if (protocols.empty()) {
            std::cerr << "警告: 未注册任何协议处理器" << std::endl;
            std::cerr << "请确保编译时启用了 FALCON_ENABLE_HTTP 选项" << std::endl;

            // For demonstration, show what would happen
            std::cout << "\n准备下载: " << url << std::endl;
            std::cout << "输出目录: "
                      << (options.output_directory.empty()
                              ? "."
                              : options.output_directory)
                      << std::endl;
            std::cout << "并发连接: " << options.max_connections << std::endl;
            if (options.speed_limit > 0) {
                std::cout << "限速: " << format_bytes(options.speed_limit) << "/s"
                          << std::endl;
            }
            std::cout << "\n(实际下载功能需要编译时启用 libcurl)" << std::endl;
            return 0;
        }

        // Add download task
        auto task = engine.add_task(url, options);
        if (!task) {
            std::cerr << "错误: 无法创建下载任务" << std::endl;
            return 1;
        }

        if (!quiet) {
            std::cout << "开始下载: " << url << std::endl;
            std::cout << "保存到: " << task->output_path() << std::endl;
            std::cout << std::endl;
        }

        // Wait for completion
        while (!task->is_finished() && !g_interrupted) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        std::cout << std::endl;

        if (task->status() == falcon::TaskStatus::Completed) {
            if (!quiet) {
                std::cout << "下载完成!" << std::endl;
            }
            return 0;
        } else if (task->status() == falcon::TaskStatus::Cancelled) {
            std::cerr << "下载已取消" << std::endl;
            return 130;  // Standard exit code for SIGINT
        } else {
            std::cerr << "下载失败: " << task->error_message() << std::endl;
            return 1;
        }

    } catch (const falcon::InvalidURLException& e) {
        std::cerr << "错误: 无效的 URL - " << e.what() << std::endl;
        return 1;
    } catch (const falcon::UnsupportedProtocolException& e) {
        std::cerr << "错误: 不支持的协议 - " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "错误: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
