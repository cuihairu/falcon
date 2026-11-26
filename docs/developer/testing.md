# Testing Guide

This document explains how tests are organized in the current repository and which interfaces new tests should target.

## Test directories

```text
packages/libfalcon-core/tests/
packages/libfalcon-protocols/tests/
packages/libfalcon-storage/tests/
packages/libfalcon-drives/tests/
packages/falcon-daemon/tests/
```

These libraries currently contain:

- `unit/`
- `integration/`
- `performance/`

## Building tests

The recommended path is the vcpkg manifest build. The repository-root `vcpkg.json` declares `gtest`, so configuring with the vcpkg toolchain installs the test dependency automatically.

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
```

Verify the minimal stable surface of the core library:

```bash
./build-vcpkg-core/bin/falcon_core_tests --gtest_brief=1
./build-vcpkg-core/bin/falcon_split_tests --gtest_brief=1
```

Current verified results:

- `falcon_core_tests`: 384 tests passed
- `falcon_split_tests`: 10 tests passed

To use system dependencies instead of vcpkg:

```bash
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug -DFALCON_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

A bare CMake configuration does not install dependencies from the vcpkg manifest. If `GTest` is not preinstalled, the repository skips the test directories rather than fetching dependencies over the network with `FetchContent`.

Common installation commands:

```bash
# macOS (Homebrew)
brew install googletest

# Ubuntu / Debian
sudo apt-get install libgtest-dev
```

After installation, rerun the CMake configuration and build commands above.

## Interface baseline for tests

New tests should target these public symbols:

- `DownloadEngine::add_task()`
- `DownloadEngine::start_task()`
- `DownloadEngine::pause_task()`
- `DownloadEngine::resume_task()`
- `DownloadEngine::cancel_task()`
- `DownloadTask::status()`
- `DownloadTask::id()`

Do not use `startDownload()`, `loadPlugin()`, or `getStatus()` as defaults in new test examples.

## Basic examples

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

## Test fixture

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

## Parameterized tests

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

## Integration-test guidance

Integration tests are a better fit for verifying:

- Whether a real URL routes to the correct handler.
- The transition of a download task from `Pending` to `Completed`.
- Resume and output-path behavior.
- Active-task counts and global concurrency limits.

Example:

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

## Coverage

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON \
  -DFALCON_ENABLE_COVERAGE=ON

cmake --build build
ctest --test-dir build
```

## Best practices

- Test real interfaces from the public headers; do not extend new examples around historical design drafts.
- Prefer stable behavior: state transitions, URL support checks, output paths, and concurrency limits.
- Use local test servers or fixtures for network-dependent tests to avoid external-network flakiness.
- When a plugin is disabled by default, state the required build-option precondition in the test.
