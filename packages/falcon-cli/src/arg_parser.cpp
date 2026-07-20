/**
 * @file arg_parser.cpp
 * @brief falcon-cli command-line argument parsing helpers
 */

#include "arg_parser.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

namespace falcon::cli {

namespace {

void trim_ascii_whitespace(std::string& line) {
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) {
        line.pop_back();
    }

    std::size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) {
        ++start;
    }
    if (start > 0) {
        line = line.substr(start);
    }
}

} // namespace

falcon::Bytes parse_size_bytes(std::string size_str) {
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

falcon::TaskPriority parse_priority(const std::string& value) {
    std::string lower = value;
    for (auto& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (lower == "low" || lower == "0") return falcon::TaskPriority::Low;
    if (lower == "high" || lower == "2") return falcon::TaskPriority::High;
    if (lower == "critical" || lower == "3") return falcon::TaskPriority::Critical;
    return falcon::TaskPriority::Normal;
}

bool parse_bool(std::string value, bool default_value) {
    if (value.empty()) return default_value;

    for (auto& c : value) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (value == "1" || value == "true" || value == "yes" || value == "y" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "n" || value == "off") return false;
    return default_value;
}

std::vector<std::string> parse_url_lines(std::istream& in) {
    std::vector<std::string> urls;
    std::string line;

    while (std::getline(in, line)) {
        trim_ascii_whitespace(line);
        if (line.empty() || line[0] == '#') continue;
        urls.push_back(line);
    }

    return urls;
}

std::vector<std::string> read_urls_from_file(const std::string& path) {
    if (path == "-") {
        return parse_url_lines(std::cin);
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return {};
    }

    return parse_url_lines(file);
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
        } else if (arg == "--priority" || arg == "-p") {
            if (i + 1 < argc) {
                args.priority = parse_priority(argv[++i]);
                args.priority_specified = true;
            }
        } else if (arg == "--retry-wait") {
            if (i + 1 < argc) {
                args.retry_wait = std::max(0, std::stoi(argv[++i]));
            }
        } else if (arg == "--referer") {
            if (i + 1 < argc) {
                args.referer = argv[++i];
            }
        } else if (arg == "--load-cookies") {
            if (i + 1 < argc) {
                args.cookie_file = argv[++i];
            }
        } else if (arg == "--save-cookies") {
            if (i + 1 < argc) {
                args.save_cookies = argv[++i];
            }
        } else if (arg == "--http-user") {
            if (i + 1 < argc) {
                args.http_user = argv[++i];
            }
        } else if (arg == "--http-passwd") {
            if (i + 1 < argc) {
                args.http_passwd = argv[++i];
            }
        } else if (arg == "--proxy-user") {
            if (i + 1 < argc) {
                args.proxy_user = argv[++i];
            }
        } else if (arg == "--proxy-passwd") {
            if (i + 1 < argc) {
                args.proxy_passwd = argv[++i];
            }
        } else if (arg == "--use-head") {
            args.use_head = true;
        } else if (arg == "--conditional-download") {
            args.conditional_download = true;
        } else if (arg == "--auto-file-renaming") {
            args.auto_renaming = true;
        } else if (arg == "--rpc-secret") {
            if (i + 1 < argc) {
                args.rpc_secret = argv[++i];
            }
        } else if (arg == "--rpc-listen-port") {
            if (i + 1 < argc) {
                args.rpc_listen_port = std::stoi(argv[++i]);
            }
        } else if (arg == "--rpc-allow-origin-all") {
            args.rpc_allow_origin_all = true;
        } else if (arg == "--config" || arg == "-C") {
            if (i + 1 < argc) {
                args.config_file = argv[++i];
            }
        } else if (arg == "--show-config-path") {
            args.show_config_path = true;
        } else if (arg == "--create-default-config") {
            args.create_default_config = true;
        } else if (arg == "--no-color") {
            args.no_color = true;
        } else if (!arg.empty() && arg[0] != '-') {
            args.urls.push_back(arg);
        }
    }

    return args;
}

} // namespace falcon::cli
