[根目录](../../CLAUDE.md) > [packages](../) > **falcon-daemon**

---

# falcon-daemon - 后台守护进程

## 变更记录 (Changelog)

### 2026-09-14 - 覆盖率批次 I：websocket_rpc_client.cpp 99 → 9 miss + 原始 WS 测试服务器
- `websocket_rpc_client_test.cpp` 增量 18 用例（29 → 47，挂
  `falcon_daemon_rpc_client_tests`），新增 `RawWsServer` 可编程原
  始 WS 服务器基建：握手剧本（正确 101 / 半截头 EOF / 非 101 /
  错 Accept）、`on_connected` 会话钩子注入服务器帧（ping/close/
  binary）、`on_request` 请求应答脚本、客户端帧记录与等待辅助——
  与 JsonRpcServer 回环互补，帧级行为完全由测试控制
- 收口：便捷方法全簇 14 转发方法（params 归一形状服务器侧逐项断
  言 + as_gid/expect_ok 解包防御 -32600 变体）、call 失败路径（非
  数组 params 归一 / 应答超时 -32000 / 无 result 无 error -32600
  / 请求在途中断连 fail_pending -32000 "connection closed" 唤
  醒）、控制帧（ping→pong 回帧 payload 断言、close 回应后会话收
  尾、binary/pong 忽略且连接仍可用）、握手容错簇、set_url 运行期
  重定向（旧服务器下线后 call 仍成功 ⇒ 必连新端点）、parse_url
  （IPv6 字面量 / 裸主机默认 / 自定义 path 请求行）
- 剩余 9 miss 定性：238-239/494-499 时序窗口不可测（无注入点）、
  520 不可达（response 两条赋值路径均保证 object）
- **daemon 套件首次进 ASan**：build-asan `FALCON_BUILD_DAEMON`
  OFF→ON，rpc_client_tests 47 用例 ASan+UBSan 零告警；全量 ctest
  1884 零失败

### 2026-09-12 - 下载参数配置化（daemon.json "download" 节）
- `daemon.json` 新增 `download` 节：`max_concurrent_tasks`（全局并发
  任务数）、`max_overall_speed_limit`（全局总限速，字节/秒，0=不限），
  与 `aria2.changeGlobalOption`/`getGlobalOption` 已支持的键对齐
- `DownloadConfig` 用 `std::optional` 表达"文件出现才覆盖"：未出现的
  键不动引擎默认；类型错误报错、未知键告警，与既有节一致
- 启动时引擎构造后应用；SIGHUP 重载视为可热更项（引擎 setter 运行时
  可调），节值变化即重新应用
- 加固 WS 测试基建：`connect()` 返回只代表客户端收到 101，服务端会话
  线程可能尚未注册——新增 `wait_registered` 轮询辅助，修复 3 处
  即时断言/广播的注册窗口竞争（负载下偶发：BroadcastFanout 广播漏
  连接、count 断言落空）
- 测试：config 新增 4 用例（全量/optional 语义/节缺失/类型错误）+
  main 集成 1 用例（配置 → getGlobalOption 反映 → SIGHUP 热更生效）；
  daemon 全量 229 用例通过

### 2026-09-12 - SIGHUP 配置重载（daemon.json 热更新）
- `DaemonManager` 新增 `request_reload()`（仅置原子标志，
  async-signal-safe）/ `reload_pending()`；`run()` 主循环消费标志后
  在普通线程上下文执行重载回调——修复此前 `unix_signal_handler` 直接
  在信号处理器里调回调（打日志/读文件均非 signal-safe）的缺陷
- `reload()`（编程触发）保留同步语义不变
- main.cpp 重载回调：重读 `active_conf_path`（启动时实际生效的配置
  文件，`--no-conf`/默认路径未命中则跳过重载）；重载失败（文件缺失/
  JSON 非法）保持现有配置继续运行（区别于启动时硬失败），并告警
- 热更分级：`rpc.secret`/`rpc.allow_origin_all` 经
  `JsonRpcServer::update_auth` 立即生效（带锁读写，已建 WebSocket
  会话不受影响）；监听地址/端口、task_db_path、daemon 节各项变化
  告警"restart required"
