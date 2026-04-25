# 快速开始

本指南基于当前仓库实际状态，帮助你从源码构建并运行 Falcon CLI。

## 前置要求

- **操作系统**：Windows 10+、macOS 10.14+、Linux（主流发行版）
- **编译器**：
  - GCC 7+（Linux）
  - Clang 5+（macOS）
  - MSVC 2017+（Windows）
- **CMake**：3.15 或更高版本

## 安装

### 从源码编译（当前推荐）

#### 1. 获取源码

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
```

#### 2. 安装依赖

**使用 vcpkg（推荐）**：

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

**或使用系统包管理器**：

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest

# Ubuntu/Debian
sudo apt install libspdlog-dev nlohmann-json3-dev libcurl4-openssl-dev libssl-dev libgtest-dev
```

#### 3. 编译

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

cmake --build build --config Release
```

当前文档仅保证源码构建路径准确。若后续提供预编译版本或系统包，请以仓库发布页和 CI 产物说明为准。

## 验证安装

```bash
falcon-cli --version
```

你应该看到类似以下输出：

```
Falcon CLI v0.2.0
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

# 查看下载进度和详细日志
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

## 当前可直接使用的 CLI 选项

仓库中的 CLI 参数以 `packages/falcon-cli/src/main.cpp` 为准，下面列的是已经在代码里实现的常用项。

| 选项 | 说明 | 示例 |
|------|------|------|
| `-o`, `--output` | 指定输出文件名 | `-o file.zip` |
| `-d`, `--directory` | 指定输出目录 | `-d ~/Downloads` |
| `-c`, `--connections` | 设置并发连接数 | `-c 5` |
| `-j`, `--max-concurrent-downloads` | 设置最大并发任务数 | `-j 3` |
| `--limit` | 单任务限速 | `--limit 1M` |
| `--retry-wait` | 设置重试等待秒数 | `--retry-wait 5` |
| `--proxy` | 指定代理地址 | `--proxy http://127.0.0.1:7890` |
| `-H`, `--header` | 添加 HTTP 头 | `-H "Authorization: Bearer token"` |

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
falcon-cli -i urls.txt -j 3
```

## 下一步

- [安装说明](installation.md) - 详细的构建和依赖说明
- [使用指南](usage.md) - 深入了解命令行选项和 C++ API
- [协议支持](/protocols/) - 了解所有支持的下载协议

## 获取帮助

```bash
# 查看帮助信息
falcon-cli --help

# 查看版本
falcon-cli --version
```

::: tip 提示
使用 `--verbose` 选项可以查看详细的下载日志，有助于排查问题。
:::
