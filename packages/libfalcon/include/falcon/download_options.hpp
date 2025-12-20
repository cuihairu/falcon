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

    /// Verify SSL certificates (HTTPS)
    bool verify_ssl = true;

    /// Minimum segment size for multi-connection download (bytes)
    std::size_t min_segment_size = 1024 * 1024;  // 1 MB

    /// Progress callback interval in milliseconds
    std::size_t progress_interval_ms = 500;

    /// Create output directory if not exists
    bool create_directory = true;

    /// Overwrite existing file
    bool overwrite_existing = false;
};

/// Global engine configuration
struct EngineConfig {
    /// Maximum concurrent download tasks
    std::size_t max_concurrent_tasks = 5;

    /// Global speed limit in bytes/second (0 = unlimited)
    std::size_t global_speed_limit = 0;

    /// Enable disk cache for better write performance
    bool enable_disk_cache = true;

    /// Disk cache size in bytes (per task)
    std::size_t disk_cache_size = 4 * 1024 * 1024;  // 4 MB

    /// Log level: 0=off, 1=error, 2=warn, 3=info, 4=debug, 5=trace
    int log_level = 3;

    /// Temporary file extension during download
    std::string temp_extension = ".falcon.tmp";

    /// Auto-start queued tasks when slot available
    bool auto_start = true;
};

}  // namespace falcon
