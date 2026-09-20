#pragma once

#include <chrono>
#include <map>
#include <string>

namespace falcon {

/// Download configuration options
struct DownloadOptions {
    /// Maximum number of concurrent connections for segmented download
    std::size_t max_connections = 4;

    /// Connection timeout in seconds
    std::size_t timeout_seconds = 30;

    /// Maximum retry attempts on failure
    std::size_t max_retries = 3;

    /// Retry delay in seconds (doubles after each retry)
    std::size_t retry_delay_seconds = 1;

    /// Output directory for downloaded files
    std::string output_directory = ".";

    /// Output filename (empty = auto-detect from URL/headers)
    std::string output_filename;

    /// Speed limit per task in bytes/second (0 = unlimited)
    std::size_t speed_limit = 0;

    /// Enable resume if partial file exists
    bool resume_enabled = true;

    /// User agent string for HTTP requests
    std::string user_agent = "Falcon/0.1.0";

    /// Custom HTTP headers
    std::map<std::string, std::string> headers;

    /// Proxy server URL (e.g., "http://proxy:8080", "socks5://proxy:1080")
    std::string proxy;

    /// Proxy username (optional)
    std::string proxy_username;

    /// Proxy password (optional)
    std::string proxy_password;

    /// Proxy type override: "http", "https", "socks4", "socks5"
    /// If empty, will be auto-detected from proxy URL
    std::string proxy_type;

    /// Verify SSL certificates (HTTPS)
    bool verify_ssl = true;

    /// HTTP Referer header value (optional)
    std::string referer;

    /// Load cookies from file (libcurl CURLOPT_COOKIEFILE)
    std::string cookie_file;

    /// Save cookies to file (libcurl CURLOPT_COOKIEJAR)
    std::string cookie_jar;

    /// HTTP authentication username (optional)
    std::string http_username;

    /// HTTP authentication password (optional)
    std::string http_password;

    /// Client certificate file path for mutual TLS (PEM; aria2
    /// --certificate). Empty = no client certificate.
    std::string client_certificate;

    /// Client private key file path for mutual TLS (PEM; aria2
    /// --private-key). May point to the same combined PEM file as
    /// client_certificate.
    std::string client_private_key;

    /// Minimum segment size for multi-connection download (bytes)
    std::size_t min_segment_size = 1024 * 1024;  // 1 MB

    /// Enable adaptive segment sizing for segmented download
    bool adaptive_segment_sizing = true;

    /// Progress callback interval in milliseconds
    std::size_t progress_interval_ms = 500;

    /// Create output directory if not exists
    bool create_directory = true;

    /// Overwrite existing file
    bool overwrite_existing = false;

    /// Auto-rename output when the target file already exists instead of
    /// failing: a number (1..9999) is inserted before the extension
    /// ("file.zip" -> "file.1.zip", aria2 --auto-file-renaming). Only
    /// applies to self-derived output paths; explicit overwrite_existing
    /// takes precedence.
    bool auto_file_renaming = false;

    /// Conditional download: when the output file already exists, send
    /// If-Modified-Since with its modification time and treat a 304
    /// response as success keeping the local file (aria2
    /// --conditional-get). Implies overwrite authorization for the
    /// existing file (auto-renaming a "new" path has nothing to be
    /// conditional about), so it suppresses auto_file_renaming.
    bool conditional_get = false;
};

/// Global engine configuration
struct EngineConfig {
    /// Maximum concurrent download tasks
    std::size_t max_concurrent_tasks = 5;

    /// Global speed limit in bytes/second (0 = unlimited)
    std::size_t global_speed_limit = 0;

    /// Log level: 0=off, 1=error, 2=warn, 3=info, 4=debug, 5=trace
    int log_level = 3;
};

}  // namespace falcon
