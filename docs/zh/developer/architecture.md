# 架构设计

本文档基于当前仓库代码说明 Falcon 的核心结构，而不是早期设计稿中的理想化接口。

## 总体结构

```text
falcon/
├── packages/
│   ├── libfalcon-core/     # 核心下载引擎、任务模型、事件系统
│   ├── libfalcon-protocols/# 标准下载协议与协议相关实现
│   ├── libfalcon-storage/  # 对象存储与远程资源浏览
│   ├── libfalcon-drives/   # 网盘、分享链、账号态能力
│   ├── falcon-cli/         # 命令行工具
│   └── falcon-daemon/      # 后台服务
├── apps/
│   └── desktop/            # Qt6 桌面应用
└── docs/                   # 文档站
```

## 库依赖关系

```
falcon_protocols  →  falcon_core
falcon_storage    →  falcon_core
falcon_drives     →  falcon_core
```

禁止反向依赖：`core` 不依赖 `protocols/storage/drives`。

顶层还提供兼容聚合 target：

```text
Falcon::falcon = falcon_core + falcon_protocols + falcon_storage + falcon_drives
```

`Falcon::falcon` 主要用于历史兼容和“一次性链接全部能力”的场景。内部应用、测试和新增模块应优先按需链接具体 target，例如 `Falcon::core`、`Falcon::protocols`、`Falcon::storage` 或 `Falcon::drives`，避免把无关模块隐式带入最终链接图。

## 当前分层状态

### libfalcon-core

核心层包含稳定领域模型和调度抽象：

- `DownloadTask`、`TaskManager`、`DownloadEngine`
- `ProtocolRegistry`、`IProtocolHandler`
- `EventDispatcher`、`IEventListener`
- 基础类型、异常、日志、版本、密码管理

`falcon_core` 可以独立构建和测试，不依赖具体协议实现。内置协议注册通过 `builtin_protocol_handlers_stub.cpp` 提供弱符号桩函数；当最终程序同时链接 `falcon_protocols` 时，`libfalcon-protocols/src/builtin_protocol_handlers.cpp` 中的真实实现会覆盖桩函数并注册 HTTP/FTP 等具体处理器。

### libfalcon-protocols

协议层依赖 `falcon_core`，负责具体下载协议和传输能力：

- HTTP/FTP 等协议处理器
- 分段下载、增量下载、请求组、文件 hash
- socket/event poll 等协议侧网络基础设施
- 可选私有协议插件

协议层可以扩展具体协议，但不应把协议实现细节反向放入 core。

### libfalcon-storage

存储层依赖 `falcon_core`，负责远程资源浏览和对象存储浏览能力，例如 FTP/S3/OSS/COS/Kodo/Upyun browser。它不应依赖 `falcon_protocols`。

### libfalcon-drives

网盘层依赖 `falcon_core`，负责网盘、分享链、资源搜索和配置管理类能力。当前这层仍包含占位源文件和多个可选功能开关，成熟度低于 `core` 和基础协议层。

## 运行时分层

上述构建包是组合单元。下面七个运行时层按实际的状态所有权、通道、配置和生命周期归纳。逐条术语证据见[源码分层术语表](./glossary.md)。

### L1：引擎领域与调度

`DownloadEngine` 是新增、启动、暂停、恢复、取消和观察任务的稳定门面；实现负责装配 `TaskManager`、`ProtocolRegistry` 和 `EventDispatcher`（`packages/libfalcon-core/src/download_engine.cpp:372`、`packages/libfalcon-core/include/falcon/download_engine.hpp:16`）。`TaskManager` 拥有队列、优先级、worker 执行和终态清理（`packages/libfalcon-core/include/falcon/task_manager.hpp:22`）。事件经有界 worker 池离开本层（`packages/libfalcon-core/src/event_dispatcher.cpp:17`）。引擎状态是进程内存；持久化属于 daemon 装配层。

### L2：协议与传输数据面

核心注册表在启动时接收按编译条件选择的具体处理器（`packages/libfalcon-protocols/src/builtin_protocol_handlers.cpp:39`）。协议包实现 V1 处理器契约和传输基础能力。HTTP 还可分流到实验性 V2 命令引擎；其宿主保留进程级唯一实例，并定义严格的“先排水、后停机”契约（`packages/libfalcon-protocols/include/falcon/protocols/v2_engine_host.hpp:1`）。特性选择是 CMake schema 输入，不是运行时配置文件（`CMakeLists.txt:48`、`packages/libfalcon-protocols/CMakeLists.txt:194`）。

### L3：远端资源与云访问

`falcon_storage` 通过 `IResourceBrowser` 和 `ResourceBrowserManager` 暴露可浏览的远端文件系统/对象存储（`packages/libfalcon-storage/include/falcon/storage/resource_browser.hpp:81`、`packages/libfalcon-storage/src/resource_browser.cpp:90`）。`falcon_drives` 是另一个扩展点：分享链识别和直链提取（`packages/libfalcon-drives/include/falcon/drives/cloud_storage_plugin.hpp:89`），以及基于选择器的搜索（`packages/libfalcon-drives/src/resource_search.cpp:661`）。可选的加密配置保管层使用 SQLite 和 AES-256-GCM（`packages/libfalcon-drives/src/config_manager.cpp:210`）。远端浏览与云盘解析必须保持独立接口：前者对文件系统端点认证并执行路径操作，后者先从厂商页面态推导下载 URL。

