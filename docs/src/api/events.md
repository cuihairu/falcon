# 事件 API

本文档介绍 Falcon 的事件系统 API。

## IEventListener

事件监听器接口，用于接收下载事件通知。

### 接口定义

```cpp
namespace falcon {

class IEventListener {
public:
    virtual ~IEventListener() = default;

    /// 进度事件
    virtual void onProgress(const ProgressEvent& event) {}

    /// 完成事件
    virtual void onComplete(const CompleteEvent& event) {}

    /// 错误事件
    virtual void onError(const ErrorEvent& event) {}

    /// 状态变化事件
    virtual void onStatusChanged(const StatusChangedEvent& event) {}

    /// 暂停事件
    virtual void onPaused(const PausedEvent& event) {}

    /// 恢复事件
    virtual void onResumed(const ResumedEvent& event) {}
};

}
```

## ProgressEvent

进度事件，包含下载进度信息。

### 结构定义

```cpp
namespace falcon {

struct ProgressEvent {
    std::string url;              // 下载 URL
    int64_t downloaded_bytes;     // 已下载字节数
    int64_t total_bytes;          // 总字节数（-1 表示未知）
    double download_speed;        // 下载速度（字节/秒）
    double upload_speed;          // 上传速度（字节/秒）
    int64_t elapsed_seconds;      // 已用时间（秒）
    int64_t remaining_seconds;    // 剩余时间（秒，-1 表示未知）
};

}
```

### 使用示例

```cpp
class MyListener : public falcon::IEventListener {
public:
    void onProgress(const falcon::ProgressEvent& event) override {
        double percent = 0;
        if (event.total_bytes > 0) {
            percent = static_cast<double>(event.downloaded_bytes) /
                     event.total_bytes * 100.0;
        }

        std::cout << "\r["
                  << std::string(static_cast<int>(percent / 2), '=')
                  << "] "
                  << std::fixed << std::setprecision(1)
                  << percent << "%"
                  << " (" << formatSize(event.downloaded_bytes)
                  << " / " << formatSize(event.total_bytes) << ")"
                  << " @ " << formatSpeed(event.download_speed)
                  << std::flush;
    }

private:
    std::string formatSize(int64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024 && unit < 4) {
            size /= 1024;
            unit++;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    std::string formatSpeed(double bytes_per_second) {
        return formatSize(static_cast<int64_t>(bytes_per_second)) + "/s";
    }
};
```

## CompleteEvent

完成事件，下载成功时触发。

### 结构定义

```cpp
namespace falcon {

struct CompleteEvent {
    std::string url;              // 下载 URL
    std::string output_path;      // 输出文件路径
    int64_t total_bytes;          // 文件总字节数
    double avg_speed;             // 平均速度（字节/秒）
    int64_t elapsed_seconds;      // 总用时（秒）
};

}
```

### 使用示例

```cpp
void MyListener::onComplete(const falcon::CompleteEvent& event) override {
    std::cout << "\n下载完成!" << std::endl;
    std::cout << "文件: " << event.output_path << std::endl;
    std::cout << "大小: " << formatSize(event.total_bytes) << std::endl;
    std::cout << "用时: " << event.elapsed_seconds << " 秒" << std::endl;
    std::cout << "平均速度: " << formatSpeed(event.avg_speed) << std::endl;
}
```

## ErrorEvent

错误事件，下载失败时触发。

### 结构定义

```cpp
namespace falcon {

struct ErrorEvent {
    std::string url;              // 下载 URL
    std::string error_message;    // 错误消息
    int error_code;               // 错误代码
    bool is_retryable;            // 是否可重试
};

}
```

### 使用示例

```cpp
void MyListener::onError(const falcon::ErrorEvent& event) override {
    std::cerr << "\n下载失败!" << std::endl;
    std::cerr << "URL: " << event.url << std::endl;
    std::cerr << "错误: " << event.error_message << std::endl;
    std::cerr << "代码: " << event.error_code << std::endl;

    if (event.is_retryable) {
        std::cerr << "提示: 可以重试" << std::endl;
    }
}
```

## StatusChangedEvent

状态变化事件，任务状态改变时触发。

### 结构定义

```cpp
namespace falcon {

struct StatusChangedEvent {
    std::string url;              // 下载 URL
    TaskStatus old_status;        // 旧状态
    TaskStatus new_status;        // 新状态
};

}
```

