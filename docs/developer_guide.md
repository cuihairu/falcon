# Falcon 开发者指南

## 开发环境搭建

### 前置要求

- **CMake** 3.15+
- **C++ 编译器** (GCC 7+, Clang 5+, MSVC 2017+)
- **vcpkg** (可选，用于依赖管理)

### 获取源码

```bash
git clone https://github.com/yourusername/falcon.git
cd falcon
```

### 安装依赖 (使用 vcpkg)

```bash
# macOS/Linux
./vcpkg install spdlog nlohmann-json curl openssl gtest

# Windows
vcpkg install spdlog nlohmann-json curl openssl gtest
```

### 编译项目

```bash
# 配置 CMake
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build --config Release

# 运行测试
ctest --test-dir build --output-on-failure
```

### IDE 配置

#### VS Code

安装扩展：
- C/C++ Extension Pack
- CMake Tools

配置 `.vscode/settings.json`:
```json
{
    "cmake.configureArgs": [
        "-DCMAKE_TOOLCHAIN_FILE=${env:VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    ]
}
```

#### CLion

File → Settings → Build, Execution, Deployment → CMake
- CMake options: `-DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake`
- Build directory: `build`

## 项目结构

```
falcon/
├── packages/
│   ├── libfalcon/          # 核心库
│   │   ├── include/falcon/ # 公共头文件
│   │   ├── src/            # 实现文件
│   │   └── tests/          # 单元测试
│   ├── falcon-cli/         # 命令行工具
│   └── falcon-daemon/      # 守护进程
├── apps/                   # 应用层
│   ├── desktop/            # 桌面应用
│   └── web/                # Web 界面
├── docs/                   # 文档
└── examples/               # 示例代码
```

## 编码规范

### C++ 风格

遵循 **Google C++ Style Guide**：

```cpp
// 类名：PascalCase
class DownloadEngine {
public:
    // 函数名：snake_case
    void add_download(const std::string& url);

    // 成员变量：下划线后缀
    int task_count_;

private:
    // 私有成员：下划线后缀
    std::string config_path_;
};

// 常量：全大写 + 下划线
const int MAX_RETRIES = 5;

// 文件名：snake_case
// download_engine.cpp
```

### 命名空间

```cpp
namespace falcon {
namespace net {

class EventPoll {
    // ...
};

} // namespace net
} // namespace falcon
```

### 注释规范

```cpp
/**
 * @file download_engine.hpp
 * @brief 下载引擎类定义
 * @author Falcon Team
 * @date 2025-12-25
 */

/**
 * @class DownloadEngine
 * @brief 高性能下载引擎
 *
 * 支持多协议、多线程、断点续传等功能
 *
 * @example
 * DownloadEngine engine;
 * engine.add_download("http://example.com/file.zip");
 * engine.run();
 */
class DownloadEngine {
public:
    /**
     * @brief 添加下载任务
     *
     * @param url 下载 URL
     * @param options 下载选项（可选）
     * @return 任务 ID
     *
     * @throws InvalidURLException 当 URL 格式错误时
     *
     * @code
     * TaskId id = engine.add_download("http://example.com/file.zip");
     * @endcode
     */
    TaskId add_download(const std::string& url,
                       const DownloadOptions& options = {});
};
```

### 格式化

使用 **clang-format** 自动格式化：

```bash
# 格式化所有文件
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
```

配置文件 `.clang-format`:
```yaml
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
```

## 开发工作流

### 1. 创建功能分支

```bash
git checkout -b feature/my-feature
```

### 2. 编写代码

- 添加公共 API 到 `include/falcon/`
- 添加实现到 `src/`
- 添加测试到 `tests/unit/` 或 `tests/integration/`

### 3. 编写测试

```cpp
// tests/unit/my_feature_test.cpp
#include <gtest/gtest.h>
#include <falcon/my_feature.hpp>

TEST(MyFeatureTest, BasicOperation) {
    falcon::MyFeature feature;
    EXPECT_TRUE(feature.do_something());
}
```