### L4：呈现层与本地应用状态

Qt 进程拥有控件、页面、翻译、系统集成和后端选择。命令边界是 `IDownloadBackend`；两个实现要么直接调用 `DownloadEngine`，要么向 daemon 发送 JSON-RPC（`apps/desktop/src/services/download_backend.hpp:33`、`apps/desktop/src/services/download_backend.cpp:79`、`apps/desktop/src/services/download_backend.cpp:191`）。MainWindow 按 QSettings 选择后端（`apps/desktop/src/main_window.cpp:281`）。QSettings 还保存 daemon 连接、云配置、视图偏好和更新状态（`apps/desktop/src/main_window.cpp:743`、`apps/desktop/src/services/storage_service.cpp:76`、`apps/desktop/src/pages/download_page.cpp:847`）。内嵌模式额外拥有 `.falcon-trash/trash.json`（`apps/desktop/src/services/trash_store.cpp:117`），搜索引擎规则位于 `~/.config/falcon/engines.json`（`apps/desktop/src/services/search_engine_catalog.cpp:192`、`apps/desktop/src/services/search_engine_catalog.cpp:210`）。

### L5：服务装配与进程生命周期

CLI 装配是一次前台运行：解析/合并选项，构造引擎，挂接终端监听器和可选 P2SP 公告器，等待后排水 V2（`packages/falcon-cli/src/main.cpp:864`、`packages/falcon-cli/src/main.cpp:997`、`packages/falcon-cli/src/main.cpp:1055`）。Daemon 装配是长生命周期：先选择 V1/V2，再恢复任务，初始化 WAL SQLite，按原 ID 恢复非终态任务，装配持久化和 P2SP，启动 RPC/MCP，并处理信号与配置热载（`packages/falcon-daemon/src/main.cpp:300`、`packages/falcon-daemon/src/main.cpp:312`、`packages/falcon-daemon/src/main.cpp:340`、`packages/falcon-daemon/src/main.cpp:517`、`packages/falcon-daemon/src/main.cpp:543`）。每个进程拥有自己的内存引擎；只有 daemon 拥有持久任务会话。

### L6：控制面协议与持久化

Daemon 监听器接受 TCP，解析 HTTP，并服务 `/jsonrpc` 和 WebSocket 通知（`packages/falcon-daemon/src/rpc/json_rpc_server.cpp:575`、`packages/falcon-daemon/src/rpc/json_rpc_server.cpp:1086`）。MCP `tools/list` 和 `tools/call` 复用 `JsonRpcServer::dispatch_rpc`（`packages/falcon-daemon/src/rpc/mcp_server.cpp:473`、`packages/falcon-daemon/src/rpc/json_rpc_server.cpp:1176`）。SQLite 是 daemon 的任务状态权威；监听器把引擎事件转换为节流写入和最终冲刷（`packages/falcon-daemon/src/storage/task_storage.cpp:25`、`packages/falcon-daemon/src/storage/task_storage_listener.cpp:18`）。仓库未使用 Redis 或 MySQL。该控制面与访问第三方资源/云 API 的 L3 有意独立。

### L7：P2SP 集会与数据面

`falcon-swarmd` 是独立权威，不是 daemon 插件。其集会状态是内存中的节点/会话/挑战/资源映射（`packages/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:3`），由自包含 HTTP/WebSocket 服务承载（`packages/falcon-swarmd/src/rdv/swarm_rpc_server.cpp:295`）。协议文件固定签名方法、通知名和错误码（`packages/falcon-swarmd/src/common/swarm_protocol.hpp:30`）。客户端使用持久 PEM 身份（`packages/falcon-swarmd/src/client/swarm_key_store.cpp:1`）。daemon 公告器发布完成资源，本地 HTTP 服务对已公告字节响应 Range 请求（`packages/falcon-daemon/src/daemon/swarm_announcer.cpp:1`、`packages/falcon-daemon/src/daemon/swarm_data_service.cpp:193`）。因此目录权威和文件可用性拥有独立生命周期。

### 配置与状态矩阵

| 层 | 配置输入 | 状态存储 |
|---|---|---|
| L1–L2 | CMake 选项和调用方提供的 `DownloadOptions` | 进程内存；断点工件在目标文件内 |
| L3 | 调用方或桌面 profile 提供的 provider 选项 | 云盘 profile 可选加密 SQLite；远端端点本身是无状态服务 |
| L4 | QSettings、`engines.json`、CLI deep-link | QSettings、`trash.json`、下载文件 |
| L5–L6 | CLI 参数与 `daemon.json` 合并；SIGHUP 热载文件 | WAL SQLite `tasks.db`、PID 文件、日志、daemon 内存引擎 |
| L7 | `swarm.json`、CLI P2SP 参数、PEM key | 集会映射仅内存；身份是 PEM 文件；daemon 服务已公告文件 |

