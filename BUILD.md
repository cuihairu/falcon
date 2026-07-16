# 构建说明

## 前置要求

- CMake 3.15+
- C++17 兼容的编译器
- vcpkg 包管理器

## 快速开始

### 1. 克隆仓库
```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
```

### 2. 安装依赖

#### 方法一：使用 vcpkg（推荐）
```bash
# 如果还没有安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git $VCPKG_ROOT
$VCPKG_ROOT/bootstrap-vcpkg.sh

# 让 CMake 通过 vcpkg manifest 自动安装项目依赖
export VCPKG_ROOT=/path/to/vcpkg
```

仓库根目录包含 `vcpkg.json`。使用 `CMAKE_TOOLCHAIN_FILE` 配置 CMake 时，vcpkg 会自动安装默认依赖，包括 `gtest`、`curl`、`openssl`、`spdlog`、`nlohmann-json` 和 `fmt`。

Qt6 不在默认依赖中；桌面应用依赖位于 `desktop` feature，需要显式启用。

#### 方法二：使用系统包管理器
```bash
# macOS (使用 Homebrew)
brew install curl spdlog nlohmann-json googletest

# Ubuntu/Debian
sudo apt-get install libcurl4-openssl-dev libspdlog-dev nlohmann-json3-dev libgtest-dev
```

### 3. 构建项目

#### 构建核心库
```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_BUILD_TESTS=OFF \
  -DFALCON_BUILD_CLI=OFF

cmake --build build --config Release
```

#### 使用 vcpkg 配置构建（推荐）
```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_BUILD_TESTS=OFF \
  -DFALCON_BUILD_DESKTOP=OFF \
  -DFALCON_BUILD_CLI=OFF

cmake --build build --config Release
```

如需构建桌面应用，需要额外安装/启用 Qt6：

```bash
cmake -B build-desktop -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DVCPKG_MANIFEST_FEATURES=desktop \
  -DFALCON_BUILD_DESKTOP=ON
```

## 构建选项

### 通用选项
- `DCMAKE_BUILD_TYPE`: Debug/Release
- `DFALCON_BUILD_TESTS`: 构建测试（默认 OFF）
- `DFALCON_BUILD_EXAMPLES`: 构建示例（默认 OFF）

### 功能选项
- `DFALCON_ENABLE_HTTP`: HTTP/HTTPS 支持（默认 ON）
- `DFALCON_ENABLE_FTP`: FTP 支持（默认 ON）
- `FALCON_ENABLE_BITTORRENT`: BitTorrent 支持（默认 OFF）
- `FALCON_ENABLE_CLOUD_STORAGE`: 云存储支持（默认 OFF）
- `FALCON_ENABLE_RESOURCE_BROWSER`: 资源浏览（默认 OFF）

### 应用选项
- `DFALCON_BUILD_CLI`: 构建 CLI 工具（默认 ON）
- `DFALCON_BUILD_DAEMON`: 构建守护进程（默认 OFF）

## 跨平台构建

### Linux (Ubuntu/Debian)
```bash
sudo apt-get update
sudo apt-get install build-essential cmake git

# 使用 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
./vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=/path/to/vcpkg
./vcpkg/vcpkg install

cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DCMAKE_BUILD_TYPE=Release
```

### macOS
```bash
# 安装 Xcode Command Line Tools
xcode-select --install

# 使用 Homebrew 或 vcpkg
brew install cmake

cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=TRUE
```

### Windows (Visual Studio)
```powershell
# 安装 Visual Studio 2022
# 确保安装 "使用 C++ 的桌面开发" 工作负载

# 克隆并安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat

# 构建项目
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-windows \
  -A x64

cmake --build build --config Release
```

## 测试

推荐先运行核心库和分层约束测试，验证最小稳定面：

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake -B build-vcpkg-core -S . \
  -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_TARGET_TRIPLET=x64-linux \
  -DFALCON_BUILD_TESTS=ON \
  -DFALCON_BUILD_DESKTOP=OFF \
  -DFALCON_BUILD_DAEMON=OFF \
  -DFALCON_BUILD_CLI=OFF \
  -DFALCON_BUILD_EXAMPLES=OFF \
  -DFALCON_ENABLE_RESOURCE_BROWSER=OFF \
  -DFALCON_ENABLE_CLOUD_STORAGE=OFF \
  -DFALCON_ENABLE_RESOURCE_SEARCH=OFF \
  -DFALCON_ENABLE_CONFIG_MANAGER=OFF

cmake --build build-vcpkg-core --target falcon_core_tests falcon_split_tests -j4
./build-vcpkg-core/bin/falcon_core_tests --gtest_brief=1
./build-vcpkg-core/bin/falcon_split_tests --gtest_brief=1
```

当前已验证结果：

- `falcon_core_tests`: 384 个测试通过
- `falcon_split_tests`: 10 个测试通过

运行全部已配置测试：

```bash
cd build
ctest --output-on-failure
```

如果裸 CMake 配置提示 `GTest not found`，说明当前构建没有使用 vcpkg toolchain，也没有安装系统 GTest。优先使用上面的 vcpkg manifest 构建；只有选择系统包管理器构建时，才需要手动安装 `libgtest-dev` / `googletest`。

## 安装

### 安装到系统目录（可选）
```bash
sudo cmake --install build
```

### 安装到指定目录
```bash
cmake -DCMAKE_INSTALL_PREFIX=/usr/local --install build
```

## 故障排除

### 找不到依赖
- 确保已正确安装 vcpkg
- 检查 `CMAKE_TOOLCHAIN_FILE` 路径是否正确
- 清理构建缓存后重新配置

### OpenSSL 错误
- macOS: 可能需要设置 `OPENSSL_ROOT_DIR` 环境变量
- Linux: 安装 `libssl-dev` 包

### 链接错误
- 检查依赖库版本兼容性
- 确保所有必需的库都已安装

## 开发者模式

启用调试信息和测试：
```bash
cmake -B build-debug -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON \
  -DFALCON_BUILD_EXAMPLES=ON

cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

## 性能优化

使用优化编译选项：
```bash
cmake -B build-opt -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS="-O3 -march=native" \
  -DCMAKE_C_FLAGS="-O3 -march=native"

cmake --build build-opt --config Release
```
