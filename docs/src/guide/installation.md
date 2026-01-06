# 安装说明

本文档详细介绍如何在各种平台上安装 Falcon 下载器。

## 目录

- [系统要求](#系统要求)
- [预编译版本](#预编译版本)
- [包管理器安装](#包管理器安装)
- [从源码编译](#从源码编译)
- [Docker 安装](#docker-安装)
- [验证安装](#验证安装)

## 系统要求

### 最低要求

| 组件 | 要求 |
|------|------|
| 操作系统 | Windows 10+, macOS 10.14+, Linux (主流发行版) |
| CPU | x86_64 或 ARM64 |
| 内存 | 512 MB |
| 磁盘空间 | 100 MB |

### 编译要求（从源码编译）

| 组件 | 版本 |
|------|------|
| CMake | 3.15+ |
| C++ 编译器 | GCC 7+, Clang 5+, MSVC 2017+ |
| Python | 3.6+ (仅构建时需要) |

## 预编译版本

### 下载

从 [GitHub Releases](https://github.com/yourusername/falcon/releases) 下载对应平台的预编译版本。

| 平台 | 文件名 |
|------|--------|
| Linux (x64) | `falcon-1.0.0-linux-x64.tar.gz` |
| macOS (x64) | `falcon-1.0.0-macos-x64.tar.gz` |
| macOS (ARM64) | `falcon-1.0.0-macos-arm64.tar.gz` |
| Windows (x64) | `falcon-1.0.0-windows-x64.zip` |

### 安装步骤

#### Linux/macOS

```bash
# 解压
tar -xzf falcon-1.0.0-linux-x64.tar.gz

# 进入目录
cd falcon-1.0.0-linux-x64

# 安装到系统目录
sudo cp bin/falcon-cli /usr/local/bin/
sudo cp -r lib/* /usr/local/lib/

# 更新库缓存
sudo ldconfig
```

#### Windows

1. 解压 `falcon-1.0.0-windows-x64.zip`
2. 将解压后的目录添加到 PATH 环境变量
3. 在命令提示符中运行 `falcon-cli --version` 验证

## 包管理器安装

### Homebrew (macOS/Linux)

```bash
brew tap falcon/tap
brew install falcon
```

### Scoop (Windows)

```powershell
scoop bucket add falcon https://github.com/falcon/scoop.git
scoop install falcon
```

### Chocolatey (Windows)

```powershell
choco install falcon-downloader
```

### AUR (Arch Linux)

```bash
yay -S falcon-downloader
```

### apt (Ubuntu/Debian)

```bash
# 添加 Falcon APT 仓库
sudo add-apt-repository ppa:falcon/stable
sudo apt update

# 安装
sudo apt install falcon-downloader
```

## 从源码编译

### 依赖项

#### 核心依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| spdlog | 1.9+ | 日志库 |
| nlohmann/json | 3.10+ | JSON 解析 |
| libcurl | 7.68+ | HTTP 客户端 |
| OpenSSL | 1.1+ | SSL/TLS 支持 |

#### 可选依赖

| 依赖 | 版本 | 说明 |
|------|------|------|
| libtorrent | 2.0+ | BitTorrent 支持 |
| gtest | 1.12+ | 单元测试 |

### 使用 vcpkg（推荐）

#### 1. 安装 vcpkg

```bash
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh
```

#### 2. 安装依赖

```bash
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

#### 3. 编译

```bash
# 获取源码
git clone https://github.com/yourusername/falcon.git
cd falcon

# 配置 CMake
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --config Release

# 安装
sudo cmake --install build
```

### 使用系统包管理器

#### macOS

```bash
# 安装依赖
brew install spdlog nlohmann-json curl openssl googletest cmake

# 编译
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)

cmake --build build --config Release
sudo cmake --install build
```

#### Ubuntu/Debian

```bash
# 安装依赖
sudo apt install \
  build-essential \
  cmake \
  git \
  libspdlog-dev \
  nlohmann-json3-dev \
  libcurl4-openssl-dev \
  libssl-dev \
  libgtest-dev

# 编译
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
sudo cmake --install build
```

#### Windows (Visual Studio)

```powershell
# 安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

# 安装依赖
.\vcpkg install spdlog nlohmann-json curl openssl gtest

# 配置和编译
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
cmake --install build --config Release
```

### 编译选项

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DFALCON_ENABLE_HTTP=ON \          # 启用 HTTP 协议
  -DFALCON_ENABLE_FTP=ON \           # 启用 FTP 协议
  -DFALCON_ENABLE_BITTORRENT=ON \    # 启用 BitTorrent
  -DFALCON_ENABLE_THUNDER=ON \       # 启用迅雷协议
  -DFALCON_ENABLE_QQDL=ON \          # 启用 QQ旋风
  -DFALCON_ENABLE_FLASHGET=ON \      # 启用快车
  -DFALCON_ENABLE_ED2K=ON \          # 启用 ED2K
  -DFALCON_ENABLE_HLS=ON             # 启用 HLS/DASH
```

## Docker 安装

### 使用预构建镜像

```bash
docker pull falcondownloader/falcon:latest

# 运行容器
docker run -v $(pwd)/downloads:/downloads falcondownloader/falcon \
  falcon-cli https://example.com/file.zip -o /downloads/
```

### 从 Dockerfile 构建

```bash
git clone https://github.com/yourusername/falcon.git
cd falcon

docker build -t falcon .
```

## 验证安装

```bash
falcon-cli --version
```

你应该看到类似以下输出：

```
Falcon CLI v1.0.0
```

查看支持的协议：

```bash
falcon-cli --list-protocols
```

输出：

```
支持的协议:
  http/https   - HTTP/HTTPS 协议
  ftp/ftps     - FTP/FTPS 协议
  bittorrent   - BitTorrent/Magnet 协议
  thunder      - 迅雷 thunder:// 协议
  qqdl         - QQ旋风 qqlink:// 协议
  flashget     - 快车 flashget:// 协议
  ed2k         - ED2K 电驴协议
  hls          - HLS/DASH 流媒体协议
```

## 卸载

### 从预编译版本

#### Linux/macOS

```bash
sudo rm /usr/local/bin/falcon-cli
sudo rm -rf /usr/local/lib/libfalcon*
```

#### Windows

删除安装目录，并从 PATH 环境变量中移除。

### 从包管理器

```bash
# Homebrew
brew uninstall falcon

# AUR
yay -R falcon-downloader

# apt
sudo apt remove falcon-downloader
```

## 故障排除

### 编译错误

**问题**：找不到某些头文件

**解决方案**：确保所有依赖已正确安装

```bash
# 检查 CMake 是否找到所有依赖
cmake -B build -S . --debug-find
```

**问题**：链接错误

**解决方案**：确保库路径正确

```bash
# 查看详细链接信息
cmake --build build --verbose
```

### 运行时错误

**问题**：找不到共享库

**解决方案**：设置库路径

```bash
# Linux
export LD_LIBRARY_PATH=/usr/local/lib:$LD_LIBRARY_PATH

# macOS
export DYLD_LIBRARY_PATH=/usr/local/lib:$DYLD_LIBRARY_PATH
```

::: tip 获取帮助
如果遇到其他问题，请查看 [常见问题](../faq.md) 或在 [GitHub Issues](https://github.com/yourusername/falcon/issues) 中提问。
:::
