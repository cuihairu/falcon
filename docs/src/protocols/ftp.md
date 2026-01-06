# FTP/FTPS 协议

Falcon 支持完整的 FTP 和 FTPS（FTP over SSL/TLS）协议。

## 特性

- ✅ **主动模式**：PORT 方式
- ✅ **被动模式**：PASV 方式（默认）
- ✅ **FTPS**：显式 TLS (FTPES) 和隐式 TLS
- ✅ **认证**：用户名/密码登录
- ✅ **匿名登录**：支持匿名访问
- ✅ **断点续传**：支持续传中断的下载
- ✅ **目录列表**：浏览远程目录

## 命令行使用

### 基本下载

```bash
# 匿名 FTP 下载
falcon-cli ftp://ftp.example.com/file.zip

# 带用户名和密码
falcon-cli ftp://user:password@ftp.example.com/file.zip

# 指定端口
falcon-cli ftp://ftp.example.com:2121/file.zip
```

### FTPS（FTP over SSL/TLS）

```bash
# 显式 TLS（FTPES）
falcon-cli ftpes://ftp.example.com/file.zip

# 隐式 TLS
falcon-cli ftps://ftp.example.com/file.zip

# 带认证的 FTPS
falcon-cli ftpes://user:password@ftp.example.com/file.zip
```

### 主动/被动模式

```bash
# 使用被动模式（默认）
falcon-cli ftp://ftp.example.com/file.zip --passive

# 使用主动模式
falcon-cli ftp://ftp.example.com/file.zip --active
```

### 断点续传

```bash
falcon-cli ftp://ftp.example.com/large.zip --continue
```

### 使用代理

```bash
# HTTP 代理
falcon-cli ftp://ftp.example.com/file.zip --proxy http://proxy:8080

# SOCKS 代理
falcon-cli ftp://ftp.example.com/file.zip --proxy socks5://proxy:1080
```

## C++ API 使用

### 基本下载

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("ftp");

    falcon::FtpOptions ftp_options;
    ftp_options.username = "user";
    ftp_options.password = "password";
    ftp_options.passive_mode = true;

    falcon::DownloadOptions options;
    options.ftp_options = ftp_options;

    auto task = engine.startDownload(
        "ftp://ftp.example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

### FTPS 下载

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("ftp");

    falcon::FtpOptions ftp_options;
    ftp_options.username = "user";
    ftp_options.password = "password";
    ftp_options.ssl_mode = falcon::FtpSslMode::EXPLICIT;  // 或 IMPLICIT
    ftp_options.verify_ssl = true;

    falcon::DownloadOptions options;
    options.ftp_options = ftp_options;

    auto task = engine.startDownload(
        "ftpes://ftp.example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

### 主动模式

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("ftp");

    falcon::FtpOptions ftp_options;
    ftp_options.username = "user";
    ftp_options.password = "password";
    ftp_options.passive_mode = false;  // 主动模式

    falcon::DownloadOptions options;
    options.ftp_options = ftp_options;

    auto task = engine.startDownload(
        "ftp://ftp.example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

## FTP 选项

### 连接选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `username` | string | "anonymous" | 用户名 |
| `password` | string | "anonymous@" | 密码 |
| `passive_mode` | bool | true | 使用被动模式 |
| `ssl_mode` | enum | NONE | SSL 模式：NONE、EXPLICIT、IMPLICIT |
| `verify_ssl` | bool | true | 验证 SSL 证书 |

### 传输选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `transfer_mode` | enum | BINARY | 传输模式：BINARY、ASCII |
| `resume` | bool | true | 启用断点续传 |
| `connect_timeout` | chrono::seconds | 30 | 连接超时 |
| `timeout` | chrono::seconds | 60 | 操作超时 |

## URL 格式

### 标准 FTP URL

```
ftp://[user[:password]@]host[:port]/path
```

### FTPS URL

```
ftpes://[user[:password]@]host[:port]/path   # 显式 TLS
ftps://[user[:password]@]host[:port]/path    # 隐式 TLS
```

### 示例

```bash
# 匿名登录
ftp://ftp.example.com/file.zip

# 用户名密码
ftp://user:pass@ftp.example.com/file.zip

# 指定端口
ftp://ftp.example.com:2121/file.zip

# FTPS
ftpes://user:pass@ftp.example.com/file.zip
```

## 主动模式 vs 被动模式

### 被动模式（PASV）- 推荐

- 客户端连接服务器的数据端口
- 更适合防火墙后的客户端
- 是大多数客户端的默认模式

### 主动模式（PORT）

- 服务器连接客户端的数据端口
- 可能被防火墙阻止
- 适用于服务器在内网的场景

::: tip 选择建议
大多数情况下使用被动模式。只有在服务器位于防火墙后时才考虑主动模式。
:::

## 常见问题

### Q: 连接超时

A: 检查防火墙设置，确保 FTP 端口（21）和数据端口未被阻止。尝试切换主动/被动模式。

### Q: 530 Login incorrect

A: 检查用户名和密码是否正确。匿名登录使用：
- 用户名：`anonymous`
- 密码：任意（通常使用邮箱）

### Q: 目录列表错误

A: 某些服务器可能需要特定的列表格式。Falcon 会自动尝试多种格式。

### Q: FTPS 连接失败

A: 检查服务器的 SSL/TLS 配置：
- 显式 TLS：使用 `ftpes://`
- 隐式 TLS：使用 `ftps://`（端口 990）

## 安全建议

1. **使用 FTPS**：明文 FTP 传输的密码可被窃听
2. **验证证书**：生产环境应启用 SSL 验证
3. **使用强密码**：FTP 密码应足够复杂
4. **限制访问**：使用防火墙限制 FTP 访问

::: warning 安全警告
标准 FTP 协议以明文传输用户名和密码，不安全。请尽可能使用 FTPS 或 SFTP。
:::
