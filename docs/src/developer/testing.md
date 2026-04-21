# 测试指南

本文档说明当前仓库中的测试组织方式，以及编写测试时应遵循的接口口径。

## 测试目录

```text
packages/libfalcon/tests/
packages/falcon-daemon/tests/
```

`libfalcon` 下目前包含：

- `unit/`
- `integration/`
- `performance/`

## 构建测试

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

如果本机没有预装 `GTest`，当前仓库会直接跳过各测试目录，不再自动联网拉取依赖。

常见安装方式：

```bash
# macOS (Homebrew)
brew install googletest

# Ubuntu / Debian
sudo apt-get install libgtest-dev
```

安装完成后，重新执行上面的 CMake 配置与构建命令。

## 编写测试时的接口基线

当前公开接口应以这些符号为准：

- `DownloadEngine::add_task()`
- `DownloadEngine::start_task()`
- `DownloadEngine::pause_task()`
- `DownloadEngine::resume_task()`
- `DownloadEngine::cancel_task()`
- `DownloadTask::status()`
- `DownloadTask::id()`

不要再以 `startDownload()`、`loadPlugin()`、`getStatus()` 作为新测试样例的默认写法。

## 基本示例

```cpp
#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>

TEST(DownloadEngineTest, CreateInstance) {
    falcon::DownloadEngine engine;
    EXPECT_GE(engine.get_total_task_count(), 0u);
}

TEST(DownloadEngineTest, AddTaskWithSupportedUrl) {
    falcon::DownloadEngine engine;
    auto task = engine.add_task("https://example.com/file.zip");
    EXPECT_NE(task, nullptr);
}
```

## 测试固件

```cpp
class DownloadEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<falcon::DownloadEngine>();
    }

    std::unique_ptr<falcon::DownloadEngine> engine_;
};

TEST_F(DownloadEngineTest, AddedTaskStartsAsPending) {
    auto task = engine_->add_task("https://example.com/file.zip");
    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->status(), falcon::TaskStatus::Pending);
}
```

## 参数化测试

```cpp
class ProtocolSupportTest : public ::testing::TestWithParam<std::string> {
protected:
    falcon::DownloadEngine engine_;
};

TEST_P(ProtocolSupportTest, UrlSupportCheck) {
    EXPECT_TRUE(engine_.is_url_supported(GetParam()));
}

INSTANTIATE_TEST_SUITE_P(
    CommonProtocols,
    ProtocolSupportTest,
    ::testing::Values(
        "https://example.com/file.zip",
        "http://example.com/file.zip",
        "ftp://ftp.example.com/file.zip"
    )
);
```

## 集成测试建议

集成测试更适合验证：

- 真实 URL 路由是否命中正确处理器
- 下载任务从 `Pending` 到 `Completed` 的流转
- 断点续传和输出路径行为
- 并发任务数量与全局限制

示例：

```cpp
TEST(DownloadEngineIntegration, AddThenStartTask) {
    falcon::DownloadEngine engine;
    falcon::DownloadOptions options;
    options.output_directory = "/tmp/falcon-test";

    auto task = engine.add_task("http://localhost:8080/test.zip", options);
    ASSERT_NE(task, nullptr);

    EXPECT_TRUE(engine.start_task(task->id()));
    task->wait();

    EXPECT_TRUE(
        task->status() == falcon::TaskStatus::Completed ||
        task->status() == falcon::TaskStatus::Failed);
}
```

## 覆盖率

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON \
  -DFALCON_ENABLE_COVERAGE=ON

cmake --build build
ctest --test-dir build
```

## 最佳实践

- 为公共头文件中的真实接口写测试，不要围绕历史设计稿扩展新样例。
- 优先测试稳定行为：状态迁移、URL 支持判断、输出路径、并发限制。
- 需要网络的测试应尽量使用本地测试服务器或夹具，避免外网不稳定。
- 当插件默认关闭时，测试应明确设置对应的构建开关前提。