### 4. 运行测试

```bash
# 运行所有测试
ctest --test-dir build

# 运行特定测试
./build/bin/falcon_core_tests --gtest_filter="MyFeatureTest*"

# 查看测试覆盖率
cmake --build build --target coverage
```

### 5. 提交代码

```bash
git add .
git commit -m "feat: 添加新功能 XXX"
git push origin feature/my-feature
```

### 提交信息规范

遵循 **Conventional Commits**:

```
feat: 添加 XXX 功能
fix: 修复 YYY 问题
docs: 更新 ZZZ 文档
test: 添加 AAA 测试
refactor: 重构 BBB 模块
perf: 优化 CCC 性能
```

## 测试指南

### 单元测试

使用 **Google Test** 框架：

```cpp
#include <gtest/gtest.h>

class MyTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // 测试前初始化
    }

    void TearDown() override {
        // 测试后清理
    }

    falcon::MyClass* obj_;
};

TEST_F(MyTestFixture, TestCase1) {
    EXPECT_EQ(obj_->method(), expected_value);
}
```

### Mock 测试

使用 **GMock** 进行依赖隔离：

```cpp
class MockNetwork : public NetworkInterface {
public:
    MOCK_METHOD(int, send, (const std::string& data), (override));
    MOCK_METHOD(std::string, receive, (), (override));
};

TEST(HttpClientTest, SendRequest) {
    MockNetwork mock_network;
    EXPECT_CALL(mock_network, send("GET / HTTP/1.1"))
        .Times(1);

    HttpClient client(&mock_network);
    client.get("/");
}
```

### 集成测试

测试完整流程：

```cpp
TEST(IntegrationTest, DownloadCompleteWorkflow) {
    // 1. 创建测试服务器
    TestServer server(8080);
    server.start();

    // 2. 创建下载引擎
    DownloadEngineV2 engine;
    TaskId id = engine.add_download("http://localhost:8080/test.zip");

    // 3. 运行下载
    engine.run();

    // 4. 验证结果
    EXPECT_TRUE(file_exists("/tmp/downloads/test.zip"));
}
```

## 调试技巧

### 使用 gdb

```bash
# 编译 Debug 版本
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 启动 gdb
gdb ./build/bin/falcon-cli

# gdb 命令
(gdb) break main
(gdb) run
(gdb) next
(gdb) print variable_name
(gdb) continue
```

### 使用 lldb (macOS/LLVM)

```bash
lldb ./build/bin/falcon-cli

# lldb 命令
(lldb) breakpoint set --name main
(lldb) run
(lldb) next
(lldb) expr variable_name
(lldb) continue
```

### 日志调试

```cpp
// 使用日志宏
FALCON_LOG_DEBUG("调试信息: " << value);
FALCON_LOG_INFO("普通信息");
FALCON_LOG_WARN("警告信息");
FALCON_LOG_ERROR("错误信息");

// 设置日志级别
// export FALCON_LOG_LEVEL=DEBUG
```

### Valgrind 内存检查

```bash
# 编译带符号信息的版本
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_ENABLE_ASAN=OFF

# 运行 Valgrind
valgrind --leak-check=full --show-leak-kinds=all ./build/bin/falcon-cli
```

### AddressSanitizer

```bash
# 启用 ASan
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_ENABLE_ASAN=ON

# 运行程序
./build/bin/falcon-cli
```

## 性能分析

### 使用 perf (Linux)

```bash
# 记录性能数据
perf record -g ./build/bin/falcon-cli

# 查看报告
perf report

# 可视化火焰图
perf script | ./FlameGraph/flamegraph.pl > flamegraph.svg
```

### 使用 Instruments (macOS)

```bash
# 启动 Instruments
instruments -t "Time Profiler" ./build/bin/falcon-cli
```

### 基准测试

