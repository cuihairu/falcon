[根目录](../../CLAUDE.md) > [packages](../) > **falcon-daemon**

---

# falcon-daemon - 后台守护进程

## 变更记录 (Changelog)

### 2026-09-11 - aria2 兼容 RPC 全面完善
- RPC 方法扩至 26 个（`aria2.*` 全套核心 + `system.*`），查询与删除同时覆盖
  引擎内存态与 TaskStorage 历史记录（引擎优先、storage 回落，`tellWaiting`
  按 engine id 去重）
- 新增方法：`getFiles`/`getUris`/`getOption`/`getGlobalOption`/
  `changeGlobalOption`/`getSessionInfo`/`saveSession`/`purgeDownloadResult`/
  `removeDownloadResult`/`pauseAll`/`unpauseAll`/`forceShutdown`/
  `system.multicall` 等
- `forceShutdown`/`shutdown` 通过 `set_shutdown_handler` 触发正常停机流程
  （排水 + 落库），main.cpp 接线到 `DaemonManager::request_stop()`
- 会话标识：`getSessionInfo` 返回启动时生成的 `sessionId`
- `pauseAll`/`unpauseAll` 显式落库受影响任务的状态（aria2 语义）
- `TaskStorage::create_task` 支持显式插入 record.id（RPC addUri 写库时
  storage id 与引擎 id 保持一致；此前自增覆盖导致删除/重启后错位）
- JSON-RPC 错误码分层：请求体非 JSON → -32700；dispatch 内部运行时失败
  → -32603；业务错误沿用 aria2 码（1/2），协议错误 -32600/-32601/-32602
- 删除死代码 `xml_rpc_server.{hpp,cpp}`（从未接入 CMakeLists）
- 新增测试目标 `falcon_daemon_rpc_storage_tests`（JSON-RPC × TaskStorage
  集成，15 个用例）与 `falcon_daemon_rpc_coverage_tests`（HTTP 层 +
  全方法覆盖）
- Core 新增只读查询：`DownloadEngine::get_global_speed_limit()` /
  `get_max_concurrent_tasks()`（供 getGlobalOption 回显）

### 2026-09-11 - 任务持久化闭环完成
- 新增 `TaskStorageListener`（`src/storage/`）：实现 `IEventListener`，注册到引擎
  - 状态变更实时落库：`Completed`/`Failed` 走 `mark_completed`/`mark_failed`（记录终态时间戳与错误消息）
  - 进度按任务 1 秒节流落库（`update_progress`）
  - `on_error` 的错误消息按任务缓存，任务进入 `Failed` 时随 `mark_failed` 一次性消费
  - `shutdown()` 保证停机时监听器先于 `TaskStorage` 停止访问数据库
- 停机语义修正：停止信号改为 `pause_all()` + 排水等待（原先 `cancel_all()` 会把未完成任务落库为 Cancelled，重启后无法恢复）
- 恢复逻辑：从 `list_tasks()` 全量恢复（跳过终态；Paused 任务恢复但不自动启动）；恢复时还原 `output_path`
- 任务 id 冲突修复：启动时用 `TaskStorage::get_max_task_id()` 推进引擎计数器（`DownloadEngine::set_next_task_id`），避免重启后新任务 id 与历史记录冲突导致写错库
- `TaskRecord` 字段补默认初始化，防止垃圾值入库
- vcpkg.json 与 CI 增加 SQLite3 依赖，`falcon_daemon_storage_tests` 纳入 CI

### 2025-12-21 - 初始化模块架构
- 创建 Daemon 项目结构
- RPC 选型定为 aria2 兼容 JSON-RPC（HTTP），gRPC/REST 蓝图废弃
- 规划任务持久化方案

---

## 模块职责

`falcon-daemon` 是 Falcon 下载器的后台守护进程，提供以下能力：

1. **常驻后台**：作为系统服务持续运行（`--daemon` 模式、PID 文件、信号处理）
2. **aria2 兼容 RPC**：HTTP + JSON-RPC 2.0，`token:<secret>` 认证，
   aria2 客户端/前端（如 AriaNg）可直接对接
3. **任务持久化**：SQLite 状态/进度实时落库，停机保存、重启恢复
4. **多客户端支持**：无状态 HTTP 请求，天然支持多客户端并发

## 源码结构