### 使用示例

```cpp
void MyListener::onStatusChanged(const falcon::StatusChangedEvent& event) override {
    std::cout << "\n状态变化: "
              << statusToString(event.old_status)
              << " -> "
              << statusToString(event.new_status)
              << std::endl;
}

std::string statusToString(falcon::TaskStatus status) {
    switch (status) {
        case falcon::TaskStatus::Pending:   return "等待中";
        case falcon::TaskStatus::Running:   return "下载中";
        case falcon::TaskStatus::Paused:    return "已暂停";
        case falcon::TaskStatus::Completed: return "已完成";
        case falcon::TaskStatus::Failed:    return "失败";
        case falcon::TaskStatus::Cancelled: return "已取消";
        default: return "未知";
    }
}
```

## 完整示例

### 控制台输出监听器

```cpp
#include <falcon/falcon.hpp>
#include <iostream>
#include <iomanip>

class ConsoleListener : public falcon::IEventListener {
public:
    void onProgress(const falcon::ProgressEvent& event) override {
        double percent = 0;
        if (event.total_bytes > 0) {
            percent = static_cast<double>(event.downloaded_bytes) /
                     event.total_bytes * 100.0;
        }

        // 进度条
        int bar_width = 40;
        int filled = static_cast<int>(percent / 100 * bar_width);

        std::cout << "\r["
                  << std::string(filled, '=')
                  << std::string(bar_width - filled, ' ')
                  << "] "
                  << std::fixed << std::setprecision(1) << percent << "%";

        // 文件大小
        std::cout << " ("
                  << formatSize(event.downloaded_bytes)
                  << " / "
                  << (event.total_bytes > 0 ? formatSize(event.total_bytes) : std::string("unknown"))
                  << ")";

        // 速度
        if (event.download_speed > 0) {
            std::cout << " @ " << formatSize(event.download_speed) << "/s";

            // 剩余时间
            if (event.remaining_seconds > 0) {
                int minutes = event.remaining_seconds / 60;
                int seconds = event.remaining_seconds % 60;
                std::cout << " ETA: "
                          << std::setw(2) << std::setfill('0') << minutes
                          << ":"
                          << std::setw(2) << std::setfill('0') << seconds;
            }
        }

        std::cout << std::flush;
    }

    void onComplete(const falcon::CompleteEvent& event) override {
        std::cout << "\n✓ 下载完成!" << std::endl;
        std::cout << "  文件: " << event.output_path << std::endl;
        std::cout << "  大小: " << formatSize(event.total_bytes) << std::endl;
        std::cout << "  用时: " << formatTime(event.elapsed_seconds) << std::endl;
    }

    void onError(const falcon::ErrorEvent& event) override {
        std::cout << "\n✗ 下载失败: " << event.error_message << std::endl;
    }

    void onStatusChanged(const falcon::StatusChangedEvent& event) override {
        if (event.new_status == falcon::TaskStatus::Running) {
            std::cout << "开始下载..." << std::endl;
        } else if (event.new_status == falcon::TaskStatus::Paused) {
            std::cout << "\n下载已暂停" << std::endl;
        } else if (event.new_status == falcon::TaskStatus::Cancelled) {
            std::cout << "\n下载已取消" << std::endl;
        }
    }

private:
    std::string formatSize(int64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024 && unit < 5) {
            size /= 1024;
            unit++;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
        return oss.str();
    }

    std::string formatTime(int64_t seconds) {
        int hours = seconds / 3600;
        int minutes = (seconds % 3600) / 60;
        int secs = seconds % 60;

        std::ostringstream oss;
        if (hours > 0) {
            oss << hours << ":"
               << std::setw(2) << std::setfill('0') << minutes << ":"
               << std::setw(2) << std::setfill('0') << secs;
        } else {
            oss << minutes << ":"
               << std::setw(2) << std::setfill('0') << secs;
        }
        return oss.str();
    }
};

// 使用示例
int main() {
    falcon::DownloadEngine engine;
    engine.loadAllPlugins();

    ConsoleListener listener;
    auto task = engine.startDownload(
        "https://example.com/file.zip",
        {},
        &listener
    );

    task->wait();
    return 0;
}
```

::: tip 更多 API
- [核心 API](./core.md)
- [插件 API](./plugins.md)
:::
