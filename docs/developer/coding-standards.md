# Coding Standards

This document defines the coding standards for the Falcon project.

## C++ Standard

- **Minimum requirement**: C++17
- **Current status**: C++17 across the entire codebase (the root CMakeLists pins `CMAKE_CXX_STANDARD 17`, and every target also declares `cxx_std_17`)

## Naming Conventions

### File Naming

```
// File names use snake_case
download_engine.hpp
download_engine.cpp
http_plugin.cpp
```

### Class Naming

```cpp
// Class names use PascalCase
class DownloadEngine {
};

class HttpClient {
};

class BittorrentPlugin {
};
```

### Function Naming

```cpp
// Functions use snake_case
void start_download();
int get_task_count();
bool is_valid_url();
```

### Variable Naming

```cpp
// Local variables and parameters: snake_case
void func(int connection_count, std::string url) {
    int file_size = 0;
    bool is_connected = false;
}

// Member variables: trailing underscore
class MyClass {
private:
    int task_count_;
    std::string config_path_;
    bool is_initialized_;
};

// Constants and static constants: k prefix + PascalCase
class MyClass {
private:
    static constexpr int kMaxRetries = 3;
    static constexpr const char* kDefaultUserAgent = "Falcon/1.0";
};

// File-level constants also use the k prefix
constexpr int kMaxConnections = 100;
constexpr double kDefaultTimeout = 30.0;
constexpr size_t kBufferSize = 4096;
```

### Namespaces

```cpp
// Lowercase, short
namespace falcon {
namespace http {
namespace internal {

}
}
}
```

### Enumerations

```cpp
// Enum classes use PascalCase (the real TaskStatus values)
enum class TaskStatus {
    Pending,
    Preparing,
    Downloading,
    Paused,
    Completed,
    Failed,
    Cancelled
};

// Strongly-typed enum values use PascalCase
auto status = TaskStatus::Downloading;
```

### Type Aliases

```cpp
// Type aliases use PascalCase
using TaskId = uint64_t;
using DownloadCallback = std::function<void(const ProgressInfo&)>;
using PluginMap = std::unordered_map<std::string, std::unique_ptr<IProtocolHandler>>;
```

## Code Organization

### Header File Structure

```cpp
// 1. License and copyright notice (optional)
// 2. Header guard
#pragma once

// 3. Include the corresponding C standard library headers
#include <cstdint>
#include <cstring>

// 4. Include C++ standard library headers
#include <memory>
#include <string>
#include <vector>

// 5. Include third-party libraries
#include <curl/curl.h>

// 6. Include project-internal headers
#include <falcon/types.hpp>
#include <falcon/protocol_handler.hpp>

// 7. Namespace declaration
namespace falcon {

// 8. Forward declarations
class DownloadTask;
struct DownloadOptions;

// 9. Class definition
class DownloadEngine {
    // ...
};

} // namespace falcon
```

### Source File Structure

```cpp
// 1. Include the corresponding header
#include <falcon/download_engine.hpp>

// 2. Include C standard library headers
#include <cstring>

// 3. Include C++ standard library headers
#include <algorithm>
#include <iostream>

// 4. Include third-party libraries
#include <spdlog/spdlog.h>

// 5. Include project-internal headers
#include <falcon/task_manager.hpp>
#include <falcon/event_dispatcher.hpp>

// 6. Anonymous namespace (internal linkage)
namespace {

constexpr int kDefaultTimeout = 30;
std::string log_prefix = "[DownloadEngine]";

} // namespace

// 7. Namespace
namespace falcon {

// 8. Static member definitions
const int DownloadEngine::kMaxRetries = 3;

// 9. Member function definitions
DownloadEngine::DownloadEngine()
    : task_manager_(std::make_unique<TaskManager>()) {
}

// 10. Free functions
void helper_function() {
    // ...
}

} // namespace falcon
```

## Comment Conventions

### File Comments

```cpp
/**
 * @file download_engine.hpp
 * @brief Download engine class definition
 * @author Falcon Team
 * @date 2025-12-25
 * @copyright Apache License 2.0
 */
```

### Class Comments

```cpp
/**
 * @class DownloadEngine
 * @brief High-performance download engine
 *
 * Provides multi-protocol download support, including HTTP, FTP, BitTorrent, and more.
 * Supports resumable downloads, multi-threaded downloads, speed limits, and other features.
 *
 * @example
 * @code
 * DownloadEngine engine;
 * auto task = engine.add_task("https://example.com/file.zip");
 * if (task) {
 *     engine.start_task(task->id());
 *     task->wait();
 * }
 * @endcode
 */
class DownloadEngine {
};
```