- 测试：lifecycle 信号探针改断言 `reload_pending()`、run 循环消费
  路径用例；main 集成新增 2 用例（SIGHUP 换 secret 生效 + 旧 secret
  被拒、坏配置文件重载不死机保持旧 secret）；daemon 全量 225 用例通过

### 2026-09-12 - daemon.json 配置文件加载
- 新增 `daemon/config.{hpp,cpp}`：`apply_config_file` 解析 JSON 配置文件
  并应用到配置结构；三节 schema——`rpc`（enabled/host/port/secret/
  allow_origin_all）、`daemon`（run_as_daemon/pid_file/working_dir/
  log_file）、`storage`（task_db_path）
- 优先级：命令行显式参数 > 配置文件 > 内置默认值（实现：先加载配置
  文件，再走既有 argv 解析——CLI 分支只在参数显式给出时写入，天然覆盖）
- 新增参数 `--conf-path <file>`（显式指定必须存在）与 `--no-conf`
  （短路一切加载）；未指定时默认尝试 `~/.config/falcon/daemon.json`，
  存在才加载（aria2 语义）
- 容错：文件不存在/JSON 非法/类型错误 → 报错退出（在 daemonize 之前，
  错误可见）；未知节与未知键 → stderr 告警不失败（向前兼容，拼错键名
  不静默）；路径值支持 `~/` 前缀展开
- 测试：`falcon_daemon_config_tests`（12 用例：全量/部分键/错误/告警/
  展开）+ `main_integration_test` 新增 8 用例（真实二进制验证文件启动、
  CLI 覆盖、secret 生效、默认路径自动加载、--no-conf）；全量 1444 用例通过

### 2026-09-12 - WebSocket 事件流客户端（桌面端对接）
- 新增 `websocket_rpc_client.{hpp,cpp}`：`WebSocketRpcClient`——与 daemon
  维持一条 WebSocket 长连接，请求/响应同连接按 id 匹配，服务器通知经
  notification handler 推送（与 `JsonRpcClient`（HTTP）平行；桌面
  `DaemonRpcBackend` 已切换至此客户端，500ms 轮询升级为事件驱动）
  - `call()` 语义与 `JsonRpcClient::call` 完全一致（token 自动前置、
    错误码约定相同）；便捷方法（addUri/tell*/pause/...）平行复刻
  - 断线自愈：`call()` 发现未连接自动重连握手；发送失败 shutdown 加速
    读线程收尾；服务器/网络断开时挂起请求统一以 -32000 唤醒返回
  - 线程安全：`call()` 可并发（pending 表按 id 匹配 + 写帧互斥）；
    通知 handler 在读线程调用、与 `set_notification_handler` 互斥
    （setter 返回后保证无在途调用，调用方析构时序安全；handler 内可
    调 call()、不可 disconnect()）
  - 端点解析 `ws://`/`http://` 同义（与 HTTP JSON-RPC 同端点），支持
    主机名（getaddrinfo）、IPv6 字面量、可选端口（缺省 6800）；TLS 不支持
  - `set_url()` 运行期重定向端点（断开旧连接，下个 call 按新端点重连）
- `websocket_frame` 新增 `ws_encode_client_frame`（客户端掩码帧，
  thread_local mt19937_64 掩码 key）与 `ws_base64_encode`（握手 key 用）
- CMake：`falcon_daemon_rpc_client` 与 `falcon_daemon_rpc` 均纳入
  websocket_frame + websocket_rpc_client 源（重复编译同源文件既有模式）
- 测试：新增 `tests/websocket_rpc_client_test.cpp`（10 用例入
  `falcon_daemon_rpc_client_tests`）：WS 上 RPC 往返/认证/通知接收/
  断线重连/服务器停机返回/并发 id 匹配、客户端掩码帧解析回环与扩展
  长度、握手 key base64

### 2026-09-12 - WebSocket 事件流订阅（aria2 兼容通知）
- 新增 `websocket_frame.{hpp,cpp}`：RFC 6455 协议层（无 socket 依赖、无
  OpenSSL 依赖——SHA1/base64 自实现）：
  - `ws_compute_accept_key`（Sec-WebSocket-Accept）、`ws_encode_frame`
    （服务端帧，不掩码）、`WsFrameParser`（增量解析客户端帧：掩码、
    126/127 扩展长度、text 分片聚合、ping/pong/close 透传；1 MiB 消息
    上限防内存滥用，协议违规进入 error 状态）
