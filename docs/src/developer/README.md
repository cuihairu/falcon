# 开发者指南

欢迎来到 Falcon 开发者指南！本文档面向希望贡献代码或基于 Falcon 进行开发的开发者。

## 目录

- [开发环境搭建](#开发环境搭建)
- [项目架构](#项目架构)
- [代码规范](#代码规范)
- [测试指南](#测试指南)
- [插件开发](#插件开发)
- [提交代码](#提交代码)

## 开发环境搭建

### 前置要求

- **CMake** 3.15+
- **C++ 编译器** (GCC 7+, Clang 5+, MSVC 2017+)
- **Git**
- **vcpkg** (推荐) 或系统包管理器

### 获取源码

```bash
git clone https://github.com/yourusername/falcon.git
cd falcon
```

### 安装依赖

#### 使用 vcpkg

```bash
# 安装 vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh

# 安装依赖
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

#### 系统包管理器

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest cmake

# Ubuntu/Debian
sudo apt install build-essential cmake git \
  libspdlog-dev nlohmann-json3-dev libcurl4-openssl-dev \
  libssl-dev libgtest-dev
```

### 编译项目

```bash
# Debug 模式（开发）
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 编译
cmake --build build

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
  ],
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"
}
```

#### CLion

File → Settings → Build, Execution, Deployment → CMake
- CMake options: `-DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake`
- Build directory: `build`

## 项目架构

### Monorepo 结构

```
falcon/
├── packages/
│   ├── libfalcon/              # 核心库
│   │   ├── include/falcon/     # 公共头文件
│   │   ├── src/                # 实现文件
│   │   └── plugins/            # 协议插件
│   ├── falcon-cli/             # 命令行工具
│   └── falcon-daemon/          # 守护进程
├── apps/
│   ├── desktop/                # 桌面应用
│   └── web/                    # Web 界面
└── docs/                       # 文档
```

### 核心组件

| 组件 | 职责 |
|------|------|
| `DownloadEngine` | 下载引擎核心 |
| `TaskManager` | 任务管理器 |
| `PluginManager` | 插件管理器 |
| `IProtocolHandler` | 协议处理器接口 |

::: tip 详细架构
请查看 [架构设计文档](./architecture.md) 了解更多细节。
:::

## 代码规范

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
    std::string config_path_;
};

// 常量：全大写 + 下划线
const int MAX_RETRIES = 5;
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
 */
class DownloadEngine {
public:
    /**
     * @brief 添加下载任务
     *
     * @param url 下载 URL
     * @param options 下载选项
     * @return 任务 ID
     *
     * @throws InvalidURLException 当 URL 格式错误时
     */
    TaskId add_download(const std::string& url,
                       const DownloadOptions& options = {});
};
```

### 格式化

```bash
# 格式化所有文件
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
```

::: tip 详细规范
请查看 [编码规范文档](./coding-standards.md) 了解更多细节。
:::

## 测试指南

### 单元测试

使用 **Google Test** 框架：

```cpp
#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>

TEST(DownloadEngineTest, AddDownload) {
    falcon::DownloadEngine engine;
    TaskId id = engine.add_download("https://example.com/file.zip");
    EXPECT_NE(id, INVALID_TASK_ID);
}
```

### 运行测试

```bash
# 运行所有测试
ctest --test-dir build

# 运行特定测试
./build/bin/falcon_core_tests --gtest_filter="DownloadEngineTest*"

# 查看测试覆盖率
cmake --build build --target coverage
```

### 提交代码前检查

```bash
# 格式化检查
./scripts/check-format.sh

# 静态分析
./scripts/run-clang-tidy.sh

# 运行所有测试
ctest --test-dir build --output-on-failure
```

::: tip 详细测试指南
请查看 [测试文档](./testing.md) 了解更多细节。
:::

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

::: tip 详细插件开发
请查看 [插件开发文档](./plugins.md) 了解更多细节。
:::

## 提交代码

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

### Pull Request 流程

1. Fork 项目
2. 创建功能分支: `git checkout -b feature/my-feature`
3. 提交代码: `git commit -m "feat: 添加功能"`
4. 推送分支: `git push origin feature/my-feature`
5. 创建 Pull Request

### PR 检查清单

- [ ] 代码符合编码规范
- [ ] 通过所有测试
- [ ] 添加了必要的测试
- [ ] 更新了相关文档
- [ ] 提交信息清晰明确

## 调试技巧

### 使用 gdb

```bash
# 编译 Debug 版本
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# 启动 gdb
gdb ./build/bin/falcon-cli
```

### 使用 lldb (macOS)

```bash
lldb ./build/bin/falcon-cli
```

### 日志调试

```cpp
FALCON_LOG_DEBUG("调试信息: " << value);
FALCON_LOG_INFO("普通信息");
FALCON_LOG_WARN("警告信息");
FALCON_LOG_ERROR("错误信息");
```

::: tip 详细调试指南
请查看 [调试文档](./debugging.md) 了解更多细节。
:::

## 获取帮助

- **GitHub Issues**: [https://github.com/yourusername/falcon/issues](https://github.com/yourusername/falcon/issues)
- **Discussions**: [https://github.com/yourusername/falcon/discussions](https://github.com/yourusername/falcon/discussions)
- **Wiki**: [https://github.com/yourusername/falcon/wiki](https://github.com/yourusername/falcon/wiki)

::: tip 欢迎贡献
感谢你对 Falcon 的关注！我们欢迎任何形式的贡献。
:::
