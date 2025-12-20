[根目录](../../CLAUDE.md) > [packages](../) > **falcon-daemon**

---

# falcon-daemon - 后台守护进程

## 变更记录 (Changelog)

### 2025-12-21 - 初始化模块架构
- 创建 Daemon 项目结构
- 定义 RPC 接口设计（gRPC/REST）
- 规划任务持久化方案

---

## 模块职责

`falcon-daemon` 是 Falcon 下载器的后台守护进程，提供以下能力：

1. **常驻后台**：作为系统服务持续运行
2. **RPC 接口**：提供远程任务管理（gRPC 或 REST API）
3. **任务持久化**：将任务信息保存到数据库（SQLite）
4. **多客户端支持**：支持多个客户端（CLI/GUI/Web）同时连接
5. **权限控制**：支持 Token 认证与权限管理

---

## 入口与启动

### 编译与运行

```bash
# 编译
cmake -B build -DFALCON_BUILD_DAEMON=ON
cmake --build build --target falcon-daemon

# 前台运行（调试用）
./build/bin/falcon-daemon --config daemon.json

# 后台运行（守护进程模式）
./build/bin/falcon-daemon --daemon --config /etc/falcon/daemon.json

# systemd 服务（Linux）
sudo systemctl start falcon-daemon
sudo systemctl enable falcon-daemon
```

### 命令行参数

```bash
falcon-daemon [OPTIONS]

Options:
  -h, --help                显示帮助信息
  -V, --version             显示版本信息
  -c, --config <FILE>       配置文件路径
  -d, --daemon              守护进程模式（后台运行）
  --pid-file <FILE>         PID 文件路径
  --log-file <FILE>         日志文件路径
  -v, --verbose             详细日志输出
```

---

## 对外接口

### RPC 接口设计（gRPC）

#### 服务定义（`proto/falcon_service.proto`）

```protobuf
syntax = "proto3";

package falcon.rpc;

service FalconService {
  // 任务管理
  rpc CreateTask(CreateTaskRequest) returns (TaskResponse);
  rpc GetTask(GetTaskRequest) returns (TaskResponse);
  rpc ListTasks(ListTasksRequest) returns (ListTasksResponse);
  rpc PauseTask(TaskIDRequest) returns (StatusResponse);
  rpc ResumeTask(TaskIDRequest) returns (StatusResponse);
  rpc CancelTask(TaskIDRequest) returns (StatusResponse);
  rpc RemoveTask(TaskIDRequest) returns (StatusResponse);

  // 全局控制
  rpc GetGlobalStatus(Empty) returns (GlobalStatusResponse);
  rpc SetGlobalSpeedLimit(SpeedLimitRequest) returns (StatusResponse);
  rpc PauseAll(Empty) returns (StatusResponse);
  rpc ResumeAll(Empty) returns (StatusResponse);

  // 事件订阅（流式）
  rpc SubscribeEvents(SubscribeRequest) returns (stream TaskEvent);
}

message CreateTaskRequest {
  string url = 1;
  DownloadOptions options = 2;
}

message TaskResponse {
  uint64 task_id = 1;
  string url = 2;
  TaskStatus status = 3;
  float progress = 4;
  uint64 total_bytes = 5;
  uint64 downloaded_bytes = 6;
  uint64 speed = 7;
}

message TaskEvent {
  uint64 task_id = 1;
  EventType type = 2;
  string message = 3;
}

enum TaskStatus {
  PENDING = 0;
  DOWNLOADING = 1;
  PAUSED = 2;
  COMPLETED = 3;
  FAILED = 4;
  CANCELLED = 5;
}

enum EventType {
  STATUS_CHANGED = 0;
  PROGRESS_UPDATE = 1;
  ERROR = 2;
}
```

### REST API 设计（备选方案）

如果不使用 gRPC，可使用轻量级 HTTP 服务器（如 cpp-httplib）：

```
POST   /api/tasks                  创建任务
GET    /api/tasks/:id               获取任务详情
GET    /api/tasks                   列出所有任务
PUT    /api/tasks/:id/pause         暂停任务
PUT    /api/tasks/:id/resume        恢复任务
DELETE /api/tasks/:id               取消任务
GET    /api/status                  全局状态
PUT    /api/config/speed-limit      设置全局速度限制
GET    /api/events                  SSE 事件流
```

---

## 关键依赖与配置

### 编译依赖

| 依赖库 | 用途 | 版本 | 备注 |
|--------|------|------|------|
| libfalcon | 核心下载库 | 0.1.0 | 必选 |
| gRPC | RPC 框架 | 1.40+ | 可选（或用 REST） |
| SQLite3 | 任务持久化 | 3.30+ | 必选 |
| spdlog | 日志 | 1.9+ | 必选 |

### 配置文件（`daemon.json`）

