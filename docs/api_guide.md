[中文](api_guide.zh.md) | **English**

# Falcon API Guide

> [!WARNING]
> This document is legacy API design and migration material.
> For the currently maintained API documentation, refer to the pages under `docs/src/api/`.

## Quick Start

### Basic Download

```cpp
#include <falcon/download_engine_v2.hpp>
#include <falcon/download_options.hpp>

using namespace falcon;

int main() {
    // 1. Create the engine configuration
    EngineConfigV2 config;
    config.max_concurrent_tasks = 5;
    config.global_speed_limit = 0;  // No limit

    // 2. Create the download engine
    DownloadEngineV2 engine(config);

    // 3. Configure the download options
    DownloadOptions options;
    options.max_connections = 4;  // Download 4 segments concurrently
    options.output_directory = "/tmp/downloads";
    options.timeout_seconds = 30;

    // 4. Add the download task
    TaskId task_id = engine.add_download(
        "https://example.com/large_file.zip",
        options
    );

    // 5. Start the event loop (blocks until completion)
    engine.run();

    // 6. Get statistics
    auto stats = engine.get_statistics();
    std::cout << "Download complete: " << stats.total_downloaded << " bytes\n";

    return 0;
}
```

## Detailed API Reference

### DownloadEngineV2 - Download Engine

#### Creating the Engine

```cpp
// With default configuration
DownloadEngineV2 engine;

// With custom configuration
EngineConfigV2 config;
config.max_concurrent_tasks = 10;
config.poll_timeout_ms = 50;
config.enable_disk_cache = true;
config.disk_cache_size = 8 * 1024 * 1024;  // 8 MB

DownloadEngineV2 engine(config);
```

#### Adding Download Tasks

```cpp
// Single-URL download
TaskId id1 = engine.add_download(
    "http://example.com/file.zip",
    options
);

// Multi-mirror download (automatic failover)
std::vector<std::string> urls = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://mirror3.example.com/file.zip"
};
TaskId id2 = engine.add_download(urls, options);
```

#### Task Control

```cpp
// Pause a task
engine.pause_task(task_id);

// Resume a task
engine.resume_task(task_id);

// Cancel a task
engine.cancel_task(task_id);

// Bulk operations
engine.pause_all();
engine.resume_all();
engine.cancel_all();
```

#### Statistics

```cpp
auto stats = engine.get_statistics();
std::cout << "Active tasks: " << stats.active_tasks << "\n";
std::cout << "Waiting tasks: " << stats.waiting_tasks << "\n";
std::cout << "Completed: " << stats.completed_tasks << "\n";
std::cout << "Download speed: " << (stats.global_download_speed / 1024) << " KB/s\n";
std::cout << "Total downloaded: " << (stats.total_downloaded / 1024 / 1024) << " MB\n";
```

### DownloadOptions - Download Options

#### Basic Options

```cpp
DownloadOptions options;

// Concurrency settings
options.max_connections = 8;              // Up to 8 segments per HTTP connection
options.min_segment_size = 1024 * 1024;   // Minimum segment size of 1 MB

// Timeout settings
options.timeout_seconds = 30;             // Stall watchdog: aborts only when sustained < 1 B/s for 30 s (not a total-duration cap)

// Retry settings
options.max_retries = 5;                  // Retry up to 5 times
options.retry_delay_seconds = 5;          // Wait 5 seconds between retries

// Output settings
options.output_directory = "/tmp/downloads";
options.output_filename = "custom_name.zip";
options.overwrite_existing = false;       // Do not overwrite existing files
```

#### HTTP Options

```cpp
// User-Agent
options.user_agent = "Falcon/1.0 (https://github.com/cuihairu/falcon)";

// Custom HTTP headers
options.headers = {
    {"Authorization", "Bearer token123"},
    {"Accept", "application/json"}
};

// Referrer
options.referer = "https://example.com";

// Authentication
options.http_username = "username";
options.http_password = "password";
```

#### Speed Limits

```cpp
// Per-task speed limit
options.speed_limit = 1024 * 1024;  // 1 MB/s

// Global speed limit (in the engine configuration)
EngineConfigV2 config;
config.global_speed_limit = 10 * 1024 * 1024;  // 10 MB/s
```

#### Resume Downloads

```cpp
options.resume_enabled = true;  // Resume automatically (default on; continues from the temp-file checkpoint)
```

### Command-Line Arguments

Falcon CLI supports aria2-compatible command-line arguments:

#### Basic Arguments