- `JsonRpcServer` 同端口支持 WebSocket 升级（`ws://host:6800/jsonrpc`，
  aria2 真实行为，AriaNg 实时模式可直接对接）：
  - `handle_connection` 按 `Upgrade: websocket + Sec-WebSocket-Key` 分流，
    握手后进入会话循环：text 帧走与 HTTP 相同的 `handle_jsonrpc` 分发
    （含 token 认证），ping→pong，close→回应后断开
  - 删除死代码原型 `websocket_server.{hpp,cpp}`（从未接入构建，
    POSIX-only、依赖不存在的 `base64.h`，与 xml_rpc_server 同例）
- 事件流：新增内部类 `RpcEventBridge`（IEventListener，start/stop 时挂接/
  摘除引擎），引擎事件 → JSON-RPC 通知广播到全部 WebSocket 订阅者：
  - aria2 兼容：`onDownloadStart`（含 Paused 恢复）/`onDownloadPause`/
    `onDownloadComplete`/`onDownloadError`/`onDownloadStop`
  - params[0] 为 gid + Falcon 扩展进度快照（status/totalLength/
    completedLength/downloadSpeed），严格 aria2 客户端忽略未知字段
  - Falcon 扩展 `falcon.onProgress`：进度推送，每任务节流（默认 1 秒，
    `JsonRpcServerConfig::progress_push_interval` 可配）
  - `broadcast_notification` 公开（快照后逐连接发送，写互斥 per-connection；
    发送失败 shutdown 唤醒会话线程，由其统一注销，避免跨线程 close 的
    fd 复用竞争）
- 认证：WS 握手不鉴权（对齐 aria2）；WS 上的 JSON-RPC 请求逐条校验
  `token:<secret>`；通知广播不受 secret 限制
- 停机安全：`stop()` 先摘事件桥，再 shutdown 全部 WS 会话 fd 唤醒阻塞在
  recv 的会话线程——有活动订阅者时 `forceShutdown` 不再挂死
- 新增 `tests/websocket_test.cpp`（15 用例入 `falcon_daemon_rpc_tests`）：
  RFC 6455 向量、帧解析（掩码/分片/控制帧/坏操作码/超长帧/孤立
  continuation）、回环握手、WS 上的 JSON-RPC 与认证、真实引擎事件链的
  状态/进度通知与节流、广播 fan-out、关闭帧清理、活动订阅者下的停机

### 2026-09-11 - RPC 客户端库与快照转换层（桌面应用支撑）
- 新增 `aria2_snapshots.{hpp,cpp}`：纯 C++ 把 aria2 `tell*`/`getGlobalStat`
  JSON 应答转换为 `TaskSnapshot`/`GlobalStats`（容错解析：字节字段接受
  字符串/数值/缺失，坏数组条目跳过；gid 支持 16 位 hex 及可选 `0x` 前缀）
- 新增 `json_rpc_client.{hpp,cpp}`：JSON-RPC 2.0 客户端（libcurl 传输、
  `token:<secret>` 认证、超时/错误传播），封装 `addUri`/`changePriority`/
  `tell*`/`pause`/`unpause`/`remove`/`purgeDownloadResult`/
  `changeGlobalOption`/`getGlobalStat` 等桌面所需方法
- tellStatus 应答新增 Falcon 扩展字段 `priority`，供桌面恢复每任务优先级
  （storage 回落记录暂报 Normal，优先级尚未持久化）
- CMake 拆分：`falcon_daemon_rpc_client` 静态库（client + snapshots，
  链接 Falcon::core + nlohmann_json + CURL），供 apps/desktop 独立链接
  （桌面不需要 RPC 服务器）；`falcon_daemon_rpc`（daemon 本体，含服务器）
  保持原源文件列表，两组重复编译同源文件，无二进制同时链接两者
- 测试：`json_rpc_client_test`（内嵌服务器回环）与 `aria2_snapshots_test`
  纳入 `falcon_daemon_rpc_tests`

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
3. **事件流订阅**：同端口 WebSocket 升级，推送 aria2 兼容状态通知与
   Falcon 扩展进度通知
4. **任务持久化**：SQLite 状态/进度实时落库，停机保存、重启恢复
5. **多客户端支持**：无状态 HTTP 请求，天然支持多客户端并发

