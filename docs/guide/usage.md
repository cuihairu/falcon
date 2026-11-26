# Usage Guide

This document covers how to use the Falcon downloader in detail, including the command-line tool and the C++ API.

## Table of Contents

- [Command-Line Tool](#command-line-tool)
- [C++ API](#c-api)
- [Advanced Usage](#advanced-usage)

## Command-Line Tool

### Basic Syntax

```bash
falcon-cli [OPTIONS] URL
```

### Common Options

| Option | Short | Description | Example |
|--------|-------|-------------|---------|
| `--output` | `-o` | Specify the output file name | `-o file.zip` |
| `--directory` | `-d` | Specify the save directory | `-d ~/Downloads` |
| `--connections` | `-c` | Number of connections (multi-threaded download) | `-c 5` |
| `--max-connections` | `-x` | aria2-style alias of `--connections` | `-x 5` |
| `--max-concurrent-downloads` | `-j` | Maximum number of concurrent download tasks | `-j 3` |
| `--limit` |  | Speed limit (bytes/second) | `--limit 1M` |
| `--continue` | | Resume interrupted downloads | `--continue` |
| `--proxy` | | Proxy server | `--proxy http://proxy:8080` |
| `--verbose` | `-v` | Verbose output | `-v` |
| `--help` | `-h` | Show help | `-h` |
| `--version` | `-V` | Show version | `-V` |

### Basic Usage

#### Download a Single File

```bash
falcon-cli https://example.com/file.zip
```

#### Specify the Output File Name

```bash
falcon-cli https://example.com/file.zip -o my-file.zip
```

#### Specify the Save Directory

```bash
falcon-cli https://example.com/file.zip -d ~/Downloads
```

#### Multi-Threaded Download

```bash
# Download with 5 concurrent connections
falcon-cli https://example.com/large.zip -c 5
```

#### Speed Limit

```bash
# Limit the speed to 1MB/s
falcon-cli https://example.com/file.zip --limit 1M

# Limit the speed to 500KB/s
falcon-cli https://example.com/file.zip --limit 500K
```

#### Resume

```bash
# Automatic resume (enabled by default)
falcon-cli https://example.com/file.zip --continue
```

#### Using a Proxy

```bash
# HTTP proxy
falcon-cli https://example.com/file.zip --proxy http://proxy:8080

# SOCKS5 proxy
falcon-cli https://example.com/file.zip --proxy socks5://proxy:1080

# Proxy with authentication
falcon-cli https://example.com/file.zip --proxy http://user:pass@proxy:8080
```

### Downloading Different Protocols

#### HTTP/HTTPS

```bash
falcon-cli https://example.com/file.zip

# Custom User-Agent
falcon-cli https://example.com/file.zip --user-agent "MyApp/2.0"

# Add custom headers
falcon-cli https://example.com/file.zip --header "Authorization: Bearer TOKEN"
```

#### FTP

```bash
# Anonymous download
falcon-cli ftp://ftp.example.com/file.zip

# With username and password
falcon-cli ftp://user:password@ftp.example.com/file.zip

# FTPS
falcon-cli ftps://ftp.example.com/file.zip
```

#### Thunder Links

```bash
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# Thunder offline link
falcon-cli "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
```

#### QQ Whirlwind Links

```bash
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# Link with GID
falcon-cli "qqlink://1234567890ABCDEF|video.mp4"
```

#### FlashGet Links

```bash
falcon-cli "flashget://[URL]&ref=http://example.com"

# Base64 format
falcon-cli "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw"
```

#### ED2K Links

```bash
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# Link with sources
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|s,192.168.1.1:4662"
```

#### BitTorrent

```bash
# Magnet link
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# Torrent file
falcon-cli https://example.com/file.torrent

# Local torrent file
falcon-cli file:///path/to/file.torrent
```

#### HLS Streaming

```bash
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# Automatically pick the best quality
falcon-cli "https://example.com/master.m3u8" -o best.mp4
```

### Batch Downloads

#### Read URLs from a File

Create a text file `urls.txt`:

```
# Comment lines start with #
https://example.com/file1.zip
https://example.com/file2.zip
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTMuemlwWg==
```

Then download in batch:

```bash
falcon-cli -i urls.txt
```

#### Concurrency Control

```bash
# At most 3 concurrent downloads
falcon-cli -i urls.txt -j 3
```

### Progress Display

#### Verbose Output

```bash
falcon-cli https://example.com/file.zip --verbose
```

Sample output:

```
[INFO] Download started: https://example.com/file.zip
[INFO] File size: 104.85 MB
[INFO] Connections: 1
[========------------------] 25% (26.21 MB / 104.85 MB, 2.5 MB/s, ETA 32s)
[========================] 100% (104.85 MB / 104.85 MB, 3.2 MB/s, done)
[INFO] Download complete: /path/to/file.zip
```

## C++ API

### Basic Usage

```cpp
#include <falcon/falcon.hpp>

int main() {
    // Create the download engine
    falcon::DownloadEngine engine;

    // Configure download options
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.speed_limit = 1024 * 1024;  // 1MB/s

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());

    // Wait for the download to finish
    task->wait();

    if (task->status() == falcon::TaskStatus::Completed) {
        std::cout << "Download complete!" << std::endl;
    }

    return 0;
}
```

### Event Listening

```cpp
#include <falcon/falcon.hpp>

class MyEventListener : public falcon::IEventListener {
public:
    void on_progress(const falcon::ProgressInfo& info) override {
        double percent = info.progress * 100.0;
        std::cout << "\rProgress: " << percent << "%";
    }

    void on_completed(falcon::TaskId, const std::string& output_path) override {
        std::cout << "\nDownload complete: " << output_path << std::endl;
    }

    void on_error(falcon::TaskId, const std::string& error_message) override {
        std::cerr << "\nDownload failed: " << error_message << std::endl;
    }
};

int main() {
    falcon::DownloadEngine engine;

    MyEventListener listener;
    falcon::DownloadOptions options;
    engine.add_listener(&listener);

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    engine.remove_listener(&listener);
    return 0;
}
```

### Multi-Task Downloads

```cpp
#include <falcon/falcon.hpp>
#include <vector>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.max_connections = 3;

    std::vector<std::string> urls = {
        "https://example.com/file1.zip",
        "https://example.com/file2.zip",
        "https://example.com/file3.zip",
    };

    auto tasks = engine.add_tasks(urls, options);

    for (const auto& task : tasks) {
        engine.start_task(task->id());
    }

    // Wait for all downloads to finish
    engine.wait_all();

    return 0;
}
```

### Resume

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.resume_enabled = true;
    options.output_filename = "file.zip";
    options.output_directory = "./downloads";

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## Advanced Usage

### Speed Limit

```cpp
falcon::DownloadOptions options;
options.speed_limit = 1024 * 1024;  // 1MB/s
```

### Custom HTTP Headers

```cpp
falcon::DownloadOptions options;
options.user_agent = "MyApp/2.0";
options.headers = {
    {"Authorization", "Bearer TOKEN"},
    {"Accept", "application/json"}
};
```

### Proxy Settings

```cpp
falcon::DownloadOptions options;
options.proxy = "http://proxy.example.com:8080";
options.proxy_username = "user";
options.proxy_password = "pass";
```

### Timeout Settings

```cpp
falcon::DownloadOptions options;
options.timeout_seconds = 30;
```

### Retry Policy

```cpp
falcon::DownloadOptions options;
options.max_retries = 5;
options.retry_delay_seconds = 10;
```

## Compiling Your Program

```bash
# Assume Falcon is installed in /usr/local
g++ -std=c++17 \
    -I/usr/local/include/falcon \
    -L/usr/local/lib \
    -o my_app \
    main.cpp \
    -lfalcon
```

::: tip Tip
For more sample code, see the examples directory in the [GitHub repository](https://github.com/cuihairu/falcon/tree/main/examples).
:::
