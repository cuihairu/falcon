# 源码分层术语表

[English](/developer/glossary) | **简体中文**

本表记录从源码归纳出的运行时职责，而不是单纯复述包名。文件锚点对应分析提交 `28143a0b2673e065498137d9558c01e735e8298a`。

| 术语 | 英文 | 定义 | 证据 |
|---|---|---|---|
| 进程内下载编排 | In-process download orchestration | 为直接嵌入 Falcon 的调用方拥有任务标识、队列、并发、取消、事件和协议路由。 | `packages/libfalcon-core/include/falcon/download_engine.hpp:16` |
| 任务管理器 | Task manager | 维护任务状态、优先级队列、全局并发、后台清理，并在公共引擎门面后执行任务。 | `packages/libfalcon-core/include/falcon/task_manager.hpp:22` |
| 事件分发 | Event dispatch | 从工作线程异步分发任务状态、进度、错误、完成和文件元数据回调。 | `packages/libfalcon-core/src/event_dispatcher.cpp:17` |
| 协议注册表 | Protocol registry | 将 URL 路由到协议处理器，只暴露支持的 scheme，不了解具体线上协议。 | `packages/libfalcon-core/src/protocol_registry.cpp:18` |
| 内置协议面 | Built-in protocol surface | 将 HTTP、FTP、BitTorrent、SFTP、WebDAV、Metalink、私有协议和流媒体处理器按编译条件装配进引擎注册表。 | `packages/libfalcon-protocols/src/builtin_protocol_handlers.cpp:39` |
| V1 协议执行 | V1 protocol execution | 稳定引擎门面和 curl 版 HTTP/FTP 流程使用的逐任务协议处理器契约。 | `packages/libfalcon-core/include/falcon/protocol_handler.hpp:17` |
| V2 下载引擎 | V2 download engine | 面向实验性 HTTP 数据面的事件驱动命令引擎，有独立任务 ID、socket、事件轮询和重试命令。 | `packages/libfalcon-protocols/include/falcon/protocols/download_engine_v2.hpp:104` |
| V2 引擎宿主 | V2 engine host | 在 V1 HTTP 处理器后维持进程级唯一 V2 实例，并在桥接 worker 全部返回后排水停机。 | `packages/libfalcon-protocols/include/falcon/protocols/v2_engine_host.hpp:1` |
| 远程资源浏览 | Remote resource browsing | 通过浏览器抽象列出和操作 S3 兼容、OSS、COS、Kodo、Upyun、WebDAV、SFTP 端点上的文件。 | `packages/libfalcon-storage/include/falcon/storage/resource_browser.hpp:81` |
| 云盘解析 | Cloud-drive resolution | 识别分享 URL，提取分享态或直链，把厂商页面转换为可下载资源。 | `packages/libfalcon-drives/include/falcon/drives/cloud_storage_plugin.hpp:89` |
| 云配置保管 | Cloud configuration vault | 将命名的云凭证和设置存入加密 SQLite，并用 AES-256-GCM 提供密码保护的导入导出。 | `packages/libfalcon-drives/src/config_manager.cpp:210` |
| 资源搜索 | Resource search | 抓取配置页面、应用选择器规则、归一 magnet 结果，并由统一管理器聚合多个 provider。 | `packages/libfalcon-drives/include/falcon/drives/resource_search.hpp:105` |
| 下载后端选择 | Download backend selection | 桌面端接口，可在内嵌引擎和 daemon RPC 之间切换，而不改变 UI 命令语义。 | `apps/desktop/src/services/download_backend.hpp:33` |
| 桌面资源桥 | Desktop resource bridge | 把 Qt 页面桥接到 storage/drives 库，并用 QSettings 持久化云配置。 | `apps/desktop/src/services/storage_service.cpp:76` |
| 本地回收站 | Local trash | 在内嵌模式下把删除的本地文件移入 `.falcon-trash`，原子维护 `trash.json` 以支持还原。 | `apps/desktop/src/services/trash_store.cpp:117` |
| 用户配置目录 | User configuration directory | 在平台相关的 Falcon 配置根下保存 daemon、swarm、CLI、engines、任务数据库和 swarm 身份文件。 | `packages/falcon-daemon/src/daemon/config.cpp:80` |
| 命令行装配 | CLI assembly | 解析并合并命令行与文件选项，构建内嵌引擎，挂接进度或 swarm 公告器，然后等待任务。 | `packages/falcon-cli/src/main.cpp:864` |
| 守护进程装配 | Daemon assembly | 恢复持久化任务，装配存储与 P2SP 服务，启动 RPC/MCP，处理信号、热载和优雅排水。 | `packages/falcon-daemon/src/main.cpp:300` |
| ARIA2 RPC 面 | aria2 RPC surface | 自包含 HTTP 监听器，暴露 aria2 兼容 JSON-RPC 方法、token 鉴权、CORS 和 WebSocket 通知。 | `packages/falcon-daemon/src/rpc/json_rpc_server.cpp:1086` |
| MCP 工具面 | MCP tool surface | 通过 Streamable HTTP 和 SSE，把 MCP 工具调用映射到 daemon 共享的 aria2 方法分发器。 | `packages/falcon-daemon/src/rpc/mcp_server.cpp:473` |
| 任务持久化 | Task persistence | 在 WAL SQLite 中保存任务、进度、错误和终态，重启后按原 ID 恢复活跃或暂停任务。 | `packages/falcon-daemon/src/storage/task_storage.cpp:25` |
| 持久化监听 | Persistence listener | 把引擎事件转换为按时间节流的 SQLite 写入，并在终态与暂停转换时冲刷最终进度。 | `packages/falcon-daemon/src/storage/task_storage_listener.cpp:14` |
| P2SP 数据节点 | P2SP data node | 公告已完成资源，并通过独立 HTTP 端点向对端提供本地字节。 | `packages/falcon-daemon/src/daemon/swarm_announcer.cpp:1` |
| P2SP 集会服务 | P2SP rendezvous service | 隔离的节点会话、挑战、资源目录、TTL 清扫、限频和 WebSocket 更新权威。 | `packages/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:3` |
| SWARM 协议 | Swarm protocol | 定义带签名的注册、心跳、公告、撤回、查询、退订方法，以及通知和应用错误码。 | `packages/falcon-swarmd/src/common/swarm_protocol.hpp:30` |
| SWARM 会话 | Swarm session | 在单把 mutex 下保存内存节点、会话、挑战和资源表；进程重启后不保留。 | `packages/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:6` |
| 群密钥身份 | Swarm key identity | 加载或创建持久节点密钥，推导签名请求，并让 CLI 与 daemon 共享同机身份。 | `packages/falcon-swarmd/src/client/swarm_key_store.cpp:1` |
