[中文](developer_guide.zh.md) | **English**

# Falcon Developer Guide

> [!WARNING]
> This document is an archived version of the old developer guide.
> For the currently maintained developer documentation, refer to the pages under `docs/src/developer/`.

## Setting Up the Development Environment

### Prerequisites

- **CMake** 3.15+
- **C++ compiler** (GCC 7+, Clang 5+, MSVC 2017+)
- **vcpkg** (optional, for dependency management)

### Obtaining the Source Code

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
```

### Installing Dependencies (with vcpkg)

```bash
# macOS/Linux
./vcpkg install spdlog nlohmann-json curl openssl gtest

# Windows
vcpkg install spdlog nlohmann-json curl openssl gtest
```

### Building the Project

```bash
# Configure CMake
cmake -B build -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build --config Release

# Run tests
ctest --test-dir build --output-on-failure
```

### IDE Configuration

#### VS Code

Install the extensions:
- C/C++ Extension Pack
- CMake Tools

Configure `.vscode/settings.json`:
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

## Project Structure

```
falcon/
├── packages/
│   ├── libfalcon/          # Core library
│   │   ├── include/falcon/ # Public headers
│   │   ├── src/            # Implementation files
│   │   └── tests/          # Unit tests
│   ├── falcon-cli/         # Command-line tool
│   └── falcon-daemon/      # Daemon
├── apps/                   # Application layer
│   ├── desktop/            # Desktop application
│   └── browser_extension/  # Browser extension
├── docs/                   # Documentation
└── examples/               # Example code
```

## Coding Standards

### C++ Style

Follow the **Google C++ Style Guide**:

```cpp
// Class names: PascalCase
class DownloadEngine {
public:
    // Function names: snake_case
    void add_download(const std::string& url);

    // Member variables: trailing underscore
    int task_count_;

private:
    // Private members: trailing underscore
    std::string config_path_;
};

// Constants: UPPER_SNAKE_CASE
const int MAX_RETRIES = 5;

// File names: snake_case
// download_engine.cpp
```

### Namespaces

```cpp
namespace falcon {
namespace net {

class EventPoll {
    // ...
};

} // namespace net
} // namespace falcon
```

### Comment Conventions

```cpp
/**
 * @file download_engine.hpp
 * @brief Download engine class definition
 * @author Falcon Team
 * @date 2025-12-25
 */

/**
 * @class DownloadEngine
 * @brief High-performance download engine
 *
 * Supports multi-protocol, multi-threaded downloading, resumable
 * transfers, and more.
 *
 * @example
 * DownloadEngine engine;
 * engine.add_download("http://example.com/file.zip");
 * engine.run();
 */
class DownloadEngine {
public:
    /**
     * @brief Add a download task
     *
     * @param url The download URL
     * @param options Download options (optional)
     * @return The task ID
     *
     * @throws InvalidURLException when the URL format is invalid
     *
     * @code
     * TaskId id = engine.add_download("http://example.com/file.zip");
     * @endcode
     */
    TaskId add_download(const std::string& url,
                       const DownloadOptions& options = {});
};
```

### Formatting

Use **clang-format** for automatic formatting:

```bash
# Format all files
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
```

Configuration file `.clang-format`:
```yaml
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
```

## Development Workflow

### 1. Create a Feature Branch

```bash
git checkout -b feature/my-feature
```

### 2. Write Code

- Add public APIs to `include/falcon/`
- Add implementations to `src/`
- Add tests to `tests/unit/` or `tests/integration/`

### 3. Write Tests

```cpp
// tests/unit/my_feature_test.cpp
#include <gtest/gtest.h>
#include <falcon/my_feature.hpp>

TEST(MyFeatureTest, BasicOperation) {
    falcon::MyFeature feature;
    EXPECT_TRUE(feature.do_something());
}
```

### 4. Run Tests

```bash
# Run all tests
ctest --test-dir build

# Run a specific test
./build/bin/falcon_core_tests --gtest_filter="MyFeatureTest*"

# View test coverage
cmake --build build --target coverage
```

### 5. Commit Code

```bash
git add .
git commit -m "feat: add new feature XXX"
git push origin feature/my-feature
```

### Commit Message Conventions

Follow **Conventional Commits**:

```
feat: add XXX feature
fix: fix YYY issue
docs: update ZZZ documentation
test: add AAA tests
refactor: refactor BBB module
perf: optimize CCC performance
```

## Testing Guide

### Unit Tests

Use the **Google Test** framework:

```cpp
#include <gtest/gtest.h>

class MyTestFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Initialize before each test
    }

    void TearDown() override {
        // Clean up after each test
    }

    falcon::MyClass* obj_;
};