### Function Comments

```cpp
/**
 * @brief Add a download task
 *
 * Creates a download task from the given URL and options.
 * If no plugin supports the URL, nullptr is returned.
 *
 * @param url The download URL; supports http/https/ftp/magnet and more
 * @param options Download options, including connection count, save path, etc.
 * @return std::shared_ptr<DownloadTask> the task object, nullptr on failure
 *
 * @throws InvalidURLException When the URL is malformed
 * @throws UnsupportedProtocolException When no supported protocol handler exists
 *
 * @note After adding a task, call start_task(task->id()) to start it
 *
 * @see DownloadTask
 * @see DownloadOptions
 */
DownloadTask::Ptr add_task(const std::string& url,
                           const DownloadOptions& options = {});
```

### Member Variable Comments

```cpp
class DownloadEngine {
private:
    /// Task manager, responsible for the lifecycle of all tasks
    std::unique_ptr<TaskManager> task_manager_;

    /// Protocol registry, responsible for loading and routing protocol handlers
    ProtocolRegistry protocol_registry_;

    /// Event dispatcher, handles callback dispatch
    EventDispatcher event_dispatcher_;

    /// Number of currently running tasks
    std::atomic<int> active_tasks_;
};
```

### Inline Comments

```cpp
// Use regular comments to explain code intent
int result = calculate();  // Single-line comments go after the statement

// Multi-line comments:
// Add an explanation before complex logic
// 1. First check the connection status
// 2. Then send the request
// 3. Finally process the response
```

## Formatting

Use **clang-format** for automatic formatting, configured via `.clang-format`:

```yaml
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
```

### Automatic Formatting

```bash
# Format all files
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i

# Check formatting
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format --dry-run --Werror
```

## Best Practices

### RAII

```cpp
// Good: manage resources with RAII
class FileHandle {
public:
    FileHandle(const std::string& path) : fd_(open(path.c_str(), O_RDONLY)) {
        if (fd_ < 0) {
            throw std::runtime_error("Failed to open file");
        }
    }

    ~FileHandle() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    // Forbid copying
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // Allow moving
    FileHandle(FileHandle&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

private:
    int fd_;
};
```

### Smart Pointers

```cpp
// Exclusive ownership
std::unique_ptr<DownloadTask> task = std::make_unique<DownloadTask>();

// Shared ownership
std::shared_ptr<DownloadEngine> engine = std::make_shared<DownloadEngine>();

// Weak reference, avoids reference cycles
std::weak_ptr<DownloadTask> weak_task = task;
```

### Exception Safety

```cpp
// Good: RAII, exception-safe
std::lock_guard<std::mutex> lock(mutex_);

// Good: unique_lock, for manual lock control
std::unique_lock<std::mutex> lock(mutex_);
// ... do some work
lock.unlock();

// Avoid: raw pointers and manual resource management
// int* ptr = new int(42);
// delete ptr;  // may be forgotten or skipped when an exception is thrown
```

### const Correctness

```cpp
// Good: mark as const
int get_value() const { return value_; }
bool is_empty() const { return items_.empty(); }
const std::string& get_name() const { return name_; }

// Good: pass by const reference
void process(const std::string& input);
void update(const Config& config);
```

### Default Arguments

```cpp
// Good: use default arguments
void download(const std::string& url,
              const DownloadOptions& options = {},
              IEventListener* listener = nullptr);

// Avoid: overloading a function just to provide default values
void download(const std::string& url);
void download(const std::string& url, const DownloadOptions& options);
```

## Forbidden Practices

### Things Not To Do

```cpp
// 1. Do not use C-style casts
// char* ptr = (char*)malloc(100);

// Good: use C++ casts
char* ptr = static_cast<char*>(malloc(100));

// 2. Do not use NULL
// void* ptr = NULL;

// Good: use nullptr
void* ptr = nullptr;

// 3. Do not use "using namespace std;"
// using namespace std;

// Good: use specific using declarations, or none at all
using std::vector;

// 4. Do not use bool as a bitfield
// struct Flag { unsigned int ready : 1; };

// 5. Do not use using-directives in headers
// using namespace std;

// 6. Do not use global variables
// int global_counter = 0;

// Good: use constants inside a namespace or static members
namespace { const int kGlobalCounter = 0; }

// 7. Do not use magic numbers
// if (size > 1073741824) { }

// Good: use named constants
constexpr int k1GB = 1073741824;
if (size > k1GB) { }
```

::: tip Code review
Before submitting code, make sure it follows the standards above. Check formatting with `clang-format --dry-run --Werror`.
:::
