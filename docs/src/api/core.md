# 核心 API

本文档介绍 Falcon 的核心 API。

## DownloadEngine

下载引擎是 Falcon 的核心类，负责管理所有下载任务。

### 类定义

```cpp
namespace falcon {

class DownloadEngine {
public:
    DownloadEngine();
    ~DownloadEngine();

    // 插件管理
    void loadPlugin(const std::string& name);
    void loadAllPlugins();
    void unloadPlugin(const std::string& name);
    bool isPluginLoaded(const std::string& name) const;

    // 下载管理
    std::shared_ptr<DownloadTask> startDownload(
        const std::string& url,
        const DownloadOptions& options = {},
        IEventListener* listener = nullptr
    );

    std::shared_ptr<DownloadTask> getTask(TaskId id) const;
    std::vector<std::shared_ptr<DownloadTask>> getAllTasks() const;

    // 任务控制
    void pauseTask(TaskId id);
    void resumeTask(TaskId id);
    void cancelTask(TaskId id);

    void pauseAll();
    void resumeAll();
    void cancelAll();

    // 查询
    bool supportsUrl(const std::string& url) const;
    size_t getTaskCount() const;
    size_t getActiveTaskCount() const;

    // 引擎控制
    void run();
    void stop();
    bool isRunning() const;
};

}
```

### 使用示例

```cpp
#include <falcon/falcon.hpp>

int main() {
    // 创建引擎
    falcon::DownloadEngine engine;

    // 加载插件
    engine.loadPlugin("http");
    engine.loadPlugin("bittorrent");

    // 下载文件
    auto task = engine.startDownload("https://example.com/file.zip");
    task->wait();

    return 0;
}
```

## DownloadOptions

下载选项类，用于配置下载行为。

### 类定义

```cpp
namespace falcon {

struct DownloadOptions {
    // 输出选项
    std::string output_file;
    std::string output_directory = ".";

    // 连接选项
    int max_connections = 1;
    int connect_timeout = 30;
    int read_timeout = 60;

    // 传输选项
    int64_t speed_limit = 0;      // 0 = 无限制
    bool resume = true;            // 断点续传
    bool overwrite = false;        // 覆盖已存在文件

    // 重试选项
    int max_retries = 3;
    int retry_delay = 5;           // 秒

    // 代理选项
    ProxyInfo proxy;

    // 协议特定选项
    std::optional<HttpOptions> http_options;
    std::optional<FtpOptions> ftp_options;
    std::optional<BittorrentOptions> bt_options;
    std::optional<HlsOptions> hls_options;
};

}
```

### 使用示例

```cpp
falcon::DownloadOptions options;

// 基本选项
options.output_file = "my-file.zip";
options.output_directory = "./downloads";
options.max_connections = 5;

// 速度限制
options.speed_limit = 1024 * 1024;  // 1MB/s

// 代理设置
options.proxy = falcon::ProxyInfo{
    .type = falcon::ProxyType::HTTP,
    .host = "proxy.example.com",
    .port = 8080
};

// 开始下载
auto task = engine.startDownload(url, options);
```

## DownloadTask

下载任务类，表示一个正在进行的下载。

### 类定义

```cpp
namespace falcon {

class DownloadTask {
public:
    // 状态查询
    TaskId getId() const;
    TaskStatus getStatus() const;
    std::string getUrl() const;
    std::string getOutputPath() const;

    // 进度信息
    int64_t getDownloadedBytes() const;
    int64_t getTotalBytes() const;
    double getProgress() const;        // 0.0 - 1.0
    double getDownloadSpeed() const;
    double getUploadSpeed() const;

    // 错误信息
    std::string getErrorMessage() const;

    // 等待
    void wait();
    bool waitFor(std::chrono::milliseconds timeout);

    // 控制
    void pause();
    void resume();
    void cancel();
};

}
```

### 使用示例

```cpp
auto task = engine.startDownload(url);

// 等待完成
task->wait();

// 检查状态
if (task->getStatus() == falcon::TaskStatus::Completed) {
    std::cout << "下载完成!" << std::endl;
    std::cout << "文件大小: " << task->getTotalBytes() << " bytes" << std::endl;
}

// 超时等待
if (task->waitFor(std::chrono::seconds(60))) {
    std::cout << "在 60 秒内完成" << std::endl;
} else {
    std::cout << "超时，取消下载" << std::endl;
    task->cancel();
}
```

## TaskStatus

任务状态枚举。

### 定义

```cpp
namespace falcon {

enum class TaskStatus {
    Pending,    // 等待开始
    Running,    // 正在下载
    Paused,     // 已暂停
    Completed,  // 已完成
    Failed,     // 失败
    Cancelled   // 已取消
};

}
```

### 使用示例

```cpp
switch (task->getStatus()) {
    case falcon::TaskStatus::Running:
        std::cout << "下载中: " << task->getProgress() * 100 << "%" << std::endl;
        break;
    case falcon::TaskStatus::Completed:
        std::cout << "下载完成!" << std::endl;
        break;
    case falcon::TaskStatus::Failed:
        std::cout << "下载失败: " << task->getErrorMessage() << std::endl;
        break;
    default:
        break;
}
```

## ProxyInfo

代理信息类。

### 定义

```cpp
namespace falcon {

enum class ProxyType {
    HTTP,
    HTTPS,
    SOCKS4,
    SOCKS5
};

struct ProxyInfo {
    ProxyType type = ProxyType::HTTP;
    std::string host;
    int port = 8080;
    std::string username;
    std::string password;
};

}
```

### 使用示例

```cpp
falcon::ProxyInfo proxy;
proxy.type = falcon::ProxyType::SOCKS5;
proxy.host = "127.0.0.1";
proxy.port = 1080;
proxy.username = "user";
proxy.password = "pass";

falcon::DownloadOptions options;
options.proxy = proxy;
```

::: tip 更多 API
- [插件 API](./plugins.md)
- [事件 API](./events.md)
:::
