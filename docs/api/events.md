# Events API

Falcon's current public event interface centers on `IEventListener`, `TaskStatus`, `ProgressInfo`, and `FileInfo`.

## IEventListener

```cpp
class IEventListener {
public:
    virtual ~IEventListener() = default;

    virtual void on_status_changed(
        TaskId task_id,
        TaskStatus old_status,
        TaskStatus new_status) {}

    virtual void on_progress(const ProgressInfo& info) {}

    virtual void on_error(
        TaskId task_id,
        const std::string& error_message) {}

    virtual void on_completed(
        TaskId task_id,
        const std::string& output_path) {}

    virtual void on_file_info(
        TaskId task_id,
        const FileInfo& info) {}
};
```

Unlike older design drafts, the current public event interface does not define separate event structs such as `ProgressEvent` or `CompleteEvent`.

## TaskStatus

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

Use `falcon::to_string(status)` to convert a status to a string.

## ProgressInfo

```cpp
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

## FileInfo

```cpp
struct FileInfo {
    std::string url;
    std::string filename;
    std::string content_type;
    Bytes total_size = 0;
    bool supports_resume = false;
    TimePoint last_modified{};
};
```

## Listener example

```cpp
#include <falcon/falcon.hpp>
#include <iomanip>
#include <iostream>
#include <sstream>

class ConsoleListener : public falcon::IEventListener {
public:
    void on_status_changed(
        falcon::TaskId task_id,
        falcon::TaskStatus old_status,
        falcon::TaskStatus new_status) override {
        std::cout << "\n[task " << task_id << "] "
                  << falcon::to_string(old_status)
                  << " -> "
                  << falcon::to_string(new_status)
                  << '\n';
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        std::cout << "\r"
                  << std::fixed << std::setprecision(1)
                  << info.progress * 100.0f << "%";
        std::cout.flush();
    }

    void on_error(
        falcon::TaskId task_id,
        const std::string& error_message) override {
        std::cerr << "\n[task " << task_id << "] " << error_message << '\n';
    }

    void on_completed(
        falcon::TaskId task_id,
        const std::string& output_path) override {
        std::cout << "\n[task " << task_id << "] completed: "
                  << output_path << '\n';
    }
};
```

## Integrating with the engine

```cpp
falcon::DownloadEngine engine;
ConsoleListener listener;

engine.add_listener(&listener);

auto task = engine.add_task("https://example.com/file.zip");
if (task) {
    engine.start_task(task->id());
    task->wait();
}

engine.remove_listener(&listener);
```
