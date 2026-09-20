/**
 * @file arg_parser.hpp
 * @brief falcon-cli command-line argument parsing helpers
 */

#pragma once

#include <falcon/types.hpp>

#include <cstddef>
#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace falcon::cli {

/**
 * @brief Parsed CLI arguments.
 */
struct CliArgs {
    std::vector<std::string> urls;
    std::string input_file;
    std::string output_file;
    std::string output_dir;
    std::string config_file;
    int connections = 4;
    int max_concurrent_downloads = 1;
    falcon::Bytes speed_limit = 0;
    std::size_t min_segment_size = 1024 * 1024;
    bool continue_download = true;
    int timeout = 30;
    int max_retries = 3;
    int retry_wait = 5;
    bool adaptive_sizing = true;
    bool verify_ssl = true;
    std::string proxy;
    std::string proxy_user;
    std::string proxy_passwd;
    std::string user_agent = "Falcon/0.2.0";
    std::string referer;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string cookie_file;
    std::string save_cookies;
    std::string http_user;
    std::string http_passwd;
    /// 双向 TLS 客户端证书与私钥（PEM 路径，aria2 --certificate/
    /// --private-key 同语义；可指向同一复合 PEM 文件）
    std::string client_cert;
    std::string client_key;
    /// HTTP 数据面引擎："" 未指定（= v1）|"v1"|"v2"（实验性 V2 引擎）
    std::string http_engine;
    bool use_head = false;
    bool conditional_download = false;
    bool auto_renaming = false;
    std::string rpc_secret;
    int rpc_listen_port = 6800;
    bool rpc_allow_origin_all = false;
    bool verbose = false;
    bool quiet = false;
    bool show_help = false;
    bool show_version = false;
    falcon::TaskPriority priority = falcon::TaskPriority::Normal;
    bool priority_specified = false;
    bool show_config_path = false;
    bool create_default_config = false;
    bool no_color = false;
};

falcon::Bytes parse_size_bytes(std::string size_str);
falcon::TaskPriority parse_priority(const std::string& value);
bool parse_bool(std::string value, bool default_value);
std::vector<std::string> parse_url_lines(std::istream& in);
std::vector<std::string> read_urls_from_file(const std::string& path);
CliArgs parse_args(int argc, char* argv[]);

} // namespace falcon::cli
