# Source-Layer Glossary

[简体中文](/zh/developer/glossary) | **English**

This glossary records runtime responsibilities observed in source, not package names alone. File anchors refer to the analyzed commit `28143a0b2673e065498137d9558c01e735e8298a`.

| Term | English | Definition | Evidence |
|---|---|---|---|
| 进程内下载编排 | In-process download orchestration | Owns task identities, queues, concurrency, cancellation, events, and protocol routing for callers that embed Falcon directly. | `packages/libfalcon-core/include/falcon/download_engine.hpp:16` |
| 任务管理器 | Task manager | Maintains task state, priority queues, global concurrency, background cleanup, and execution workers behind the public engine facade. | `packages/libfalcon-core/include/falcon/task_manager.hpp:22` |
| 事件分发 | Event dispatch | Asynchronously fans task status, progress, error, completion, and file metadata callbacks out from worker threads. | `packages/libfalcon-core/src/event_dispatcher.cpp:17` |
| 协议注册表 | Protocol registry | Routes URLs to protocol handlers and exposes supported schemes without knowing any concrete wire protocol. | `packages/libfalcon-core/src/protocol_registry.cpp:18` |
| 内置协议面 | Built-in protocol surface | Compile-time assembly of HTTP, FTP, BitTorrent, SFTP, WebDAV, Metalink, private, and streaming handlers into the engine registry. | `packages/libfalcon-protocols/src/builtin_protocol_handlers.cpp:39` |
| V1 协议执行 | V1 protocol execution | The per-task protocol-handler contract used by the stable engine facade and curl-based HTTP/FTP flows. | `packages/libfalcon-core/include/falcon/protocol_handler.hpp:17` |
| V2 下载引擎 | V2 download engine | An event-driven command engine for experimental HTTP data-plane work, with its own task IDs, sockets, event polling, and retry commands. | `packages/libfalcon-protocols/include/falcon/protocols/download_engine_v2.hpp:104` |
| V2 引擎宿主 | V2 engine host | Keeps one process-wide V2 instance alive behind the V1 HTTP handler and drains it only after all bridged workers return. | `packages/libfalcon-protocols/include/falcon/protocols/v2_engine_host.hpp:1` |
| 远程资源浏览 | Remote resource browsing | Lists and manipulates files on S3-compatible, OSS, COS, Kodo, Upyun, WebDAV, and SFTP endpoints through a browser abstraction. | `packages/libfalcon-storage/include/falcon/storage/resource_browser.hpp:81` |
| 云盘解析 | Cloud-drive resolution | Detects share URLs, extracts share state or direct links, and converts vendor-specific pages into download resources. | `packages/libfalcon-drives/include/falcon/drives/cloud_storage_plugin.hpp:89` |
| 云配置保管 | Cloud configuration vault | Stores named cloud credentials and settings in encrypted SQLite, with AES-256-GCM password-protected import/export. | `packages/libfalcon-drives/src/config_manager.cpp:210` |
| 资源搜索 | Resource search | Fetches configured pages, applies selector rules, normalizes magnet results, and aggregates providers behind one manager. | `packages/libfalcon-drives/include/falcon/drives/resource_search.hpp:105` |
| 下载后端选择 | Download backend selection | A desktop interface that chooses an embedded engine or a daemon RPC without changing UI command semantics. | `apps/desktop/src/services/download_backend.hpp:33` |
| 桌面资源桥 | Desktop resource bridge | Bridges Qt pages to storage/drives libraries and persists cloud profiles in QSettings. | `apps/desktop/src/services/storage_service.cpp:76` |
| 本地回收站 | Local trash | Moves deleted local files into `.falcon-trash` and atomically maintains `trash.json` for restore operations in embedded mode. | `apps/desktop/src/services/trash_store.cpp:117` |
| 用户配置目录 | User configuration directory | Holds daemon, swarm, CLI, engines, task database, and swarm identity files under a platform-specific Falcon config root. | `packages/falcon-daemon/src/daemon/config.cpp:80` |
| 命令行装配 | CLI assembly | Parses and merges CLI/file options, builds one embedded engine, attaches progress or swarm announcers, and waits for tasks. | `packages/falcon-cli/src/main.cpp:864` |
| 守护进程装配 | Daemon assembly | Restores persisted tasks, wires storage and P2SP services, starts RPC/MCP, handles signals, reload, and graceful drain. | `packages/falcon-daemon/src/main.cpp:300` |
| ARIA2 RPC 面 | aria2 RPC surface | A self-contained HTTP listener exposing aria2-compatible JSON-RPC methods, token auth, CORS, and WebSocket notifications. | `packages/falcon-daemon/src/rpc/json_rpc_server.cpp:1086` |
| MCP 工具面 | MCP tool surface | Maps MCP tool calls to the daemon's shared aria2 method dispatcher over Streamable HTTP and SSE. | `packages/falcon-daemon/src/rpc/mcp_server.cpp:473` |
| 任务持久化 | Task persistence | Stores tasks, progress, errors, and terminal state in WAL-backed SQLite and restores active or paused IDs after restart. | `packages/falcon-daemon/src/storage/task_storage.cpp:25` |
| 持久化监听 | Persistence listener | Converts engine events into throttled SQLite writes, flushing final progress at terminal and pause transitions. | `packages/falcon-daemon/src/storage/task_storage_listener.cpp:14` |
| P2SP 数据节点 | P2SP data node | Announces completed resources and serves local bytes to peers over a dedicated HTTP endpoint. | `packages/falcon-daemon/src/daemon/swarm_announcer.cpp:1` |
| P2SP 集会服务 | P2SP rendezvous service | An isolated authority for node sessions, challenges, resource catalogs, TTL sweeps, rate limits, and WebSocket updates. | `packages/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:3` |
| SWARM 协议 | Swarm protocol | Defines signed registration, heartbeat, announce, retract, query, unsubscribe methods, notifications, and application error codes. | `packages/falcon-swarmd/src/common/swarm_protocol.hpp:30` |
| SWARM 会话 | Swarm session | Holds in-memory node, session, challenge, and resource tables under one mutex; it does not survive process restart. | `packages/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:6` |
| 群密钥身份 | Swarm key identity | Loads or creates a persistent node key, derives signed requests, and shares one machine-local identity between CLI and daemon. | `packages/falcon-swarmd/src/client/swarm_key_store.cpp:1` |
