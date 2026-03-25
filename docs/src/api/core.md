# 核心 API

本文档基于当前公共头文件说明 Falcon 的核心 C++ 接口。

## DownloadEngine

`DownloadEngine` 负责任务生命周期、并发控制、事件监听和协议处理器注册。

### 主要接口

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

### 基本示例

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

### 多任务示例

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

`DownloadOptions` 描述单个下载任务的行为。

### 当前字段

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

### 示例

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

`EngineConfig` 描述引擎级全局行为。

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

示例：

```cpp
falcon::EngineConfig config;
config.max_concurrent_tasks = 3;
config.global_speed_limit = 10 * 1024 * 1024;

falcon::DownloadEngine engine(config);
```

## DownloadTask

`DownloadTask` 表示单个下载任务。

### 主要接口

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

### 示例

```cpp
auto task = engine.add_task("https://example.com/file.zip");
engine.start_task(task->id());

task->wait();

if (task->status() == falcon::TaskStatus::Completed) {
    std::cout << task->output_path() << std::endl;
}
```

## TaskStatus

当前任务状态枚举定义在 `event_listener.hpp`：

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

## 基础类型

常用基础类型定义在 `types.hpp`：

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