```json
{
  "rpc": {
    "host": "127.0.0.1",
    "port": 6800,
    "enable_auth": true,
    "auth_token": "your-secret-token"
  },
  "storage": {
    "task_db_path": "/var/lib/falcon/tasks.db",
    "auto_cleanup_completed": true,
    "completed_task_retention_days": 7
  },
  "download": {
    "max_concurrent_tasks": 5,
    "global_speed_limit": 0,
    "default_download_dir": "/var/lib/falcon/downloads"
  },
  "log": {
    "level": "info",
    "file": "/var/log/falcon/daemon.log",
    "max_size_mb": 10,
    "max_files": 5
  },
  "daemon": {
    "pid_file": "/var/run/falcon-daemon.pid",
    "user": "falcon",
    "group": "falcon"
  }
}
```

---

## 数据模型

### 数据库模式（SQLite）

#### 表：`tasks`

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY | 任务 ID |
| url | TEXT | 下载 URL |
| output_path | TEXT | 输出路径 |
| status | TEXT | 任务状态 |
| progress | REAL | 进度（0.0 ~ 1.0） |
| total_bytes | INTEGER | 总大小 |
| downloaded_bytes | INTEGER | 已下载大小 |
| created_at | TIMESTAMP | 创建时间 |
| updated_at | TIMESTAMP | 更新时间 |
| completed_at | TIMESTAMP | 完成时间 |
| options_json | TEXT | 配置选项（JSON） |

#### 表：`task_events`

| 字段 | 类型 | 说明 |
|------|------|------|
| id | INTEGER PRIMARY KEY | 事件 ID |
| task_id | INTEGER | 关联任务 ID |
| event_type | TEXT | 事件类型 |
| message | TEXT | 事件消息 |
| timestamp | TIMESTAMP | 时间戳 |

---

## 测试与质量

### 测试策略

#### 1. 单元测试（`tests/unit/`）

- `rpc_server_test.cpp`：RPC 服务器基础测试
- `task_storage_test.cpp`：数据库持久化测试
- `auth_test.cpp`：认证逻辑测试

#### 2. 集成测试（`tests/integration/`）

- `daemon_lifecycle_test.cpp`：守护进程启动/停止测试
- `client_server_test.cpp`：客户端与服务器交互测试
- `multi_client_test.cpp`：多客户端并发测试

### 测试覆盖率目标

- RPC 接口：≥ 80%
- 持久化逻辑：≥ 90%
- 整体覆盖率：≥ 70%

---

## 常见问题 (FAQ)

### Q1：如何部署为 systemd 服务？

创建服务文件 `/etc/systemd/system/falcon-daemon.service`：

```ini
[Unit]
Description=Falcon Download Daemon
After=network.target

[Service]
Type=forking
User=falcon
Group=falcon
ExecStart=/usr/local/bin/falcon-daemon --daemon --config /etc/falcon/daemon.json
PIDFile=/var/run/falcon-daemon.pid
Restart=on-failure

[Install]
WantedBy=multi-user.target
```

启用服务：

```bash
sudo systemctl enable falcon-daemon
sudo systemctl start falcon-daemon
```

### Q2：如何实现认证？

使用 Token 认证（HTTP Header 或 gRPC Metadata）：

```
Authorization: Bearer your-secret-token
```

服务端验证 Token 是否与配置文件中的 `auth_token` 匹配。

### Q3：如何监控 Daemon 状态？

- 使用 `systemctl status falcon-daemon`（systemd）
- 通过 RPC 接口调用 `GetGlobalStatus`
- 查看日志文件（`/var/log/falcon/daemon.log`）

---

## 相关文件清单

```
packages/falcon-daemon/
├── CMakeLists.txt
├── CLAUDE.md
├── src/
│   ├── main.cpp              # 主入口
│   ├── daemon.cpp            # 守护进程逻辑
│   ├── rpc_server.cpp        # gRPC 服务器实现
│   ├── task_storage.cpp      # SQLite 持久化
│   ├── auth_manager.cpp      # 认证管理
│   └── event_broadcaster.cpp # 事件广播器
├── proto/
│   └── falcon_service.proto  # gRPC 协议定义
└── tests/
    ├── unit/
    └── integration/
```

---

## 下一步开发计划

### 第一阶段（基础功能）
1. 实现守护进程框架（信号处理、PID 文件）
2. SQLite 任务持久化
3. 基础 RPC 接口（创建/查询/控制任务）
4. 配置文件加载

### 第二阶段（高级功能）
1. gRPC 服务器完整实现
2. Token 认证
3. 事件流订阅（实时进度推送）
4. systemd 服务配置

### 第三阶段（企业级特性）
1. 用户与权限管理（多用户支持）
2. HTTPS/TLS 加密（gRPC over TLS）
3. 任务调度策略（时间段、带宽分配）
4. 监控与指标（Prometheus 集成）

---

**文档维护**：每次修改 RPC 接口、数据库模式、配置格式时，请更新本文档并在"变更记录"中添加条目。
