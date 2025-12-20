# Falcon 第三方依赖

## 依赖管理策略

推荐使用 **vcpkg** 或 **Conan** 管理第三方依赖，避免使用 Git Submodules（维护成本高）。

---

## vcpkg 使用方式

### 安装 vcpkg

```bash
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh  # Linux/macOS
# 或
bootstrap-vcpkg.bat   # Windows
```

### 安装依赖

```bash
./vcpkg install curl libtorrent spdlog cli11 nlohmann-json gtest
```

### 配置 CMake

```bash
cmake -B build -S . \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
```

---

## Conan 使用方式

### 安装 Conan

```bash
pip install conan
```

### 创建 `conanfile.txt`

```ini
[requires]
libcurl/7.80.0
spdlog/1.10.0
nlohmann_json/3.11.2
cli11/2.3.2
gtest/1.12.1

[generators]
CMakeDeps
CMakeToolchain
```

### 安装依赖

```bash
conan install . --build=missing
```

---

## Git Submodules（备选）

如果无法使用包管理器，可将第三方库作为 Submodule：

```bash
cd third_party
git submodule add https://github.com/spdlog/spdlog.git
git submodule add https://github.com/nlohmann/json.git
```

然后在根 `CMakeLists.txt` 中：

```cmake
add_subdirectory(third_party/spdlog)
add_subdirectory(third_party/json)
```

---

## 依赖列表

| 依赖库 | 版本要求 | 用途 | 必选/可选 |
|--------|---------|------|----------|
| libcurl | 7.68+ | HTTP/FTP 协议支持 | 可选（插件） |
| libtorrent-rasterbar | 2.0+ | BitTorrent 协议 | 可选（插件） |
| spdlog | 1.9+ | 日志库 | 必选 |
| CLI11 | 2.3+ | 命令行解析 | CLI 必选 |
| nlohmann/json | 3.10+ | JSON 配置解析 | 必选 |
| gRPC | 1.40+ | RPC 框架 | Daemon 可选 |
| SQLite3 | 3.30+ | 任务持久化 | Daemon 必选 |
| GoogleTest | 1.12+ | 单元测试 | 测试必选 |

---

**建议**：优先使用 vcpkg（Windows/Linux/macOS 跨平台一致性好）。