## 源码结构

```
packages/falcon-daemon/src/
├── main.cpp                  # 入口：参数解析 → DaemonManager 组装与接线
├── daemon/
│   ├── daemon.hpp/.cpp       # DaemonManager：生命周期、信号、pid 文件、停机排水
│   ├── config.hpp/.cpp       # daemon.json 配置文件加载（JSON → 配置结构，
│   │                         #   优先级 CLI > 文件 > 默认值；~ 路径展开）
│   └── (daemonize POSIX 细节)
├── rpc/
│   ├── json_rpc_server.hpp/.cpp  # aria2 兼容 JSON-RPC 2.0 服务器（28 个方法）
│   │                             #   + WebSocket 升级与通知广播（RpcEventBridge）
│   ├── websocket_frame.hpp/.cpp  # RFC 6455 帧编解码（握手应答/掩码/分片，
│   │                             #   自实现 SHA1+base64，无 OpenSSL 依赖）
│   ├── websocket_rpc_client.hpp/.cpp # WebSocket JSON-RPC 客户端（单连接
│   │                             #   承载请求/响应与通知；随
│   │                             #   falcon_daemon_rpc_client 库供桌面链接）
│   ├── json_rpc_client.hpp/.cpp  # JSON-RPC 2.0 客户端（libcurl；随
│   │                             #   falcon_daemon_rpc_client 库供桌面链接）
│   └── aria2_snapshots.hpp/.cpp  # aria2 JSON → TaskSnapshot/GlobalStats 转换
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

# 经配置文件运行
./build/bin/falcon-daemon --conf-path /etc/falcon/daemon.json

# 后台运行（守护进程模式）
./build/bin/falcon-daemon --daemon --enable-rpc --pid-file /var/run/falcon-daemon.pid
```

### 命令行参数

```bash
falcon-daemon [OPTIONS]

Global Options:
  -h, --help                  显示帮助
  --conf-path <file>          配置文件（JSON）；默认尝试
                              ~/.config/falcon/daemon.json（存在才加载）
  --no-conf                   不加载任何配置文件（显式 --conf-path 也忽略）

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

### 配置文件（daemon.json）

加载时机与优先级：**命令行显式参数 > 配置文件 > 内置默认值**。
配置文件在 daemonize 之前加载（pid_file/working_dir/log_file 影响守护化
行为）；显式 `--conf-path` 指定的文件必须存在且合法，否则进程报错退出；
默认路径存在才加载、不存在静默跳过。

```json
{
  "rpc": {
    "enabled": true,
    "host": "127.0.0.1",
    "port": 6800,
    "secret": "YOUR_TOKEN",
    "allow_origin_all": false
  },
  "daemon": {
    "run_as_daemon": false,
    "pid_file": "/var/run/falcon-daemon.pid",
    "working_dir": "/var/lib/falcon",
    "log_file": "/var/log/falcon/daemon.log"
  },
  "storage": {
    "task_db_path": "~/.config/falcon/tasks.db"
  },
  "download": {
    "max_concurrent_tasks": 5,
    "max_overall_speed_limit": 0
  }
}
```

- 只覆盖文件中出现的键，未出现的键保持下层值
- 路径值支持 `~/` 前缀展开（HOME / USERPROFILE）
- 配置文件给出 `pid_file` 时与 `--pid-file` 行为一致（隐含创建 PID 文件）
- 未知节/未知键打印告警到 stderr 但不失败；类型错误报错退出

**SIGHUP 热重载**：向运行中的 daemon 发送 SIGHUP（或 systemd
`ExecReload=/bin/kill -HUP $MAINPID`）会重读启动时实际生效的配置
文件。可热更项立即生效：`rpc.secret`/`rpc.allow_origin_all`（已建立
的连接不受影响）与 `download` 节（引擎 setter 运行时可调）；监听
地址/端口、`storage.task_db_path`、`daemon` 节各项变化仅告警
"restart required"。重载失败（文件缺失/JSON 非法）保持现有配置继续
运行。`--no-conf` 启动时无文件可重载，SIGHUP 记日志跳过。

---

## 对外接口

### aria2 兼容 JSON-RPC

- 端点：`http://<host>:<port>/jsonrpc`（`/` 同义），仅 POST
- 认证：params 首位 `token:<secret>`；未配置 secret 时跳过校验
- CORS：`--rpc-allow-origin-all` 时回显 `Access-Control-Allow-*`
- 批量调用：`system.multicall`（结果包装为 `[result]`）

