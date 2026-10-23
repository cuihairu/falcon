# Core API

This document describes Falcon's core C++ interfaces as defined by the current public headers.

## DownloadEngine

`DownloadEngine` owns the task lifecycle, concurrency control, event listeners, and protocol-handler registration.

### Main interface

```cpp
class DownloadEngine {
public:
    DownloadEngine();
    explicit DownloadEngine(const EngineConfig& config);
    ~DownloadEngine();

    DownloadTask::Ptr add_task(
        const std::string& url,
        const DownloadOptions& options = {});

    std::vector<DownloadTask::Ptr> add_tasks(
        const std::vector<std::string>& urls,
        const DownloadOptions& options = {});

    DownloadTask::Ptr get_task(TaskId id) const;
    std::vector<DownloadTask::Ptr> get_all_tasks() const;
    std::vector<DownloadTask::Ptr> get_tasks_by_status(TaskStatus status) const;
    std::vector<DownloadTask::Ptr> get_active_tasks() const;

    bool remove_task(TaskId id);
    std::size_t remove_finished_tasks();

    bool start_task(TaskId id);
    bool pause_task(TaskId id);
    bool resume_task(TaskId id);
    bool cancel_task(TaskId id);

    void pause_all();
    void resume_all();
    void cancel_all();
    void wait_all();

    void register_handler(std::unique_ptr<IProtocolHandler> handler);
    void register_handler_factory(ProtocolHandlerFactory factory);
    std::vector<std::string> get_supported_protocols() const;
    bool is_url_supported(const std::string& url) const;

    void add_listener(IEventListener* listener);
    void remove_listener(IEventListener* listener);

    const EngineConfig& config() const noexcept;
    void set_global_speed_limit(BytesPerSecond bytes_per_second);
    void set_max_concurrent_tasks(std::size_t max_tasks);

    BytesPerSecond get_total_speed() const;
    std::size_t get_active_task_count() const;
    std::size_t get_total_task_count() const;
};
```

### Basic example

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.output_directory = "./downloads";
    options.max_connections = 4;

    auto task = engine.add_task("https://example.com/file.zip", options);
    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

### Multiple tasks

```cpp
falcon::DownloadEngine engine;
falcon::DownloadOptions options;
options.max_connections = 3;

auto tasks = engine.add_tasks({
    "https://example.com/file1.zip",
    "https://example.com/file2.zip",
    "https://example.com/file3.zip",
}, options);

for (const auto& task : tasks) {
    engine.start_task(task->id());
}

engine.wait_all();
```

## DownloadOptions

`DownloadOptions` describes the behavior of a single download task.

### Current fields

```cpp
struct DownloadOptions {
    std::size_t max_connections = 4;
    std::size_t timeout_seconds = 30;
    std::size_t max_retries = 3;
    std::size_t retry_delay_seconds = 1;

    std::string output_directory = ".";
    std::string output_filename;

    std::size_t speed_limit = 0;
    bool resume_enabled = true;

    std::string user_agent = "Falcon/0.1.0";
    std::map<std::string, std::string> headers;

    std::string proxy;
    std::string proxy_username;
    std::string proxy_password;
    std::string proxy_type;

    bool verify_ssl = true;
    std::string referer;
    std::string cookie_file;
    std::string cookie_jar;
    std::string http_username;
    std::string http_password;

    std::size_t min_segment_size = 1024 * 1024;
    bool adaptive_segment_sizing = true;
    std::size_t progress_interval_ms = 500;
    bool create_directory = true;
    bool overwrite_existing = false;
};
```

### Example

```cpp
falcon::DownloadOptions options;
options.output_directory = "./downloads";
options.output_filename = "archive.zip";
options.max_connections = 8;
options.speed_limit = 1024 * 1024;
options.proxy = "http://127.0.0.1:7890";
options.headers["Authorization"] = "Bearer TOKEN";
```

## EngineConfig

`EngineConfig` describes global engine behavior.

```cpp
struct EngineConfig {
    std::size_t max_concurrent_tasks = 5;
    std::size_t global_speed_limit = 0;
    bool enable_disk_cache = true;
    std::size_t disk_cache_size = 4 * 1024 * 1024;
    int log_level = 3;
    std::string temp_extension = ".falcon.tmp";
    bool auto_start = true;
};
```

Example:

```cpp
falcon::EngineConfig config;
config.max_concurrent_tasks = 3;
config.global_speed_limit = 10 * 1024 * 1024;

falcon::DownloadEngine engine(config);
```

## DownloadTask

`DownloadTask` represents a single download task.

### Main interface

```cpp
class DownloadTask {
public:
    TaskId id() const noexcept;
    const std::string& url() const noexcept;
    TaskStatus status() const noexcept;
    float progress() const noexcept;
    Bytes total_bytes() const noexcept;
    Bytes downloaded_bytes() const noexcept;
    BytesPerSecond speed() const noexcept;
    const std::string& output_path() const noexcept;
    const DownloadOptions& options() const noexcept;
    const FileInfo& file_info() const noexcept;
    const std::string& error_message() const noexcept;

    bool pause();
    bool resume();
    bool cancel();
    void wait();
    bool wait_for(Duration timeout);
};
```

### Example

```cpp
auto task = engine.add_task("https://example.com/file.zip");
engine.start_task(task->id());

task->wait();

if (task->status() == falcon::TaskStatus::Completed) {
    std::cout << task->output_path() << std::endl;
}
```

## TaskStatus

The current task-status enum is defined in `event_listener.hpp`:

```cpp
enum class TaskStatus {
    Pending,
    Preparing,
    Downloading,
    Paused,
    Completed,
    Failed,
    Cancelled
};
```

## Fundamental types

Common fundamental types are defined in `types.hpp`:

```cpp
using TaskId = std::uint64_t;
using Bytes = std::uint64_t;
using BytesPerSecond = std::uint64_t;
using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

struct FileInfo {
    std::string url;
    std::string filename;
    std::string content_type;
    Bytes total_size = 0;
    bool supports_resume = false;
    TimePoint last_modified{};
};

struct ProgressInfo {
    TaskId task_id = INVALID_TASK_ID;
    Bytes downloaded_bytes = 0;
    Bytes total_bytes = 0;
    Speed speed = 0;
    float progress = 0.0f;
    Duration elapsed{};
    Duration estimated_remaining{};
};
```
