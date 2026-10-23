# Developer Guide

Welcome to the Falcon developer guide! This document is aimed at developers who want to contribute code or build on top of Falcon.

## Table of Contents

- [Setting Up the Development Environment](#setting-up-the-development-environment)
- [Project Architecture](#project-architecture)
- [Coding Standards](#coding-standards)
- [Testing Guide](#testing-guide)
- [Plugin Development](#plugin-development)
- [Submitting Code](#submitting-code)

## Setting Up the Development Environment

### Prerequisites

- **CMake** 3.15+
- **C++ compiler** (GCC 7+, Clang 5+, MSVC 2017+)
- **Git**
- **vcpkg** (recommended) or a system package manager

### Getting the Source

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
```

### Installing Dependencies

#### Using vcpkg

```bash
# Install vcpkg
git clone https://github.com/Microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.sh

# Install dependencies
./vcpkg install spdlog nlohmann-json curl openssl gtest
```

#### System Package Manager

```bash
# macOS
brew install spdlog nlohmann-json curl openssl googletest cmake

# Ubuntu/Debian
sudo apt install build-essential cmake git \
  libspdlog-dev nlohmann-json3-dev libcurl4-openssl-dev \
  libssl-dev libgtest-dev
```

### Building the Project

```bash
# Debug mode (development)
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON \
  -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build

# Run the tests
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
  ],
  "C_Cpp.default.configurationProvider": "ms-vscode.cmake-tools"
}
```

#### CLion

File → Settings → Build, Execution, Deployment → CMake
- CMake options: `-DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake`
- Build directory: `build`

## Project Architecture

### Monorepo Layout

```
falcon/
├── packages/
│   ├── libfalcon-core/        # Core download engine
│   ├── libfalcon-protocols/   # Standard download protocols
│   ├── libfalcon-storage/     # Object storage and resource browsing
│   ├── libfalcon-drives/      # Cloud drives and cloud storage
│   ├── falcon-cli/            # Command-line tool
│   └── falcon-daemon/         # Background daemon
├── apps/
│   └── desktop/               # Qt6 desktop application
└── docs/                      # Documentation
```

### Library Dependencies

```
falcon_protocols  →  falcon_core
falcon_storage    →  falcon_core
falcon_drives     →  falcon_core
```

Reverse dependencies are forbidden: `core` does not depend on `protocols/storage/drives`.

### Core Components

| Component | Library | Responsibility |
|-----------|---------|----------------|
| `DownloadEngine` | core | Download engine core |
| `TaskManager` | core | Task manager |
| `ProtocolRegistry` | core | Protocol handler registration and routing |
| `IProtocolHandler` | core | Protocol handler interface |
| `HttpHandler` | protocols | HTTP/HTTPS protocol |
| `SegmentDownloader` | protocols | Multi-threaded segmented downloads |
| `S3Browser` | storage | S3 resource browsing |
| `CloudStorageManager` | drives | Legacy abstraction for cloud drives and share links |

::: tip Full architecture
See the [architecture design document](./architecture.md) for more details.
:::

## Coding Standards

### C++ Style

Follow the **Google C++ Style Guide**:

```cpp
// Class names: PascalCase
class DownloadEngine {
public:
    // Function names: snake_case
    bool start_task(TaskId id);

    // Member variables: trailing underscore
    int task_count_;

private:
    std::string config_path_;
};

// Constants: k prefix + PascalCase
constexpr int kMaxRetries = 5;
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
 * Supports multiple protocols, multi-threading, resumable downloads, and more
 */
class DownloadEngine {
public:
    /**
     * @brief Add a download task
     *
     * @param url Download URL
     * @param options Download options
     * @return Task object, nullptr on failure
     *
     * @throws InvalidURLException When the URL is malformed
     */
    DownloadTask::Ptr add_task(const std::string& url,
                               const DownloadOptions& options = {});
};
```

### Formatting

```bash
# Format all files
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i
```

::: tip Full standards
See the [coding standards document](./coding-standards.md) for more details.
:::

## Testing Guide

### Unit Tests

The project uses the **Google Test** framework:

```cpp
#include <gtest/gtest.h>
#include <falcon/download_engine.hpp>

TEST(DownloadEngineTest, AddTask) {
    falcon::DownloadEngine engine;
    auto task = engine.add_task("https://example.com/file.zip");
    EXPECT_NE(task, nullptr);
}
```

### Running Tests

```bash
# Run all tests
ctest --test-dir build

# Run a specific test
./build/bin/falcon_core_tests --gtest_filter="DownloadEngineTest*"

# Test coverage: reconfigure with coverage instrumentation, then build and test
cmake -B build-cov -S . -DFALCON_ENABLE_COVERAGE=ON -DFALCON_BUILD_TESTS=ON
cmake --build build-cov
ctest --test-dir build-cov --output-on-failure

# Generate a report with gcovr (installed separately)
gcovr -r . --filter 'packages/' build-cov
```

### Pre-Submission Checks

```bash
# Format check
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format --dry-run --Werror

# Static analysis
clang-tidy -p build <source-file>

# Run all tests
ctest --test-dir build --output-on-failure
```

::: tip Full testing guide
See the [testing document](./testing.md) for more details.
:::

## Plugin Development

### Creating a New Protocol Plugin

1. **Define the plugin interface**:

```cpp
// plugins/myprotocol/myprotocol_plugin.hpp
#pragma once
#include <falcon/protocol_handler.hpp>

class MyProtocolPlugin : public IProtocolHandler {
public:
    std::string protocol_name() const override { return "myproto"; }

    std::vector<std::string> supported_schemes() const override {
        return {"myproto"};
    }

    bool can_handle(const std::string& url) const override {
        return url.find("myproto://") == 0;
    }

    FileInfo get_file_info(const std::string& url,
                           const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task,
                  IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;
    void resume(DownloadTask::Ptr task, IEventListener* listener) override;
    void cancel(DownloadTask::Ptr task) override;
};
```

2. **Implement the plugin logic**:

```cpp
// plugins/myprotocol/myprotocol_plugin.cpp
#include "myprotocol_plugin.hpp"

void MyProtocolPlugin::download(DownloadTask::Ptr task,
                                IEventListener* listener) {

    // Parse the URL
    // Establish the connection
    // Download the data
    // Report progress
}
```

3. **Register the handler**:

```cpp
falcon::DownloadEngine engine;
engine.register_handler(std::make_unique<MyProtocolPlugin>());
```

::: tip Full plugin development guide
See the [plugin development document](./plugins.md) for more details.
:::

## Desktop Application Development

### Technology Stack

The desktop application is built with the **Qt6** framework:

- **CMake**: Build system
- **Qt6 Widgets**: UI framework
- **QSS**: Style sheets (light/dark themes supported)
- **falcon_core**: Core download library

### Directory Layout

```
apps/desktop/
├── src/
│   ├── main.cpp               # Entry point
│   ├── main_window.{hpp,cpp}  # Main window
│   ├── widgets/               # UI components
│   │   ├── top_bar.hpp        # Top toolbar
│   │   └── status_bar.hpp     # Bottom status bar
│   ├── navigation/            # Navigation
│   │   └── sidebar.hpp        # Side navigation bar
│   ├── pages/                 # Pages
│   │   ├── download_page.hpp  # Download management
│   │   ├── cloud_page.hpp     # Cloud drive management
│   │   ├── discovery_page.hpp # Resource discovery
│   │   └── settings_page.hpp  # Settings
│   ├── dialogs/               # Dialogs
│   ├── services/              # Service layer
│   │   ├── search_service.hpp # Search service
│   │   └── storage_service.hpp # Storage service
│   └── utils/                 # Utilities
│       ├── theme_manager.hpp  # Theme management
│       ├── clipboard_monitor.hpp
│       └── url_detector.hpp    # URL detection (including cloud drives)
└── CMakeLists.txt
```

### Building the Desktop Application

```bash
# Install Qt6
# Ubuntu
sudo apt install qt6-base-dev qt6-tools-dev

# macOS
brew install qt@6

# Configure CMake
cmake -B build -S . \
  -DFALCON_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x.x/gcc_64

# Build
cmake --build build --target falcon-desktop

# Run
./build/bin/falcon-desktop
```

### Main Features

| Feature | Description |
|---------|-------------|
| Grid/table view switching | Task list supports both card and table display modes |
| System tray integration | Closing minimizes to the tray |
| Theme switching | Light/dark themes, switchable at runtime |
| Cloud drive link detection | Supports Baidu Netdisk, Aliyun Drive, Quark, and more |
| Clipboard monitoring | Automatically detects download links in the clipboard |
| IPC server | Receives download requests from the browser extension |

## Submitting Code

### Commit Message Conventions

Follow **Conventional Commits**:

```
feat: add xxx feature
fix: fix yyy issue
docs: update zzz docs
test: add aaa tests
refactor: refactor bbb module
perf: improve ccc performance
```

### Pull Request Workflow

1. Fork the project
2. Create a feature branch: `git checkout -b feature/my-feature`
3. Commit your changes: `git commit -m "feat: add my feature"`
4. Push the branch: `git push origin feature/my-feature`
5. Create a Pull Request

### PR Checklist

- [ ] Code follows the coding standards
- [ ] All tests pass
- [ ] Necessary tests were added
- [ ] Related documentation was updated
- [ ] Commit messages are clear and descriptive

## Debugging Tips

### Using gdb

```bash
# Build a Debug version
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build

# Start gdb
gdb ./build/bin/falcon-cli
```

### Using lldb (macOS)

```bash
lldb ./build/bin/falcon-cli
```

### Log-Based Debugging

```cpp
FALCON_LOG_DEBUG("debug info: " << value);
FALCON_LOG_INFO("info message");
FALCON_LOG_WARN("warning message");
FALCON_LOG_ERROR("error message");
```

::: tip Full debugging guide
See the [debugging document](./debugging.md) for more details.
:::

## Getting Help

- **GitHub Issues**: [https://github.com/cuihairu/falcon/issues](https://github.com/cuihairu/falcon/issues)
- **Discussions**: [https://github.com/cuihairu/falcon/discussions](https://github.com/cuihairu/falcon/discussions)
- **Wiki**: [https://github.com/cuihairu/falcon/wiki](https://github.com/cuihairu/falcon/wiki)

::: tip Contributions welcome
Thank you for your interest in Falcon! We welcome contributions of any kind.
:::