方法清单（28 个，`system.listMethods` 列出 27 个，`shutdown` 为别名）：

| 分组 | 方法 |
|------|------|
| 任务控制 | `addUri` `pause` `forcePause` `unpause` `unpauseAll` `pauseAll` `remove` `forceRemove` `changePriority`（Falcon 扩展） |
| 查询 | `tellStatus`（含 Falcon 扩展字段 `priority`） `tellActive` `tellWaiting` `tellStopped` `getFiles` `getUris` `getOption` `getGlobalStat` |
| 选项 | `getGlobalOption` `changeGlobalOption`（支持 `max-overall-download-limit`、`max-concurrent-downloads`；`"none"`/`"0"` 取消限制） |
| 会话与清理 | `getSessionInfo` `saveSession` `purgeDownloadResult` `removeDownloadResult` `forceShutdown` `shutdown`（= forceShutdown 别名） |
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

### WebSocket 事件流订阅

- 端点：`ws://<host>:<port>/jsonrpc`（`/` 同义）——与 HTTP JSON-RPC 同端口，
  GET + `Upgrade: websocket` + `Sec-WebSocket-Key` 完成升级
- 会话内双向 JSON-RPC：text 帧即请求，与 HTTP 走同一分发（token 认证、
  全部 28 个方法可用）；ping/pong、close 按 RFC 6455 处理
- 通知推送（服务端 → 客户端，JSON-RPC 通知格式，无 id）：

| 通知 | 触发 |
|------|------|
| `aria2.onDownloadStart` | 任务进入 Downloading（含 Paused 恢复） |
| `aria2.onDownloadPause` | 任务暂停 |
| `aria2.onDownloadComplete` | 下载成功 |
| `aria2.onDownloadError` | 下载失败 |
| `aria2.onDownloadStop` | 任务取消 |
| `falcon.onProgress` | 下载中进度更新（Falcon 扩展，每任务默认 1 秒节流） |

- params[0] 均为 `gid` + Falcon 扩展快照（`status`/`totalLength`/
  `completedLength`/`downloadSpeed`；`falcon.onProgress` 另含数值型
  `progress` 0~1），aria2 严格客户端只读 gid 不受影响
- 认证：握手不鉴权（对齐 aria2），会话内每条请求需 `token:<secret>`；
  通知广播不受 secret 限制
- 对接示例（AriaNg：WebSocket 服务地址填同一 `host:port`、路径 `/jsonrpc`）；
  停机时服务器主动关闭全部订阅连接

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
| `falcon_daemon_rpc_tests` | `json_rpc_server_test.cpp` `websocket_test.cpp` | RPC 基础；WebSocket 帧协议/握手/通知/节流/停机 |
| `falcon_daemon_rpc_client_tests` | `json_rpc_client_test.cpp` `aria2_snapshots_test.cpp` `websocket_rpc_client_test.cpp` | 客户端 × 真实服务器回环 + 快照转换 + WS 客户端事件流 |
| `falcon_daemon_rpc_coverage_tests` | `json_rpc_server_coverage_test.cpp` | HTTP 层 + 全方法 |
| `falcon_daemon_rpc_storage_tests` | `json_rpc_storage_test.cpp` | RPC × storage 集成（回落/删除联动/批量落库/停机回调） |
| `falcon_daemon_storage_tests` | `task_storage_test.cpp` `task_storage_listener_test.cpp` | 持久化与监听器 |
| `falcon_daemon_lifecycle_tests` | `daemon_lifecycle_test.cpp` | 守护进程生命周期（POSIX） |
| `falcon_daemon_config_tests` | `config_test.cpp` | daemon.json 解析/优先级/容错/~ 展开 |
| `falcon_daemon_main_tests` | `main_integration_test.cpp` | 真实二进制参数/退出码/配置文件加载（POSIX） |

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

（当前无排期项；已完成：daemon.json 配置加载、SIGHUP 热重载、下载参数配置化）

---

**文档维护**：每次修改 RPC 接口、数据库模式、配置格式时，请更新本文档并在"变更记录"中添加条目。
