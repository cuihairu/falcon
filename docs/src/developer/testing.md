# 测试指南

本文档介绍 Falcon 的测试框架和编写测试的最佳实践。

## 测试框架

Falcon 使用 **Google Test** 作为单元测试框架，**Google Mock** 用于 mock 测试。

## 测试目录结构

```
falcon/
├── packages/
│   ├── libfalcon/
│   │   └── tests/
│   │       ├── unit/           # 单元测试
│   │       ├── integration/    # 集成测试
│   │       └── benchmark/      # 性能测试
│   ├── falcon-cli/
│   │   └── tests/
│   └── falcon-daemon/
│       └── tests/
```

## 编写单元测试

### 基本测试

```cpp
#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>

TEST(DownloadEngineTest, CreateInstance) {
    falcon::DownloadEngine engine;
    EXPECT_FALSE(engine.isRunning());
}

TEST(DownloadEngineTest, LoadPlugin) {
    falcon::DownloadEngine engine;
    EXPECT_TRUE(engine.loadPlugin("http"));
    EXPECT_FALSE(engine.loadPlugin("nonexistent"));
}
```

### 测试固件

```cpp
class DownloadEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = std::make_unique<falcon::DownloadEngine>();
        engine_->loadAllPlugins();
    }

    void TearDown() override {
        engine_.reset();
    }

    std::unique_ptr<falcon::DownloadEngine> engine_;
};

TEST_F(DownloadEngineTest, StartDownload) {
    auto task = engine_->startDownload("https://example.com/file.zip");
    EXPECT_NE(task, nullptr);
}
```

### 参数化测试

```cpp
class ProtocolTest : public ::testing::TestWithParam<std::string> {
protected:
    falcon::DownloadEngine engine_;
};

TEST_P(ProtocolTest, SupportsUrl) {
    std::string url = GetParam();
    EXPECT_TRUE(engine_->supportsUrl(url));
}

INSTANTIATE_TEST_SUITE_P(
    CommonProtocols,
    ProtocolTest,
    ::testing::Values(
        "https://example.com/file.zip",
        "http://example.com/file.zip",
        "ftp://ftp.example.com/file.zip"
    )
);
```

## Mock 测试

### 使用 GMock

```cpp
#include <gmock/gmock.h>

class MockNetwork : public NetworkInterface {
public:
    MOCK_METHOD(int, send, (const std::string& data), (override));
    MOCK_METHOD(std::string, receive, (), (override));
    MOCK_METHOD(bool, connect, (const std::string& host, int port), (override));
    MOCK_METHOD(void, disconnect, (), (override));
};

TEST(HttpClientTest, SendRequest) {
    MockNetwork mock_network;

    // 设置期望
    EXPECT_CALL(mock_network, connect("example.com", 80))
        .Times(1)
        .WillOnce(Return(true));

    EXPECT_CALL(mock_network, send("GET / HTTP/1.1\r\n"))
        .Times(1)
        .WillOnce(Return(17));

    EXPECT_CALL(mock_network, receive())
        .Times(1)
        .WillOnce(Return("HTTP/1.1 200 OK\r\n\r\n"));

    HttpClient client(&mock_network);
    bool success = client.get("/");
    EXPECT_TRUE(success);
}
```

### 高级 Mock

```cpp
TEST(AdvancedMockTest, RetryOnFailure) {
    MockNetwork mock_network;

    // 前两次失败，第三次成功
    EXPECT_CALL(mock_network, send(_))
        .Times(3)
        .WillOnce(Return(-1))
        .WillOnce(Return(-1))
        .WillOnce(Return(100));

    EXPECT_CALL(mock_network, receive())
        .Times(1)
        .WillOnce(Return("OK"));

    HttpClient client(&mock_network);
    client.setRetryCount(3);
    EXPECT_TRUE(client.sendRequest("data"));
}
```

## 集成测试

### 测试服务器

```cpp
#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>
#include "test_server.hpp"

class IntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        server_ = std::make_unique<TestServer>(8080);
        server_->start();
    }

    void TearDown() override {
        server_->stop();
    }

    std::unique_ptr<TestServer> server_;
    falcon::DownloadEngine engine_;
};

TEST_F(IntegrationTest, DownloadCompleteWorkflow) {
    // 添加测试文件
    server_->addFile("/test.zip", "test content", 200);

    // 下载文件
    auto task = engine_.startDownload("http://localhost:8080/test.zip");
    task->wait();

    // 验证结果
    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Completed);
    EXPECT_TRUE(file_exists("/tmp/downloads/test.zip"));
}
```

### 多协议测试

```cpp
TEST_F(IntegrationTest, MultipleProtocols) {
    struct TestCase {
        std::string protocol;
        std::string url;
        bool should_succeed;
    };

    std::vector<TestCase> tests = {
        {"http", "http://localhost:8080/file.zip", true},
        {"https", "https://secure.example.com/file.zip", false},
        {"ftp", "ftp://localhost:21/file.zip", true}
    };

    for (const auto& test : tests) {
        auto task = engine_.startDownload(test.url);
        task->wait();

        if (test.should_succeed) {
            EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Completed);
        } else {
            EXPECT_NE(task->getStatus(), falcon::TaskStatus::Completed);
        }
    }
}
```

## 性能测试

使用 **Google Benchmark** 进行性能测试：