TEST_F(MyTestFixture, TestCase1) {
    EXPECT_EQ(obj_->method(), expected_value);
}
```

### Mock Tests

Use **GMock** for dependency isolation:

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

### Integration Tests

Test the complete workflow:

```cpp
TEST(IntegrationTest, DownloadCompleteWorkflow) {
    // 1. Create a test server
    TestServer server(8080);
    server.start();

    // 2. Create a download engine
    DownloadEngineV2 engine;
    TaskId id = engine.add_download("http://localhost:8080/test.zip");

    // 3. Run the download
    engine.run();

    // 4. Verify the result
    EXPECT_TRUE(file_exists("/tmp/downloads/test.zip"));
}
```

## Debugging Techniques

### Using gdb

```bash
# Build a Debug version
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Start gdb
gdb ./build/bin/falcon-cli

# gdb commands
(gdb) break main
(gdb) run
(gdb) next
(gdb) print variable_name
(gdb) continue
```

### Using lldb (macOS/LLVM)

```bash
lldb ./build/bin/falcon-cli

# lldb commands
(lldb) breakpoint set --name main
(lldb) run
(lldb) next
(lldb) expr variable_name
(lldb) continue
```

### Log Debugging

```cpp
// Use the logging macros
FALCON_LOG_DEBUG("Debug info: " << value);
FALCON_LOG_INFO("Normal info");
FALCON_LOG_WARN("Warning info");
FALCON_LOG_ERROR("Error info");

// Set the log level
// export FALCON_LOG_LEVEL=DEBUG
```

### Valgrind Memory Checks

```bash
# Build with symbols
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_ENABLE_ASAN=OFF

# Run Valgrind
valgrind --leak-check=full --show-leak-kinds=all ./build/bin/falcon-cli
```

### AddressSanitizer

```bash
# Enable ASan
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_ENABLE_ASAN=ON

# Run the program
./build/bin/falcon-cli
```

## Performance Analysis

### Using perf (Linux)

```bash
# Record performance data
perf record -g ./build/bin/falcon-cli

# View the report
perf report

# Visualize a flame graph
perf script | ./FlameGraph/flamegraph.pl > flamegraph.svg
```

### Using Instruments (macOS)

```bash
# Launch Instruments
instruments -t "Time Profiler" ./build/bin/falcon-cli
```

### Benchmarks

Use **Google Benchmark**:

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

## Plugin Development

### Creating a New Protocol Plugin

1. **Define the plugin interface**:

```cpp
// plugins/myprotocol/myprotocol_plugin.hpp
#pragma once
#include <falcon/protocol_handler.hpp>

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

2. **Implement the plugin logic**:

```cpp
// plugins/myprotocol/myprotocol_plugin.cpp
#include "myprotocol_plugin.hpp"

std::shared_ptr<DownloadTask> MyProtocolPlugin::download(
    const std::string& url,
    const DownloadOptions& options,
    IEventListener* listener) {

    // Parse the URL
    // Establish the connection
    // Download the data
    // Report progress
}
```

3. **Register the plugin**:

```cpp
// src/plugin_manager.cpp
void PluginManager::load_builtin_plugins() {
    register_plugin(std::make_unique<MyProtocolPlugin>());
}
```

## Release Process

### Version Numbering

Follow **Semantic Versioning**:

```
MAJOR.MINOR.PATCH

1.0.0 - Initial stable release
1.1.0 - New features added (backward compatible)
1.1.1 - Bug fixes
2.0.0 - Breaking changes
```

### Release Steps

1. **Update the version number**:

```cpp
// include/falcon/version.hpp
#define FALCON_VERSION_MAJOR 1
#define FALCON_VERSION_MINOR 0
#define FALCON_VERSION_PATCH 0
```

2. **Update the ChangeLog**:

```markdown
## [1.0.0] - 2025-12-25

### Added
- XXX feature
- YYY feature

### Fixed
- Fix ZZZ issue
```

3. **Create a Git tag**:

```bash
git tag -a v1.0.0 -m "Release version 1.0.0"
git push origin v1.0.0
```

4. **Build the release package**:

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

### Continuous Integration

GitHub Actions configuration in `.github/workflows/ci.yml`:

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

## Frequently Asked Questions

### Q: How do I add a new dependency?

A: Add it in `CMakeLists.txt`:

```cmake
find_package(SomePackage REQUIRED)
target_link_libraries(falcon PRIVATE SomePackage::SomePackage)
```

### Q: How do I debug link errors?

A: Use verbose mode:

```bash
cmake --build build --verbose
```

### Q: How do I fix compilation errors?

A: Check the following:
1. Whether the C++ standard version matches
2. Whether the headers are included correctly
3. Whether the libraries are linked correctly

## Community Resources

- **GitHub**: https://github.com/cuihairu/falcon
- **Issues**: https://github.com/cuihairu/falcon/issues
- **Discussions**: https://github.com/cuihairu/falcon/discussions
- **Wiki**: https://github.com/cuihairu/falcon/wiki

## License

Apache License 2.0
