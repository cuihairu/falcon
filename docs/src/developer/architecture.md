# 架构设计

本文档详细介绍 Falcon 的架构设计。

## 总体架构

```
┌─────────────────────────────────────────────────────────┐
│           应用层 (apps/)                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   desktop    │  │     web      │  │   (future)   │  │
│  │  GUI 桌面应用 │  │  Web 管理界面 │  │   移动端等    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│          工具层 (packages/)                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ falcon-cli   │  │falcon-daemon │  │ (future)     │  │
│  │  命令行工具   │  │  后台守护进程 │  │  Python/JS   │  │
│  │              │  │  RPC 接口服务 │  │   绑定库     │  │
│  └──────┬───────┘  └──────┬───────┘  └──────────────┘  │
│         │                 │                             │
│         └─────────┬───────┘                             │
│                   │                                     │
│  ┌────────────────▼────────────────┐                   │
│  │       libfalcon                 │                   │
│  │   核心下载引擎库                 │                   │
│  │  ┌──────────────────────────┐   │                   │
│  │  │  任务管理器              │   │                   │
│  │  │  (TaskManager)           │   │                   │
│  │  └──────────────────────────┘   │                   │
│  │  ┌──────────────────────────┐   │                   │
│  │  │  下载引擎                │   │                   │
│  │  │  (DownloadEngine)        │   │                   │
│  │  └──────────────────────────┘   │                   │
│  │  ┌──────────────────────────┐   │                   │
│  │  │  协议插件管理器          │   │                   │
│  │  │  (PluginManager)         │   │                   │
│  │  └──────────────────────────┘   │                   │
│  └─────────────────┬────────────────┘                   │
└────────────────────┼────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│        协议插件层 (plugins/)                            │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐         │
│  │ HTTP │ │ FTP  │ │  BT  │ │ ED2K │ │ HLS  │   ...   │
│  └──────┘ └──────┘ └──────┘ └──────┘ └──────┘         │
└─────────────────────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│       基础设施层 (third_party/ & 系统库)                 │
│  libcurl, libtorrent, spdlog, CLI11, nlohmann/json...   │
└─────────────────────────────────────────────────────────┘
```

## 核心组件

### DownloadEngine (下载引擎)

下载引擎是 Falcon 的核心，负责协调整个下载过程。

```cpp
namespace falcon {

class DownloadEngine {
public:
    // 加载插件
    void loadPlugin(const std::string& name);
    void loadAllPlugins();

    // 开始下载
    std::shared_ptr<DownloadTask> startDownload(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener = nullptr
    );

    // 暂停/恢复/取消
    void pauseTask(TaskId id);
    void resumeTask(TaskId id);
    void cancelTask(TaskId id);

    // 查询状态
    TaskStatus getTaskStatus(TaskId id) const;

private:
    PluginManager plugin_manager_;
    TaskManager task_manager_;
    EventLoop event_loop_;
};

}
```

**职责**：
- 管理插件生命周期
- 创建和管理下载任务
- 协调事件循环
- 提供统一的 API 接口

### TaskManager (任务管理器)

任务管理器负责所有下载任务的生命周期管理。

```cpp
namespace falcon {

class TaskManager {
public:
    // 创建任务
    TaskId createTask(const std::string& url,
                      const DownloadOptions& options);

    // 获取任务
    std::shared_ptr<DownloadTask> getTask(TaskId id);

    // 暂停/恢复/取消
    void pauseTask(TaskId id);
    void resumeTask(TaskId id);
    void cancelTask(TaskId id);

    // 批量操作
    void pauseAll();
    void resumeAll();
    void cancelAll();

private:
    std::unordered_map<TaskId, std::shared_ptr<DownloadTask>> tasks_;
    std::mutex tasks_mutex_;
    TaskId next_task_id_;
};

}
```

**职责**：
- 任务创建和销毁
- 任务状态管理
- 任务队列调度
- 并发控制

### PluginManager (插件管理器)

插件管理器负责协议插件的加载和管理。

```cpp
namespace falcon {

class PluginManager {
public:
    // 注册插件
    void registerPlugin(std::unique_ptr<IProtocolHandler> plugin);

    // 获取协议处理器
    IProtocolHandler* getHandler(const std::string& url);

    // 加载内置插件
    void loadBuiltinPlugins();

    // 加载外部插件
    void loadExternalPlugin(const std::string& path);

private:
    std::vector<std::unique_ptr<IProtocolHandler>> plugins_;
    std::unordered_map<std::string, IProtocolHandler*> protocol_map_;
};

}
```