```cpp
#include <benchmark/benchmark.h>
#include <falcon/download_engine.hpp>

static void BM_HttpDownload(benchmark::State& state) {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    for (auto _ : state) {
        auto task = engine.startDownload("http://localhost:8080/file.zip");
        task->wait();
    }

    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_HttpDownload);

static void BM_MultiThreadDownload(benchmark::State& state) {
    falcon::DownloadEngine engine;
    engine.loadPlugin("http");

    falcon::DownloadOptions options;
    options.max_connections = state.range(0);

    for (auto _ : state) {
        auto task = engine.startDownload("http://localhost:8080/large.zip", options);
        task->wait();
    }
}

BENCHMARK(BM_MultiThreadDownload)->Range(1, 16);

BENCHMARK_MAIN();
```

## 运行测试

### 命令行

```bash
# 运行所有测试
ctest --test-dir build --output-on-failure

# 运行特定测试
./build/tests/libfalcon_tests --gtest_filter="DownloadEngineTest*"

# 运行特定测试套件
./build/tests/libfalcon_tests --gtest_filter="*Http*:*Ftp*"

# 重复运行（检测不稳定测试）
./build/tests/libfalcon_tests --gtest_repeat=10

# 随机顺序运行
./build/tests/libfalcon_tests --gtest_shuffle

# 显示详细输出
./build/tests/libfalcon_tests --gtest_print_time=1
```

### 测试覆盖率

```bash
# 启用覆盖率编译
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_ENABLE_COVERAGE=ON
cmake --build build

# 运行测试
ctest --test-dir build

# 生成覆盖率报告
cmake --build build --target coverage
```

## 测试最佳实践

### 1. 测试命名

```cpp
// 好：清晰的测试名称
TEST(DownloadEngineTest, StartDownload_ValidUrl_ReturnsValidTask)
TEST(DownloadEngineTest, StartDownload_InvalidUrl_ThrowsException)

// 避免：模糊的测试名称
TEST(DownloadEngineTest, Test1)
TEST(DownloadEngineTest, DownloadTest)
```

### 2. AAA 模式

```cpp
TEST(DownloadEngineTest, PauseDownload) {
    // Arrange（准备）
    falcon::DownloadEngine engine;
    auto task = engine.startDownload("https://example.com/file.zip");

    // Act（执行）
    engine.pauseTask(task->getId());

    // Assert（断言）
    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Paused);
}
```

### 3. 一个测试一个断言

```cpp
// 好：每个测试验证一个方面
TEST(DownloadEngineTest, StartDownload_ReturnsValidTask) {
    auto task = engine.startDownload("https://example.com/file.zip");
    EXPECT_NE(task, nullptr);
}

TEST(DownloadEngineTest, StartDownload_SetsStatusToPending) {
    auto task = engine.startDownload("https://example.com/file.zip");
    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Pending);
}

// 避免：一个测试多个断言
TEST(DownloadEngineTest, StartDownload) {
    auto task = engine.startDownload("https://example.com/file.zip");
    EXPECT_NE(task, nullptr);
    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Pending);
    EXPECT_EQ(task->getUrl(), "https://example.com/file.zip");
    // ... 更多断言
}
```

### 4. 使用自定义断言

```cpp
// 定义自定义断言
#define EXPECT_FILE_EXISTS(path) \
    do { \
        std::ifstream file(path); \
        EXPECT_TRUE(file.good()) << "File does not exist: " << path; \
    } while(0)

TEST(DownloadTest, FileCreated) {
    auto task = download("https://example.com/file.zip");
    task->wait();
    EXPECT_FILE_EXISTS("/tmp/downloads/file.zip");
}
```

### 5. 测试异常

```cpp
TEST(DownloadEngineTest, InvalidUrl_ThrowsException) {
    falcon::DownloadEngine engine;

    EXPECT_THROW(
        engine.startDownload("not-a-valid-url"),
        falcon::InvalidURLException
    );

    EXPECT_NO_THROW(
        engine.startDownload("https://example.com/file.zip")
    );
}
```

### 6. 测试超时

```cpp
// 设置测试超时
TEST(DownloadEngineTest, SlowDownload) {
    falcon::DownloadEngine engine;

    auto start = std::chrono::steady_clock::now();
    auto task = engine.startDownload("https://slow-server.com/file.zip");
    task->wait();
    auto end = std::chrono::steady_clock::now();

    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);
    EXPECT_LT(duration.count(), 60);  // 应该在 60 秒内完成
}
```

## 持续集成

### GitHub Actions 配置

```yaml
name: Tests

on: [push, pull_request]

jobs:
  test:
    runs-on: ${{ matrix.os }}
    strategy:
      matrix:
        os: [ubuntu-latest, macos-latest, windows-latest]
        build_type: [Debug, Release]

    steps:
    - uses: actions/checkout@v3

    - name: Install dependencies
      run: |
        ./vcpkg install spdlog nlohmann-json curl openssl gtest

    - name: Configure CMake
      run: |
        cmake -B build -S . \
          -DCMAKE_BUILD_TYPE=${{ matrix.build_type }} \
          -DFALCON_BUILD_TESTS=ON \
          -DCMAKE_TOOLCHAIN_FILE=${{ github.workspace }}/vcpkg/scripts/buildsystems/vcpkg.cmake

    - name: Build
      run: cmake --build build --config ${{ matrix.build_type }}

    - name: Run tests
      run: ctest --test-dir build --output-on-failure

    - name: Upload coverage
      if: matrix.os == 'ubuntu-latest' && matrix.build_type == 'Debug'
      uses: codecov/codecov-action@v3
```

::: tip 测试覆盖率目标
- 核心库 (libfalcon): ≥ 80%
- 关键路径: ≥ 90%
- 协议插件: ≥ 70%
:::
