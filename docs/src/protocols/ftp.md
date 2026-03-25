# FTP/FTPS 协议

Falcon 支持 FTP 和 FTPS，并默认内置 FTP 处理器。

## 特性

- ✅ FTP 下载
- ✅ FTPS 链接
- ✅ 用户名 / 密码认证
- ✅ 匿名访问
- ✅ 断点续传
- ✅ 代理透传

## 命令行使用

### 基本下载

```bash
falcon-cli ftp://ftp.example.com/file.zip
falcon-cli ftp://user:password@ftp.example.com/file.zip
falcon-cli ftp://ftp.example.com:2121/file.zip
```

### FTPS

```bash
falcon-cli ftpes://ftp.example.com/file.zip
falcon-cli ftps://ftp.example.com/file.zip
falcon-cli ftpes://user:password@ftp.example.com/file.zip
```

### 断点续传与代理

```bash
falcon-cli ftp://ftp.example.com/large.zip --continue true
falcon-cli ftp://ftp.example.com/file.zip --proxy socks5://proxy:1080
```

## C++ API

当前公共 API 不暴露独立的 `FtpOptions` 结构体，常用方式仍是 URL 加 `DownloadOptions`。

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.output_directory = "./downloads";
    options.output_filename = "file.zip";
    options.resume_enabled = true;
    options.timeout_seconds = 30;

    auto task = engine.add_task(
        "ftp://user:password@ftp.example.com/file.zip",
        options
    );

    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## URL 格式

```text
ftp://[user[:password]@]host[:port]/path
ftpes://[user[:password]@]host[:port]/path
ftps://[user[:password]@]host[:port]/path
```

## 说明

- 用户名和密码通常直接从 URL 中给出。
- 连接、重试、输出目录、限速等通用行为由 `DownloadOptions` 负责。
- 更细的 FTP 专属行为如果未来进入公共头文件，再单独补充到这里。

::: warning 安全提示
明文 FTP 会直接传输用户名和密码。能使用 FTPS 时，优先使用 FTPS。
:::