```
packages/falcon-daemon/src/
├── main.cpp                  # 入口：参数解析 → DaemonManager 组装与接线
├── daemon/
│   ├── daemon.hpp/.cpp       # DaemonManager：生命周期、信号、pid 文件、停机排水
│   └── (daemonize POSIX 细节)
├── rpc/
│   ├── json_rpc_server.hpp/.cpp  # aria2 兼容 JSON-RPC 2.0 服务器（26 个方法）
│   └── websocket_server.hpp/.cpp # 预留（未来事件流订阅），未接入构建
└── storage/
    ├── task_storage.hpp/.cpp         # SQLite 持久化（TaskRecord CRUD）
    └── task_storage_listener.hpp/.cpp # IEventListener → 实时落库
```

---

## 入口与启动

### 编译与运行

```bash
# 编译
cmake -B build -DFALCON_BUILD_DAEMON=ON
cmake --build build --target falcon-daemon

# 前台运行（调试用）
./build/bin/falcon-daemon --enable-rpc --rpc-listen-port 6800

# 后台运行（守护进程模式）
./build/bin/falcon-daemon --daemon --enable-rpc --pid-file /var/run/falcon-daemon.pid
```

### 命令行参数

```bash
falcon-daemon [OPTIONS]

RPC Options:
  --enable-rpc[=true|false]   启用 JSON-RPC 服务器（默认 false）
  --rpc-listen-port <port>    监听端口（默认 6800）
  --rpc-secret <token>        要求 params 首位为 token:<token>（缺省不鉴权）
  --rpc-allow-origin-all      附加 CORS 头（Access-Control-Allow-Origin: *）
  --rpc-listen-host <ip>      绑定地址（默认 127.0.0.1）

Daemon Options:
  -d, --daemon                后台守护进程模式（POSIX）
  --pid-file <path>           PID 文件路径
  --working-dir <dir>         工作目录
  --log-file <path>           日志文件（重定向 stdout/stderr）
  --task-db <path>            任务数据库路径（默认 ~/.config/falcon/tasks.db）

Windows Service Options（仅 Windows）:
  --install-service           安装为 Windows 服务
  --uninstall-service         卸载 Windows 服务
  --service-name <name>       服务名（默认 falcon-daemon）
```

> 配置文件（`daemon.json`）加载尚未实现，目前仅命令行参数。

---

## 对外接口

### aria2 兼容 JSON-RPC

- 端点：`http://<host>:<port>/jsonrpc`（`/` 同义），仅 POST
- 认证：params 首位 `token:<secret>`；未配置 secret 时跳过校验
- CORS：`--rpc-allow-origin-all` 时回显 `Access-Control-Allow-*`
- 批量调用：`system.multicall`（结果包装为 `[result]`）

方法清单（26 个）：

| 分组 | 方法 |
|------|------|
| 任务控制 | `addUri` `pause` `forcePause` `unpause` `unpauseAll` `pauseAll` `remove` `forceRemove` |
| 查询 | `tellStatus` `tellActive` `tellWaiting` `tellStopped` `getFiles` `getUris` `getOption` `getGlobalStat` |
| 选项 | `getGlobalOption` `changeGlobalOption`（支持 `max-overall-download-limit`、`max-concurrent-downloads`；`"none"`/`"0"` 取消限制） |
| 会话与清理 | `getSessionInfo` `saveSession` `purgeDownloadResult` `removeDownloadResult` `forceShutdown` |
| 系统 | `system.listMethods` `system.multicall` |

错误码约定（对齐 aria2）：

| 码 | 含义 |
|----|------|
| -32700 | 请求体不是合法 JSON |
| -32600 | 无效请求（缺 method/params 形状错误） |
| -32601 | 方法不存在 |
| -32602 | 参数错误 |
| -32603 | 服务端内部错误（dispatch 抛异常） |
| -32001 | 认证失败 |
| 1 | 任务忙/操作失败（如移除活动任务） |
| 2 | gid 不存在 |

### 查询的存储回落

引擎内存态优先；`gid → TaskId` 在引擎查不到时回落 TaskStorage
（重启后终态历史只在数据库里）。`tellWaiting`/`tellStopped`/
`getGlobalStat` 对引擎与 storage 的并集按 engine id 去重。

### 停机

