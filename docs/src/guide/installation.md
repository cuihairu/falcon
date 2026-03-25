# 安装说明

本文档详细介绍如何在当前仓库状态下构建和安装 Falcon。

## 目录

- [系统要求](#系统要求)
- [从源码编译](#从源码编译)
- [验证安装](#验证安装)

## 系统要求

### 最低要求

| 组件 | 要求 |
|------|------|
| 操作系统 | Windows 10+、macOS 10.14+、Linux（主流发行版） |
| CPU | x86_64 或 ARM64 |
| 内存 | 512 MB |
| 磁盘空间 | 100 MB |

### 编译要求

| 组件 | 版本 |
|------|------|
| CMake | 3.15+ |
| C++ 编译器 | GCC 7+、Clang 5+、MSVC 2017+ |
| Python | 非必需 |

当前仓库文档默认以源码构建为准。若未来提供 Release 制品或系统包，应以仓库发布页和 CI 产物说明为准，而不是本文中的静态占位示例。

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
git clone https://github.com/cuihairu/falcon.git
cd falcon

# 配置 CMake
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --config Release
```

### 使用系统包管理器

#### macOS

```bash
brew install spdlog nlohmann-json curl openssl googletest cmake

cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)

cmake --build build --config Release
```

#### Ubuntu/Debian

```bash
sudo apt install \
  build-essential \
  cmake \
  git \
  libspdlog-dev \
  nlohmann-json3-dev \
  libcurl4-openssl-dev \
  libssl-dev \
  libgtest-dev

cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

#### Windows（Visual Studio）

```powershell
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
cd C:\vcpkg
.\bootstrap-vcpkg.bat

.\vcpkg install spdlog nlohmann-json curl openssl gtest

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

### 常用编译开关

根目录 `CMakeLists.txt` 当前暴露了这些常用选项：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `FALCON_BUILD_TESTS` | `ON` | 构建测试 |
| `FALCON_BUILD_EXAMPLES` | `ON` | 构建示例 |
| `FALCON_BUILD_CLI` | `ON` | 构建 CLI |
| `FALCON_BUILD_DAEMON` | `ON` | 构建 Daemon |
| `FALCON_BUILD_DESKTOP` | `ON` | 构建桌面端 |
| `FALCON_ENABLE_HTTP` | `ON` | 启用 HTTP/HTTPS 插件 |
| `FALCON_ENABLE_FTP` | `ON` | 启用 FTP 插件 |
| `FALCON_ENABLE_BITTORRENT` | `OFF` | 启用 BitTorrent 插件 |
| `FALCON_ENABLE_THUNDER` | `OFF` | 启用迅雷插件 |
| `FALCON_ENABLE_QQDL` | `OFF` | 启用 QQ 旋风插件 |
| `FALCON_ENABLE_FLASHGET` | `OFF` | 启用快车插件 |
| `FALCON_ENABLE_ED2K` | `OFF` | 启用 ED2K 插件 |
| `FALCON_ENABLE_HLS` | `OFF` | 启用 HLS/DASH 插件 |

示例：

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DFALCON_BUILD_DESKTOP=OFF \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_HLS=ON
```

## 验证安装

```bash
./build/bin/falcon-cli --version
```

如果只构建了部分目标，请按实际输出目录运行对应二进制。
