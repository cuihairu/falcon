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
| `--max-connections` | `-x` | `--connections` 的 aria2 风格别名 | `-x 5` |
| `--max-concurrent-downloads` | `-j` | 最大并发下载任务数 | `-j 3` |
| `--limit` |  | 速度限制（字节/秒） | `--limit 1M` |
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
falcon-cli -i urls.txt -j 3
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

    // 配置下载选项
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.speed_limit = 1024 * 1024;  // 1MB/s

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());

    // 等待下载完成
    task->wait();

    if (task->status() == falcon::TaskStatus::Completed) {
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
    void on_progress(const falcon::ProgressInfo& info) override {
        double percent = info.progress * 100.0;
        std::cout << "\r进度: " << percent << "%";
    }

    void on_completed(falcon::TaskId, const std::string& output_path) override {
        std::cout << "\n下载完成: " << output_path << std::endl;
    }

    void on_error(falcon::TaskId, const std::string& error_message) override {
        std::cerr << "\n下载失败: " << error_message << std::endl;
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

### 多任务下载

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

    // 等待所有下载完成
    engine.wait_all();

    return 0;
}
```

### 断点续传

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

## 高级用法

### 速度限制

```cpp
falcon::DownloadOptions options;
options.speed_limit = 1024 * 1024;  // 1MB/s
```

### 自定义 HTTP 头部

```cpp
falcon::DownloadOptions options;
options.user_agent = "MyApp/2.0";
options.headers = {
    {"Authorization", "Bearer TOKEN"},
    {"Accept", "application/json"}
};
```

### 代理设置

```cpp
falcon::DownloadOptions options;
options.proxy = "http://proxy.example.com:8080";
options.proxy_username = "user";
options.proxy_password = "pass";
```

### 超时设置

```cpp
falcon::DownloadOptions options;
options.timeout_seconds = 30;
```

### 重试策略

```cpp
falcon::DownloadOptions options;
options.max_retries = 5;
options.retry_delay_seconds = 10;
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
更多示例代码请查看 [GitHub 仓库](https://github.com/cuihairu/falcon/tree/main/examples) 中的 examples 目录。
:::