具体 schema 读取点在 `packages/falcon-daemon/src/daemon/config.cpp:96`、`packages/falcon-swarmd/src/swarmd_config.cpp:94` 和 `packages/falcon-cli/src/config_loader.cpp:176`。构建期 schema 与 CMake option 同处（`CMakeLists.txt:37`、`packages/libfalcon-drives/CMakeLists.txt:121`）。

## 拆分理由

1. **把可复用领域抽象与线上实现分离。** Core 只暴露处理器路由，不含具体协议（`packages/libfalcon-core/src/protocol_registry.cpp:97`）；链接具体协议包后才注册处理器（`packages/libfalcon-protocols/src/builtin_protocol_handlers.cpp:39`）。这样后续协议不会反过来改变任务调度 API。
2. **不把分享解析和文件系统浏览合并。** Storage 浏览需要连接端点并执行路径操作（`packages/libfalcon-storage/include/falcon/storage/resource_browser.hpp:117`），而 Drive 解析先把分享 URL 变成直接资源（`packages/libfalcon-drives/src/cloud_storage_plugin.cpp:320`、`packages/libfalcon-drives/src/cloud_storage_plugin.cpp:466`）。两者的失败、认证和状态模型不同。
3. **把 V1/V2 HTTP 面隔离在单一宿主后。** V2 引擎有自己的任务 ID 和命令/事件循环（`packages/libfalcon-protocols/include/falcon/protocols/download_engine_v2.hpp:104`），宿主则保护 V1 持久化，并要求全部 V1 worker 返回后再排水（`packages/libfalcon-protocols/include/falcon/protocols/v2_engine_host.hpp:1`）。这允许回退而不引入第二套持久化模型。
4. **让 daemon 持久化由事件驱动，而不是由 RPC 序列化。** SQLite 写入发生在引擎监听器中，带进度节流和终态冲刷（`packages/falcon-daemon/src/storage/task_storage_listener.cpp:18`、`packages/falcon-daemon/src/storage/task_storage_listener.cpp:74`）。RPC 读取持久真相，而不是成为唯一写入者。
5. **把长生命周期控制面与一次性客户端分离。** Daemon 启动时恢复任务 ID 和活跃任务（`packages/falcon-daemon/src/main.cpp:340`）；CLI 为单次运行构造任务并等待（`packages/falcon-cli/src/main.cpp:997`、`packages/falcon-cli/src/main.cpp:1050`）。同一门面可服务两者，但不强迫 CLI 拥有数据库。
6. **让 UI 命令语义在后端切换后保持不变。** `IDownloadBackend` 固定同一新增/控制/查询契约（`apps/desktop/src/services/download_backend.hpp:33`）；MainWindow 只在构造时选择内嵌或 RPC（`apps/desktop/src/main_window.cpp:281`）。因此可以加入 RPC，而不重写每个 Qt 页面。
7. **让 P2SP 目录权威与下载进程故障隔离。** 集会服务拥有限频和 TTL 清扫（`packages/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:3`），每个节点拥有密钥和本地数据服务（`packages/falcon-swarmd/src/client/swarm_key_store.cpp:1`、`packages/falcon-daemon/src/daemon/swarm_data_service.cpp:193`）。目录故障不会销毁本地下载；节点退出会移除可用性，也不会损坏目录数据库。
8. **把控制面协议收敛到同一方法分发器。** MCP 将 `tools/call` 委托给 `dispatch_rpc`（`packages/falcon-daemon/src/rpc/mcp_server.cpp:476`），aria2、Falcon 扩展和 MCP 不会漂移成三套业务逻辑。

## 当前稳定性基线

当前已用 vcpkg manifest 构建验证：

- `falcon_core_tests`: 384 个测试通过
- `falcon_split_tests`: 10 个测试通过

这说明 `libfalcon-core` 的核心行为和四库链接分层在当前测试覆盖范围内是稳定的。它不等价于整个项目功能齐全；协议插件、桌面端、daemon、网盘和资源浏览仍需要各自的集成测试与真实场景验证。

## libfalcon-core 核心组件

### DownloadEngine

`DownloadEngine` 是面向调用方的主入口，负责：

- 接收下载请求
- 管理任务生命周期
- 管理事件监听器
- 与 `ProtocolRegistry` 和 `TaskManager` 协作

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

### ProtocolRegistry

`ProtocolRegistry` 负责：

- 注册协议处理器
- 根据 URL 路由到合适处理器
- 加载当前构建里可用的内置 handler
- 提供协议与 scheme 查询

公共头文件中的核心方法包括：

```cpp
void register_handler(std::unique_ptr<IProtocolHandler> handler);
IProtocolHandler* get_handler(const std::string& protocol) const;
IProtocolHandler* get_handler_for_url(const std::string& url) const;
void load_builtin_handlers();
bool supports_url(const std::string& url) const;
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
ProtocolRegistry 根据 URL 找到协议处理器
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