**职责**：
- 插件注册和发现
- 协议路由
- 插件生命周期管理

### IProtocolHandler (协议处理器接口)

所有协议插件必须实现此接口。

```cpp
namespace falcon {

class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    // 协议名称
    virtual std::string protocol() const = 0;

    // 是否可以处理该 URL
    virtual bool canHandle(const std::string& url) const = 0;

    // 开始下载
    virtual std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener
    ) = 0;
};

}
```

## 下载流程

```
┌─────────────┐
│  用户调用   │
│ startDownload│
└──────┬──────┘
       │
       ▼
┌─────────────────────┐
│  DownloadEngine     │
│  解析 URL           │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  PluginManager      │
│  获取协议处理器     │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  IProtocolHandler   │
│  创建 DownloadTask  │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  TaskManager        │
│  注册任务           │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  下载执行           │
│  - 建立连接         │
│  - 下载数据         │
│  - 通知进度         │
└──────┬──────────────┘
       │
       ▼
┌─────────────────────┐
│  任务完成/失败      │
└─────────────────────┘
```

## 事件系统

Falcon 使用事件监听器模式来通知下载进度和状态变化。

```cpp
namespace falcon {

class IEventListener {
public:
    virtual ~IEventListener() = default;

    // 进度事件
    virtual void onProgress(const ProgressEvent& event) {}

    // 完成事件
    virtual void onComplete(const CompleteEvent& event) {}

    // 错误事件
    virtual void onError(const ErrorEvent& event) {}

    // 状态变化
    virtual void onStatusChanged(const StatusChangedEvent& event) {}
};

struct ProgressEvent {
    std::string url;
    int64_t downloaded_bytes;
    int64_t total_bytes;
    double download_speed;
    double upload_speed;
};

}
```

## 异步模型

Falcon 使用现代 C++ 异步模型：

```cpp
// 使用 std::future
std::future<void> DownloadEngine::startDownloadAsync(
    const std::string& url,
    const DownloadOptions& options
) {
    return std::async(std::launch::async, [this, url, options]() {
        this->startDownload(url, options)->wait();
    });
}

// 使用协程 (C++20)
Task<void> DownloadEngine::startDownloadCoroutine(
    const std::string& url,
    const DownloadOptions& options
) {
    auto task = co_await createTaskAsync(url, options);
    co_await task->waitForCompletion();
}
```

## 线程模型

```
┌─────────────────────────────────────────────────┐
│              主线程                             │
│  - 处理用户请求                                 │
│  - 更新任务状态                                 │
│  - 分发事件                                     │
└─────────────────────────────────────────────────┘
                    │
        ┌───────────┼───────────┐
        │           │           │
        ▼           ▼           ▼
┌───────────┐ ┌───────────┐ ┌───────────┐
│ 工作线程1  │ │ 工作线程2  │ │ 工作线程N  │
│ - HTTP下载 │ │ - FTP下载  │ │ - BT下载  │
└───────────┘ └───────────┘ └───────────┘
        │           │           │
        └───────────┼───────────┘
                    │
                    ▼
            ┌───────────────┐
            │  线程池       │
            │  (ThreadPool) │
            └───────────────┘
```

## 设计原则

### SOLID 原则

1. **单一职责 (SRP)**
   - DownloadEngine：下载协调
   - TaskManager：任务管理
   - PluginManager：插件管理

2. **开闭原则 (OCP)**
   - 对扩展开放：添加新协议只需实现 IProtocolHandler
   - 对修改关闭：核心代码无需改动

3. **里氏替换 (LSP)**
   - 所有 IProtocolHandler 实现可互换使用

4. **接口隔离 (ISP)**
   - IProtocolHandler：最小接口
   - IEventListener：可选接口

5. **依赖倒置 (DIP)**
   - 依赖抽象接口而非具体实现

### 其他原则

- **DRY**：避免代码重复
- **KISS**：保持简单
- **YAGNI**：不过度设计

::: tip 架构决策
重要的架构决策记录在项目的 ADR 文档中。
:::