使用 **Google Benchmark**:

```cpp
#include <benchmark/benchmark.h>

static void BM_DownloadSpeed(benchmark::State& state) {
    for (auto _ : state) {
        DownloadEngine engine;
        engine.add_download("http://example.com/100MB.zip");
        engine.run();
    }
}
BENCHMARK(BM_DownloadSpeed);

BENCHMARK_MAIN();
```

## 插件开发

### 创建新协议插件

1. **定义插件接口**:

```cpp
// plugins/myprotocol/myprotocol_plugin.hpp
#pragma once
#include <falcon/plugin_interface.hpp>

class MyProtocolPlugin : public IProtocolHandler {
public:
    std::string protocol() const override { return "myproto"; }

    bool can_handle(const std::string& url) const override {
        return url.find("myproto://") == 0;
    }

    std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener) override;
};
```

2. **实现插件逻辑**:

```cpp
// plugins/myprotocol/myprotocol_plugin.cpp
#include "myprotocol_plugin.hpp"

std::shared_ptr<DownloadTask> MyProtocolPlugin::download(
    const std::string& url,
    const DownloadOptions& options,
    IEventListener* listener) {

    // 解析 URL
    // 建立连接
    // 下载数据
    // 通知进度
}
```

3. **注册插件**:

```cpp
// src/plugin_manager.cpp
void PluginManager::load_builtin_plugins() {
    register_plugin(std::make_unique<MyProtocolPlugin>());
}
```

## 发布流程

### 版本号规范

遵循 **语义化版本** (Semantic Versioning):

```
MAJOR.MINOR.PATCH

1.0.0 - 初始稳定版本
1.1.0 - 添加新功能（向后兼容）
1.1.1 - Bug 修复
2.0.0 - 破坏性更改
```

### 发布步骤

1. **更新版本号**:

```cpp
// include/falcon/version.hpp
#define FALCON_VERSION_MAJOR 1
#define FALCON_VERSION_MINOR 0
#define FALCON_VERSION_PATCH 0
```

2. **更新 ChangeLog**:

```markdown
## [1.0.0] - 2025-12-25

### Added
- XXX 功能
- YYY 功能

### Fixed
- 修复 ZZZ 问题
```

3. **创建 Git 标签**:

```bash
git tag -a v1.0.0 -m "Release version 1.0.0"
git push origin v1.0.0
```

4. **构建发布包**:

```bash
# Linux
cmake --build build --config Release
cpack

# macOS
cmake --build build --config Release
cpack

# Windows
cmake --build build --config Release
cpack
```

### 持续集成

GitHub Actions 配置 `.github/workflows/ci.yml`:

```yaml
name: CI

on: [push, pull_request]

jobs:
  build:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]

    steps:
    - uses: actions/checkout@v2
    - name: Install dependencies
      run: |
        ./vcpkg install spdlog nlohmann-json curl openssl gtest

    - name: Configure CMake
      run: cmake -B build -DCMAKE_BUILD_TYPE=Release

    - name: Build
      run: cmake --build build

    - name: Test
      run: ctest --test-dir build
```

## 常见问题

### Q: 如何添加新的依赖？

A: 在 `CMakeLists.txt` 中添加：

```cmake
find_package(SomePackage REQUIRED)
target_link_libraries(falcon PRIVATE SomePackage::SomePackage)
```

### Q: 如何调试链接错误？

A: 使用详细模式：

```bash
cmake --build build --verbose
```

### Q: 如何解决编译错误？

A: 检查：
1. C++ 标准版本是否匹配
2. 头文件是否正确包含
3. 库是否正确链接

## 社区资源

- **GitHub**: https://github.com/yourusername/falcon
- **Issues**: https://github.com/yourusername/falcon/issues
- **Discussions**: https://github.com/yourusername/falcon/discussions
- **Wiki**: https://github.com/yourusername/falcon/wiki

## 许可证

Apache License 2.0
