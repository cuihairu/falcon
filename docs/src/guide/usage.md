# 使用指南

本文档详细介绍 Falcon 下载器的使用方法，包括命令行工具和 C++ API。

## 目录

- [命令行工具](#命令行工具)
- [C++ API](#c-api)
- [高级用法](#高级用法)

## 命令行工具

### 基本语法

```bash
falcon-cli [OPTIONS] URL
```

### 常用选项

| 选项 | 简写 | 说明 | 示例 |
|------|------|------|------|
| `--output` | `-o` | 指定输出文件名 | `-o file.zip` |
| `--directory` | `-d` | 指定保存目录 | `-d ~/Downloads` |
| `--connections` | `-c` | 连接数（多线程下载） | `-c 5` |
| `--limit` | `-l` | 速度限制（字节/秒） | `-l 1M` |
| `--continue` | | 断点续传 | `--continue` |
| `--proxy` | | 代理服务器 | `--proxy http://proxy:8080` |
| `--verbose` | `-v` | 详细输出 | `-v` |
| `--help` | `-h` | 显示帮助 | `-h` |
| `--version` | `-V` | 显示版本 | `-V` |

### 基本使用

#### 下载单个文件

```bash
falcon-cli https://example.com/file.zip
```

#### 指定输出文件名

```bash
falcon-cli https://example.com/file.zip -o my-file.zip
```

#### 指定保存目录

```bash
falcon-cli https://example.com/file.zip -d ~/Downloads
```

#### 多线程下载

```bash
# 使用 5 个连接同时下载
falcon-cli https://example.com/large.zip -c 5
```

#### 速度限制

```bash
# 限制速度为 1MB/s
falcon-cli https://example.com/file.zip --limit 1M

# 限制速度为 500KB/s
falcon-cli https://example.com/file.zip --limit 500K
```

#### 断点续传

```bash
# 自动断点续传（默认启用）
falcon-cli https://example.com/file.zip --continue
```

#### 使用代理

```bash
# HTTP 代理
falcon-cli https://example.com/file.zip --proxy http://proxy:8080

# SOCKS5 代理
falcon-cli https://example.com/file.zip --proxy socks5://proxy:1080

# 带认证的代理
falcon-cli https://example.com/file.zip --proxy http://user:pass@proxy:8080
```

### 下载不同协议

#### HTTP/HTTPS

```bash
falcon-cli https://example.com/file.zip

# 自定义 User-Agent
falcon-cli https://example.com/file.zip --user-agent "MyApp/2.0"

# 添加自定义头部
falcon-cli https://example.com/file.zip --header "Authorization: Bearer TOKEN"
```

#### FTP

```bash
# 匿名下载
falcon-cli ftp://ftp.example.com/file.zip

# 带用户名密码
falcon-cli ftp://user:password@ftp.example.com/file.zip

# FTPS（显式 TLS）
falcon-cli ftpes://ftp.example.com/file.zip
```

#### 迅雷链接

```bash
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# 迅雷离线链接
falcon-cli "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
```

#### QQ旋风链接

```bash
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# 带 GID 的链接
falcon-cli "qqlink://1234567890ABCDEF|video.mp4"
```

#### 快车链接

```bash
falcon-cli "flashget://[URL]&ref=http://example.com"

# Base64 格式
falcon-cli "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw"
```

#### ED2K 链接

```bash
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# 带源的链接
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|s,192.168.1.1:4662"
```

#### BitTorrent

```bash
# Magnet 链接
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# Torrent 文件
falcon-cli https://example.com/file.torrent

# 本地 torrent 文件
falcon-cli file:///path/to/file.torrent
```

#### HLS 流媒体

```bash
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# 自动选择最佳质量
falcon-cli "https://example.com/master.m3u8" -o best.mp4
```

### 批量下载

#### 从文件读取 URL

创建一个文本文件 `urls.txt`：

```
# 注释行以 # 开头
https://example.com/file1.zip
https://example.com/file2.zip
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTMuemlwWg==
```

然后批量下载：

```bash
falcon-cli -i urls.txt
```

#### 并发控制

```bash
# 最多 3 个并发下载
falcon-cli -i urls.txt --max-concurrent 3
```

### 进度显示

#### 详细输出

```bash
falcon-cli https://example.com/file.zip --verbose
```

输出示例：

```
[INFO] 开始下载: https://example.com/file.zip
[INFO] 文件大小: 104.85 MB
[INFO] 连接数: 1
[========------------------] 25% (26.21 MB / 104.85 MB, 2.5 MB/s, ETA 32s)
[========================] 100% (104.85 MB / 104.85 MB, 3.2 MB/s, 完成)
[INFO] 下载完成: /path/to/file.zip
```

## C++ API

### 基本用法

```cpp
#include <falcon/falcon.hpp>

int main() {
    // 创建下载引擎
    falcon::DownloadEngine engine;

    // 加载所有插件
    engine.loadAllPlugins();

    // 配置下载选项
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.speed_limit = 1024 * 1024;  // 1MB/s

    // 开始下载
    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    // 等待下载完成
    task->wait();

    if (task->getStatus() == falcon::TaskStatus::Completed) {
        std::cout << "下载完成!" << std::endl;
    }

    return 0;
}
```

### 事件监听

```cpp
#include <falcon/falcon.hpp>

class MyEventListener : public falcon::IEventListener {
public:
    void onProgress(const falcon::ProgressEvent& event) override {
        double percent = (double)event.downloaded_bytes / event.total_bytes * 100;
        std::cout << "\r进度: " << percent << "%";
    }

    void onComplete(const falcon::CompleteEvent& event) override {
        std::cout << "\n下载完成!" << std::endl;
    }

    void onError(const falcon::ErrorEvent& event) override {
        std::cerr << "\n下载失败: " << event.error_message << std::endl;
    }
};

int main() {
    falcon::DownloadEngine engine;
    engine.loadAllPlugins();

    MyEventListener listener;
    falcon::DownloadOptions options;

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options,
        &listener
    );

    task->wait();
    return 0;
}
```

### 多任务下载

```cpp
#include <falcon/falcon.hpp>
#include <vector>

int main() {
    falcon::DownloadEngine engine;
    engine.loadAllPlugins();

    falcon::DownloadOptions options;
    options.max_connections = 3;

    std::vector<std::string> urls = {
        "https://example.com/file1.zip",
        "https://example.com/file2.zip",
        "https://example.com/file3.zip",
    };

    std::vector<std::shared_ptr<falcon::DownloadTask>> tasks;

    for (const auto& url : urls) {
        auto task = engine.startDownload(url, options);
        tasks.push_back(task);
    }

    // 等待所有下载完成
    for (auto& task : tasks) {
        task->wait();
    }

    return 0;
}
```

### 断点续传

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadAllPlugins();

    falcon::DownloadOptions options;
    options.resume = true;  // 启用断点续传
    options.output_file = "./downloads/file.zip";

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

## 高级用法

### 速度限制

```cpp
falcon::DownloadOptions options;
options.speed_limit = 1024 * 1024;  // 1MB/s
```

### 自定义 HTTP 头部

```cpp
falcon::HttpOptions http_options;
http_options.headers = {
    {"User-Agent", "MyApp/2.0"},
    {"Authorization", "Bearer TOKEN"},
    {"Accept", "application/json"}
};

falcon::DownloadOptions options;
options.http_options = http_options;
```

### 代理设置

```cpp
falcon::DownloadOptions options;
options.proxy = falcon::ProxyInfo{
    .type = falcon::ProxyType::HTTP,
    .host = "proxy.example.com",
    .port = 8080,
    .username = "user",
    .password = "pass"
};
```

### 超时设置

```cpp
falcon::DownloadOptions options;
options.connect_timeout = std::chrono::seconds(30);
options.read_timeout = std::chrono::seconds(60);
```

### 重试策略

```cpp
falcon::DownloadOptions options;
options.max_retries = 5;
options.retry_delay = std::chrono::seconds(10);
```

## 编译示例程序

```bash
# 假设 Falcon 安装在 /usr/local
g++ -std=c++17 \
    -I/usr/local/include/falcon \
    -L/usr/local/lib \
    -o my_app \
    main.cpp \
    -lfalcon
```

::: tip 提示
更多示例代码请查看 [GitHub 仓库](https://github.com/yourusername/falcon/tree/main/examples) 中的 examples 目录。
:::