```bash
# Download a single file
falcon-cli https://example.com/file.zip

# Specify the output directory
falcon-cli -d /tmp/downloads https://example.com/file.zip

# Specify the output filename
falcon-cli -o custom_name.zip https://example.com/file.zip

# Limit concurrency
falcon-cli -x 8 -s 8 https://example.com/file.zip
# -x: Maximum connections per task (default: 4)
# -s: Alias for -x (aria2 --split, task-level connection count)
```

#### Progress Display

```bash
# Verbose output
falcon-cli -v https://example.com/file.zip

# Quiet mode (only errors and the final result)
falcon-cli -q https://example.com/file.zip

# Disable colored output
falcon-cli --no-color https://example.com/file.zip
```

#### Retries and Timeouts

```bash
# Set the retry count and wait time
falcon-cli -r 5 --retry-wait 10 https://example.com/file.zip

# Set the timeout
falcon-cli -t 30 https://example.com/file.zip
```

#### Speed Limits

```bash
# Limit the download speed
falcon-cli --max-download-limit 1M https://example.com/file.zip

# Set a global speed limit (daemon only: aria2.changeGlobalOption's max-overall-download-limit,
# or download.max_overall_speed_limit in daemon.json; the CLI has no such argument)
```

#### RPC Arguments (Implemented)

```bash
# Enable RPC
falcon-daemon --enable-rpc --rpc-listen-port 6800

# Set the RPC secret (clients must include "token:<secret>" in the JSON-RPC params)
falcon-daemon --enable-rpc --rpc-secret mysecrettoken

# Allow cross-origin requests
falcon-daemon --enable-rpc --rpc-allow-origin-all
```

### File Hash Verification

#### Computing File Hashes

```cpp
#include <falcon/file_hash.hpp>

// Compute a SHA256 hash
auto sha256_hash = FileHasher::calculate(
    "file.zip",
    HashAlgorithm::SHA256
);
std::cout << "SHA256: " << sha256_hash << std::endl;

// Compute other algorithms
auto md5_hash = FileHasher::calculate("file.zip", HashAlgorithm::MD5);
auto sha1_hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA1);
auto sha512_hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA512);
```

#### Verifying File Integrity

```cpp
// Verify a downloaded file
auto result = FileHasher::verify(
    "downloaded_file.zip",
    "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f",
    HashAlgorithm::SHA256
);

if (result.valid) {
    std::cout << "File verified\n";
} else {
    std::cout << "File verification failed!\n";
    std::cout << "Expected: " << result.expected << "\n";
    std::cout << "Actual: " << result.calculated << "\n";
}
```

#### Batch Verification

```cpp
std::vector<std::pair<std::string, HashAlgorithm>> hashes = {
    {"abc123...", HashAlgorithm::MD5},
    {"def456...", HashAlgorithm::SHA1},
    {"ghi789...", HashAlgorithm::SHA256}
};

auto results = FileHasher::verify_multiple("file.zip", hashes);

for (std::size_t i = 0; i < hashes.size(); ++i) {
    std::cout << "Expected: " << hashes[i].first
              << " Calculated: " << results[i].calculated
              << " Valid: " << results[i].valid << "\n";
}
```

#### Automatic Algorithm Detection

```cpp
// Automatically detect the algorithm from the hash length
std::string hash_str = "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f";
auto algorithm = FileHasher::detect_algorithm(hash_str);

if (algorithm == HashAlgorithm::SHA256) {
    std::cout << "Detected SHA256 hash\n";
}
```

### Socket Connection Pool

#### Creating a Connection Pool

```cpp
#include <falcon/net/socket_pool.hpp>

using namespace falcon::net;

// 30-second idle timeout, keep up to 16 idle connections
SocketPool pool(std::chrono::seconds(30), 16);
```

#### Acquiring and Releasing Connections

```cpp
// Define the connection key
SocketKey key;
key.host = "example.com";
key.port = 80;
key.username = "";  // HTTP basic authentication (optional)
key.proxy = "";    // Proxy address (optional)

// Acquire a connection
auto socket = pool.acquire(key);
if (socket && socket->is_valid()) {
    int fd = socket->fd();

    // Use the connection to send an HTTP request
    send_http_request(fd);

    // Return the connection to the pool
    pool.release(socket);
} else {
    // Create a new connection
    int fd = create_new_connection();
    auto new_socket = std::make_shared<PooledSocket>(fd, key);
    pool.release(new_socket);
}
```

### Event Polling

#### Creating an EventPoll

```cpp
#include <falcon/net/event_poll.hpp>

using namespace falcon::net;

// Automatically selects the optimal platform implementation
auto poll = EventPoll::create();
```

#### Registering Events