`aria2.forceShutdown`/`shutdown` 调用 `set_shutdown_handler` 注册的回调
（main.cpp 接线 `DaemonManager::request_stop()`），走正常停机流程：
暂停全部任务 → 排水等待 → 状态落库（未完成任务以 Paused 入库，重启可恢复）。

---

## 关键依赖与配置

| 依赖库 | 用途 | 备注 |
|--------|------|------|
| Falcon::core / Falcon::protocols | 下载引擎与协议 | 必选 |
| nlohmann/json | JSON-RPC 编解码 | 必选 |
| SQLite3 | 任务持久化 | 可选；无 SQLite 时 daemon 可运行但不持久化 |

SQLite 可用时 `falcon_daemon_storage` 定义 `FALCON_HAS_SQLITE3`，
RPC 服务器的 storage 回落分支在该宏内。

---

## 数据模型

### 表：`tasks`（SQLite）

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY | 任务 ID（= 引擎 TaskId；create_task 支持显式插入） |
| url | TEXT | 下载 URL |
| output_path | TEXT | 输出路径（恢复时还原） |
| status | INTEGER | TaskStatus 枚举值 |
| progress | REAL | 进度（0.0 ~ 1.0） |
| total_bytes / downloaded_bytes / speed | INTEGER | 字节统计 |
| error_message | TEXT | 失败原因 |
| options_json | TEXT | DownloadOptions 序列化（getOption 回显数据源） |
| created_at / updated_at / completed_at | INTEGER | 毫秒时间戳 |

写入路径：`TaskStorageListener` 监听引擎事件实时落库（进度 1 秒节流）；
RPC 的 `pauseAll`/`unpauseAll`/`removeDownloadResult`/`purgeDownloadResult`
显式联动 storage。

---

## 测试与质量

| 测试目标 | 文件 | 覆盖 |
|----------|------|------|
| `falcon_daemon_rpc_tests` | `json_rpc_server_test.cpp` | RPC 基础 |
| `falcon_daemon_rpc_coverage_tests` | `json_rpc_server_coverage_test.cpp` | HTTP 层 + 全方法 |
| `falcon_daemon_rpc_storage_tests` | `json_rpc_storage_test.cpp` | RPC × storage 集成（回落/删除联动/批量落库/停机回调） |
| `falcon_daemon_storage_tests` | `task_storage_test.cpp` `task_storage_listener_test.cpp` | 持久化与监听器 |
| `falcon_daemon_lifecycle_tests` | `daemon_lifecycle_test.cpp` | 守护进程生命周期（POSIX） |
| `falcon_daemon_main_tests` | `main_integration_test.cpp` | 真实二进制参数/退出码（POSIX） |

```bash
ctest --test-dir build -R "JsonRpc|TaskStorage|StorageListener|DaemonLifecycle|MainIntegration"
```

---

## 常见问题 (FAQ)

### Q1：如何部署为 systemd 服务？

创建服务文件 `/etc/systemd/system/falcon-daemon.service`：

```ini
[Unit]
Description=Falcon Download Daemon
After=network.target

[Service]
ExecStart=/usr/local/bin/falcon-daemon --enable-rpc --rpc-listen-host 127.0.0.1 --rpc-listen-port 6800 --rpc-secret YOUR_TOKEN --task-db /var/lib/falcon/tasks.db
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

### Q2：如何用 aria2 客户端对接？

任意 aria2 JSON-RPC 客户端均可，例如 curl：

```bash
curl http://127.0.0.1:6800/jsonrpc -d '
  {"jsonrpc":"2.0","id":"1","method":"aria2.addUri",
   "params":["token:YOUR_TOKEN",["https://example.com/file.bin"]]}'
```

### Q3：如何监控 Daemon 状态？

- 通过 RPC 调用 `aria2.getGlobalStat` / `aria2.tellActive`
- `aria2.getSessionInfo` 获取本次会话 id

---

## 下一步开发计划

1. **事件流订阅**：基于 `websocket_server` 或 SSE 推送任务状态/进度
2. **配置文件加载**：`daemon.json`（RPC/storage/下载参数）与命令行参数合并
3. **桌面客户端对接**：apps/desktop 增加 RPC 客户端，替换进程内引擎直连
4. **认证增强**：secret 持久化、配置文件管理（当前仅命令行传入）

---

**文档维护**：每次修改 RPC 接口、数据库模式、配置格式时，请更新本文档并在"变更记录"中添加条目。
