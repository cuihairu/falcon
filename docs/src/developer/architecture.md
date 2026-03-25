# 架构设计

本文档基于当前仓库代码说明 Falcon 的核心结构，而不是早期设计稿中的理想化接口。

## 总体结构

```text
falcon/
├── packages/
│   ├── libfalcon/          # 核心下载引擎与协议插件
│   ├── falcon-cli/         # 命令行工具
│   └── falcon-daemon/      # 后台服务
├── apps/
│   ├── desktop/            # 桌面端
│   └── browser_extension/  # 浏览器扩展
└── docs/                   # 文档站
```

## libfalcon 核心组件

### DownloadEngine

`DownloadEngine` 是面向调用方的主入口，负责：

- 接收下载请求
- 管理任务生命周期
- 管理事件监听器
- 与 `PluginManager` 和 `TaskManager` 协作

当前公开接口以这些方法为主：

```cpp
DownloadTask::Ptr add_task(const std::string& url,
                           const DownloadOptions& options = {});
std::vector<DownloadTask::Ptr> add_tasks(const std::vector<std::string>& urls,
                                         const DownloadOptions& options = {});

bool start_task(TaskId id);
bool pause_task(TaskId id);
bool resume_task(TaskId id);
bool cancel_task(TaskId id);

void add_listener(IEventListener* listener);
void remove_listener(IEventListener* listener);

std::vector<std::string> get_supported_protocols() const;
bool is_url_supported(const std::string& url) const;
```

说明：

- `DownloadEngine` 构造时会加载当前构建里可用的内置处理器。
- 文档中不再使用旧的 `startDownload()` / `loadPlugin()` 作为对外接口。

### TaskManager

`TaskManager` 负责：

- 任务队列
- 最大并发控制
- 任务状态迁移
- 启动、暂停、恢复、取消具体任务

从 `DownloadEngine` 实现来看，任务通常先通过 `add_task()` 创建，再由 `start_task()` 推进执行。

### PluginManager

`PluginManager` 负责：

- 注册协议处理器
- 根据 URL 路由到合适处理器
- 按构建开关加载内置插件
- 卸载插件

公共头文件中的核心方法包括：

```cpp
void registerPlugin(std::unique_ptr<IProtocolHandler> plugin);
IProtocolHandler* getPlugin(const std::string& protocol) const;
IProtocolHandler* getPluginByUrl(const std::string& url) const;
void loadAllPlugins();
bool supportsUrl(const std::string& url) const;
```

### IProtocolHandler

协议插件通过 `IProtocolHandler` 接入：

```cpp
std::string protocol_name() const;
std::vector<std::string> supported_schemes() const;
bool can_handle(const std::string& url) const;
FileInfo get_file_info(const std::string& url,
                       const DownloadOptions& options);
void download(DownloadTask::Ptr task, IEventListener* listener);
void pause(DownloadTask::Ptr task);
void resume(DownloadTask::Ptr task, IEventListener* listener);
void cancel(DownloadTask::Ptr task);
```

与旧设计相比，下载接口不是“传 URL 返回新 task”，而是“接收已有 `DownloadTask::Ptr`”。

## 下载流程

```text
用户调用 add_task(url, options)
        ↓
DownloadEngine 校验 URL 并创建 DownloadTask
        ↓
PluginManager 根据 URL 找到协议处理器
        ↓
TaskManager 接管任务并排队
        ↓
start_task(id) 触发执行
        ↓
协议处理器执行 download(task, listener)
        ↓
任务状态和进度通过事件系统回传
```

## 事件系统

当前公共事件接口不是 `ProgressEvent` / `CompleteEvent` 这套结构体，而是：

- `TaskStatus`
- `ProgressInfo`
- `FileInfo`
- `IEventListener`

`IEventListener` 主要回调：

```cpp
void on_status_changed(TaskId task_id,
                       TaskStatus old_status,
                       TaskStatus new_status);
void on_progress(const ProgressInfo& info);
void on_error(TaskId task_id, const std::string& error_message);
void on_completed(TaskId task_id, const std::string& output_path);
void on_file_info(TaskId task_id, const FileInfo& info);
```

## 并发与线程

从当前实现看：

- `EventDispatcher` 负责事件分发
- `TaskManager` 负责任务调度
- 协议处理器负责具体下载逻辑
- `DownloadEngine::Impl` 在构造时启动事件分发器和任务管理器

这意味着调用方通常不需要自己管理“主事件循环”。

## 设计边界

当前文档刻意不再展开这些尚未形成稳定公共 API 的设计：

- `startDownloadAsync()` 之类异步包装接口
- 协程版下载 API
- 协议专属公开选项结构体
- Web 管理界面作为既成事实架构层

这些内容如果未来进入实际头文件，再适合写进架构文档。
