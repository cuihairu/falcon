# HTTP/HTTPS 协议

HTTP/HTTPS 是 Falcon 当前最稳定、默认启用的下载协议。

## 特性

- ✅ 断点续传
- ✅ 多连接分块下载
- ✅ 自定义请求头
- ✅ 代理支持
- ✅ HTTP 认证
- ✅ SSL/TLS 校验开关

## 命令行使用

### 基本下载

```bash
falcon-cli http://example.com/file.zip
falcon-cli https://example.com/file.zip
```

### 多连接下载

```bash
falcon-cli https://example.com/large.zip -c 5
falcon-cli https://example.com/very-large.zip -x 16 -s 16
```

### 断点续传

```bash
falcon-cli https://example.com/file.zip --continue true
```

### 自定义头部与认证

```bash
falcon-cli https://example.com/file.zip \
  --user-agent "MyApp/2.0" \
  --header "Authorization: Bearer TOKEN" \
  --http-user user \
  --http-passwd pass
```

### 代理

```bash
falcon-cli https://example.com/file.zip \
  --proxy http://proxy.example.com:8080 \
  --proxy-user user \
  --proxy-passwd pass
```

## C++ API

当前公共 API 通过 `DownloadOptions` 配置 HTTP 下载，不暴露独立的 `HttpOptions` 结构体。

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.output_filename = "file.zip";
    options.user_agent = "MyApp/2.0";
    options.headers["Authorization"] = "Bearer TOKEN";
    options.proxy = "http://proxy.example.com:8080";
    options.proxy_username = "user";
    options.proxy_password = "pass";
    options.verify_ssl = true;
    options.resume_enabled = true;

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## 事件监听

```cpp
class ProgressListener : public falcon::IEventListener {
public:
    void on_progress(const falcon::ProgressInfo& info) override {
        std::cout << "\r"
                  << std::fixed << std::setprecision(1)
                  << info.progress * 100.0f << "%";
        std::cout.flush();
    }

    void on_completed(falcon::TaskId, const std::string& output_path) override {
        std::cout << "\n完成: " << output_path << std::endl;
    }
};
```

## 相关配置字段

HTTP/HTTPS 下载常用的是这些 `DownloadOptions` 字段：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| `max_connections` | `4` | 最大连接数 |
| `timeout_seconds` | `30` | 请求超时 |
| `max_retries` | `3` | 最大重试次数 |
| `retry_delay_seconds` | `1` | 重试间隔 |
| `speed_limit` | `0` | 单任务限速 |
| `resume_enabled` | `true` | 启用断点续传 |
| `user_agent` | `Falcon/0.1.0` | User-Agent |
| `headers` | `{}` | 自定义头部 |
| `proxy` | `""` | 代理 URL |
| `http_username` | `""` | HTTP 用户名 |
| `http_password` | `""` | HTTP 密码 |
| `verify_ssl` | `true` | 是否校验证书 |

## 常见问题

### Q: 为什么多连接下载反而变慢？

A: 服务器可能限制了单 IP 并发，或者文件本身太小。可以尝试降低 `-c` / `-x` 的值。

### Q: SSL 证书验证失败？

A: 如果只是临时测试，可以关闭验证：

```cpp
falcon::DownloadOptions options;
options.verify_ssl = false;
```

::: tip 建议
生产环境默认保持 `verify_ssl = true`。
:::
