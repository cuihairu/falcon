# 快速开始

本指南将帮助你快速上手 Falcon 下载器。

## 前置要求

- **操作系统**：Windows 10+、macOS 10.14+、Linux（主流发行版）
- **编译器**：
  - GCC 7+ (Linux)
  - Clang 5+ (macOS)
  - MSVC 2017+ (Windows)
- **CMake**：3.15 或更高版本

## 安装

### 方式一：使用包管理器（推荐）

#### macOS

```bash
brew install falcon
```

#### Ubuntu/Debian

```bash
sudo apt update
sudo apt install falcon-downloader
```

#### Arch Linux

```bash
yay -S falcon-downloader
```

### 方式二：从源码编译

#### 1. 获取源码

```bash
git clone https://github.com/yourusername/falcon.git
cd falcon
```

#### 2. 安装依赖

**使用 vcpkg（推荐）**：

```bash
# 安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh

# 安装依赖
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

**或使用系统包管理器**：

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest

# Ubuntu/Debian
sudo apt install libspdlog-dev nlohmann-json3-dev libcurl4-openssl-dev libssl-dev libgtest-dev
```

#### 3. 编译安装

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
sudo cmake --install build
```

### 方式三：使用预编译版本

从 [GitHub Releases](https://github.com/yourusername/falcon/releases) 下载对应平台的预编译版本。

## 验证安装

```bash
falcon-cli --version
```

你应该看到类似以下输出：

```
Falcon CLI v1.0.0
```

## 第一个下载

### 命令行下载

```bash
# 下载单个文件
falcon-cli https://example.com/file.zip

# 指定输出文件名
falcon-cli https://example.com/file.zip -o my-file.zip

# 指定保存目录
falcon-cli https://example.com/file.zip -d ~/Downloads

# 多线程下载
falcon-cli https://example.com/large.zip -c 5

# 查看下载进度
falcon-cli https://example.com/file.zip --verbose
```

### 下载不同类型的链接

#### HTTP/HTTPS

```bash
falcon-cli https://example.com/file.zip
```

#### 迅雷链接

```bash
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
```

#### 磁力链接

```bash
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example"
```

#### ED2K 链接

```bash
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"
```

#### HLS 流媒体

```bash
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4
```

## 配置文件

Falcon 支持配置文件来自定义默认行为。

### 配置文件位置

- **Linux/macOS**: `~/.config/falcon/config.json`
- **Windows**: `%APPDATA%\falcon\config.json`

### 示例配置

```json
{
  "default_download_dir": "~/Downloads",
  "max_concurrent_tasks": 5,
  "timeout_seconds": 30,
  "max_retries": 3,
  "speed_limit": "0",
  "user_agent": "Falcon/1.0",
  "verify_ssl": true,
  "auto_resume": true,
  "log_level": "info"
}
```

### 配置选项说明

| 选项 | 说明 | 默认值 |
|------|------|--------|
| `default_download_dir` | 默认下载目录 | `~/Downloads` |
| `max_concurrent_tasks` | 最大并发任务数 | `5` |
| `timeout_seconds` | 请求超时时间（秒） | `30` |
| `max_retries` | 最大重试次数 | `3` |
| `speed_limit` | 速度限制（字节/秒，0为无限制） | `0` |
| `user_agent` | HTTP User-Agent | `Falcon/1.0` |
| `verify_ssl` | 验证 SSL 证书 | `true` |
| `auto_resume` | 自动断点续传 | `true` |
| `log_level` | 日志级别 | `info` |

## 批量下载

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

## 下一步

- [安装说明](installation.md) - 详细的安装和配置说明
- [使用指南](usage.md) - 深入了解命令行选项和 C++ API
- [协议支持](../protocols/README.md) - 了解所有支持的下载协议

## 获取帮助

```bash
# 查看帮助信息
falcon-cli --help

# 查看支持的协议
falcon-cli --list-protocols

# 查看配置文件位置
falcon-cli --config-path
```

::: tip 提示
使用 `--verbose` 选项可以查看详细的下载日志，有助于排查问题。
:::