```cpp
// Register a read event
poll->add_event(socket_fd, static_cast<int>(IOEvent::READ),
    [](int fd, int events, void* user_data) {
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            // Handle the data
        }
    });

// Register a write event
poll->add_event(socket_fd, static_cast<int>(IOEvent::WRITE),
    [](int fd, int events, void* user_data) {
        const char* data = "GET / HTTP/1.1\r\n\r\n";
        send(fd, data, strlen(data), 0);
    });

// Register an error event
poll->add_event(socket_fd, static_cast<int>(IOEvent::ERR | IOEvent::HANGUP),
    [](int fd, int events, void* user_data) {
        close(fd);
    });
```

#### Modifying Events

```cpp
// Switch from a read event to a write event
poll->modify_event(socket_fd, static_cast<int>(IOEvent::WRITE));
```

#### Removing Events

```cpp
// Remove the event listener
poll->remove_event(socket_fd);
```

#### The Event Loop

```cpp
while (!should_stop) {
    // Wait for events (100 ms timeout)
    int num_events = poll->poll(100);

    if (num_events > 0) {
        // Handle ready events (processed automatically via callbacks)
    } else if (num_events == 0) {
        // Timed out; run periodic work
    } else {
        // Error
        std::cerr << "poll() error: " << poll->get_error() << std::endl;
        break;
    }
}
```

## Advanced Usage

### Concurrent Multi-Task Downloads

```cpp
DownloadEngineV2 engine;

std::vector<std::string> urls = {
    "https://example.com/file1.zip",
    "https://example.com/file2.zip",
    "https://example.com/file3.zip"
};

for (const auto& url : urls) {
    engine.add_download(url, options);
}

engine.run();
```

### Custom Progress Callbacks

```cpp
// TODO: Add a progress callback interface
// ProgressCallback callback = [](TaskId id, float progress) {
//     std::cout << "Task " << id << ": " << (progress * 100) << "%\n";
// };
// engine.set_progress_callback(callback);
```

### Mirror Failover

```cpp
std::vector<std::string> mirrors = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://backup.example.com/file.zip"
};

RequestGroup group(1, mirrors, options);

// Try the next mirror
while (!download_complete) {
    if (download_failed) {
        if (group.try_next_uri()) {
            // Retry with the next mirror
        } else {
            // All mirrors failed
            break;
        }
    }
}
```

## Error Handling

### Exception Handling

```cpp
try {
    engine.run();
} catch (const NetworkException& e) {
    std::cerr << "Network error: " << e.what() << std::endl;
} catch (const FileIOException& e) {
    std::cerr << "File error: " << e.what() << std::endl;
} catch (const std::exception& e) {
    std::cerr << "Unknown error: " << e.what() << std::endl;
}
```

### Log Configuration

```cpp
// TODO: Add a log configuration interface
// Logger::set_level(LogLevel::DEBUG);
// Logger::add_file_handler("/tmp/falcon.log");
```

## Performance Optimization Tips

### 1. Choose a Sensible Concurrency Level

```cpp
// Adjust based on network bandwidth
options.max_connections = 16;  // High-speed network
options.max_connections = 4;   // Slow network
```

### 2. Enable Connection Reuse

```cpp
// The connection pool reuses connections automatically; no extra configuration needed
SocketPool pool(std::chrono::seconds(30), 16);
```

### 3. Split Downloads into Segments

```cpp
// Enable multi-segment downloads for speed
options.min_segment_size = 1024 * 1024;  // Only split files larger than 1 MB
options.max_connections = 8;              // Up to 8 segment connections
```

### 4. Limit Speed

```cpp
// Avoid saturating the bandwidth
options.speed_limit = 5 * 1024 * 1024;  // Cap at 5 MB/s
```

## Frequently Asked Questions

### Q: How do I resume a download?

A: Falcon automatically detects the portion that has already been downloaded:

```cpp
options.resume_enabled = true;  // On by default
```

### Q: How do I download to a specific directory?

A: Set the output directory:

```cpp
options.output_directory = "/path/to/downloads";
```

### Q: How do I limit the download speed?

A: Use the speed limit option:

```cpp
options.speed_limit = 1024 * 1024;  // 1 MB/s
```

### Q: How do I verify file integrity?

A: Use file hash verification:

```cpp
auto result = FileHasher::verify("file.zip", expected_hash, HashAlgorithm::SHA256);
```

## Reference Documentation

- [Architecture documentation](aria2_architecture.md)
- [Migration plan](aria2c-migration-plan.md)
- [Protocol support](protocol_support.md)
- [Private protocols](private_protocols.md)
