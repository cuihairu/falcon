#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>

namespace falcon {

/// Download configuration options
struct DownloadOptions {
    /// Maximum number of concurrent connections for segmented download
    std::size_t max_connections = 4;

    /// Timeout in seconds, aria2 --timeout semantics: transfer-abort
    /// window when the connection stalls (V1: curl LOW_SPEED 1 B/s for
    /// this many seconds; V2: command wait timeout). NOT a cap on total
    /// transfer duration — slow-but-healthy downloads never time out.
    /// 0 disables the stall watchdog.
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

    /// aria2 --conditional-get 兼容字段。单一 V1（libcurl）引擎当前不
    /// 消费该选项：保留字段与持久化往返，避免破坏既有配置/任务格式；
    /// 不产生 If-Modified-Since 行为。
    bool conditional_get = false;

    /// aria2 --file-allocation 兼容字段（"none"/"trunc"/"falloc"/
    /// "prealloc"）。V1 引擎不消费，保留字段与持久化往返；新下载始终
    /// 按稀疏文件增长。
    std::string file_allocation = "none";

    /// BitTorrent seeding share ratio: keep seeding after download
    /// completes until uploaded/downloaded reaches this value
    /// (aria2 --seed-ratio). Default 1.0; 0 = stop seeding immediately
    /// on completion (task finishes as soon as the download does).
    /// Only consumed by BitTorrent handlers; other protocols ignore it.
    double seed_ratio = 1.0;

    /// BitTorrent seeding time limit in minutes: keep seeding after
    /// download completes for at most this long (aria2 --seed-time).
    /// Default 0 = no time limit (only seed_ratio applies). Seeding
    /// stops when EITHER limit is reached.
    std::size_t seed_time_minutes = 0;

    /// P2SP share tri-state (daemon SwarmAnnouncer consumer only):
    /// "" (default) follows the daemon global share setting,
    /// "true"/"false" explicitly override it for this download.
    /// Engines and other handlers ignore this field entirely.
    std::string p2sp_share;
};

/// Parse a byte-size string with an optional single-letter binary suffix
/// ("1048576", "1M", "512k", "2G", "1T"; suffix case-insensitive, powers
/// of 1024). Plain decimal digits only — no signs, whitespace, or
/// fractional values. Returns nullopt on empty input, a bare/unknown
/// suffix, any non-digit character, or overflow. Never throws.
/// Shared by the CLI size flags (which turn nullopt into a usage error)
/// and the daemon addUri "min-split-size" mapping (which turns nullopt
/// into -32602).
inline std::optional<std::uint64_t> parse_size_with_suffix(const std::string& text) {
    if (text.empty()) return std::nullopt;

    std::uint64_t multiplier = 1;
    std::string digits = text;
    const char suffix = digits.back();
    if (suffix == 'K' || suffix == 'k') {
        multiplier = 1024ULL;
        digits.pop_back();
    } else if (suffix == 'M' || suffix == 'm') {
        multiplier = 1024ULL * 1024ULL;
        digits.pop_back();
    } else if (suffix == 'G' || suffix == 'g') {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        digits.pop_back();
    } else if (suffix == 'T' || suffix == 't') {
        multiplier = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
        digits.pop_back();
    }
    if (digits.empty()) return std::nullopt;  // bare suffix ("M" alone)

    std::uint64_t value = 0;
    for (const char c : digits) {
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        // Base-10 accumulate: value * 10 + digit must not overflow uint64_t.
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    // Apply the suffix multiplier once, with its own overflow guard.
    if (value > std::numeric_limits<std::uint64_t>::max() / multiplier) {
        return std::nullopt;
    }
    return value * multiplier;
}

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
