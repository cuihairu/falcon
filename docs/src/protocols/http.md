# HTTP/HTTPS 协议

HTTP/HTTPS 是最常用的下载协议，Falcon 提供了全面的支持。

## 特性

- ✅ **断点续传**：自动检测服务器支持
- ✅ **多线程下载**：分块并发下载，提高速度
- ✅ **速度控制**：精确的上传/下载速度限制
- ✅ **自定义头部**：User-Agent、Authorization 等
- ✅ **代理支持**：HTTP/HTTPS/SOCKS 代理
- ✅ **认证支持**：Basic、Digest 认证
- ✅ **压缩支持**：自动解压 gzip、deflate
- ✅ **SSL/TLS**：完整的 HTTPS 支持

## 命令行使用

### 基本下载

```bash
# HTTP 下载
falcon-cli http://example.com/file.zip

# HTTPS 下载
falcon-cli https://example.com/file.zip
```

### 多线程下载

```bash
# 使用 5 个连接同时下载
falcon-cli https://example.com/large.zip -c 5

# 使用 16 个连接（适合大文件）
falcon-cli https://example.com/very-large.zip -c 16
```

### 断点续传

```bash
# 自动断点续传（默认启用）
falcon-cli https://example.com/file.zip --continue

# 下载中断后再次运行相同命令即可继续
```

### 速度限制

```bash
# 限制下载速度为 1MB/s
falcon-cli https://example.com/file.zip --limit 1M

# 限制为 500KB/s
falcon-cli https://example.com/file.zip --limit 500K

# 限制为特定字节数
falcon-cli https://example.com/file.zip --limit 1048576
```

### 自定义 HTTP 头部

```bash
# 自定义 User-Agent
falcon-cli https://example.com/file.zip --user-agent "MyApp/2.0"

# 添加 Authorization 头部
falcon-cli https://example.com/file.zip --header "Authorization: Bearer TOKEN"

# 添加多个自定义头部
falcon-cli https://example.com/file.zip \
  --header "Accept: application/json" \
  --header "X-Custom-Header: value"
```

### 代理设置

```bash
# HTTP 代理
falcon-cli https://example.com/file.zip --proxy http://proxy:8080

# HTTPS 代理
falcon-cli https://example.com/file.zip --proxy https://proxy:8443

# SOCKS5 代理
falcon-cli https://example.com/file.zip --proxy socks5://proxy:1080

# 带认证的代理
falcon-cli https://example.com/file.zip --proxy http://user:pass@proxy:8080
```

### 认证

```bash
# Basic 认证
falcon-cli https://example.com/file.zip --user username:password

# Digest 认证（自动检测）
falcon-cli https://user:password@example.com/file.zip
```

### 输出控制

```bash
# 指定输出文件名
falcon-cli https://example.com/file.zip -o my-file.zip

# 指定保存目录
falcon-cli https://example.com/file.zip -d ~/Downloads

# 同时指定文件名和目录
falcon-cli https://example.com/file.zip -o my-file.zip -d ~/Downloads

# 不覆盖已存在的文件
falcon-cli https://example.com/file.zip --no-clobber
```

## C++ API 使用

### 基本下载

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    falcon::DownloadOptions options;
    options.max_connections = 5;

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

### 自定义 HTTP 选项

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    falcon::HttpOptions http_options;
    http_options.user_agent = "MyApp/2.0";
    http_options.headers = {
        {"Authorization", "Bearer TOKEN"},
        {"Accept", "application/json"}
    };
    http_options.timeout = std::chrono::seconds(30);
    http_options.verify_ssl = true;

    falcon::DownloadOptions options;
    options.http_options = http_options;
    options.max_connections = 5;

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

### 代理设置

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    falcon::ProxyInfo proxy;
    proxy.type = falcon::ProxyType::HTTP;
    proxy.host = "proxy.example.com";
    proxy.port = 8080;
    proxy.username = "user";
    proxy.password = "pass";

    falcon::DownloadOptions options;
    options.proxy = proxy;

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

### 断点续传

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    falcon::DownloadOptions options;
    options.resume = true;
    options.output_file = "./downloads/file.zip";

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

### 速度限制

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    falcon::DownloadOptions options;
    options.speed_limit = 1024 * 1024;  // 1MB/s

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

## 进度监听

```cpp
#include <falcon/falcon.hpp>
#include <iostream>

class ProgressListener : public falcon::IEventListener {
public:
    void onProgress(const falcon::ProgressEvent& event) override {
        double percent = (double)event.downloaded_bytes / event.total_bytes * 100.0;
        double speed = event.download_speed / 1024.0 / 1024.0;  // MB/s

        std::cout << "\r["
                  << std::string(static_cast<int>(percent / 2), '=')
                  << std::string(50 - static_cast<int>(percent / 2), ' ')
                  << "] "
                  << std::fixed << std::setprecision(1) << percent
                  << "% (" << speed << " MB/s)"
                  << std::flush;
    }

    void onComplete(const falcon::CompleteEvent& event) override {
        std::cout << "\n下载完成!" << std::endl;
    }
};

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    ProgressListener listener;
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

## 配置选项

### HTTP 选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `user_agent` | string | "Falcon/1.0" | HTTP User-Agent |
| `headers` | map<string, string> | {} | 自定义 HTTP 头部 |
| `timeout` | chrono::seconds | 30 | 请求超时时间 |
| `connect_timeout` | chrono::seconds | 10 | 连接超时时间 |
| `verify_ssl` | bool | true | 验证 SSL 证书 |
| `compression` | bool | true | 启用压缩 |
| `max_redirects` | int | 5 | 最大重定向次数 |
| `retry_on_failure` | bool | true | 失败时自动重试 |

### 下载选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `max_connections` | int | 1 | 最大连接数 |
| `speed_limit` | int64_t | 0 | 速度限制（字节/秒） |
| `resume` | bool | true | 启用断点续传 |
| `output_file` | string | "" | 输出文件路径 |
| `output_directory` | string | "." | 输出目录 |

## 常见问题

### Q: 为什么多线程下载反而变慢了？

A: 服务器可能限制了单个 IP 的连接数。尝试减少连接数，或服务器本身带宽有限。

### Q: 断点续传不工作？

A: 确保服务器支持 Range 请求。可以使用 `--verbose` 选项查看详细信息。

### Q: SSL 证书验证失败？

A: 如果您信任该服务器，可以临时禁用 SSL 验证（不推荐）：

```cpp
falcon::HttpOptions http_options;
http_options.verify_ssl = false;
```

::: tip 最佳实践
- **小文件**（< 10MB）：使用单连接
- **中等文件**（10MB - 1GB）：使用 3-5 个连接
- **大文件**（> 1GB）：使用 8-16 个连接
:::
