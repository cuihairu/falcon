# Falcon MCP Server 设计（AI 助手驱动下载）

状态：**阶段 1 已实现**（daemon `/mcp` 端点 + 10 工具翻译层 + Bearer 鉴权/会话管理，
单测 + 真实二进制冒烟收口，实现对账见 §8）。**阶段 2 已拍板开工（2026-10-08 巡检）**：
stdio 薄壳/进度订阅/全局选项与做种工具，按 §7 拆原子增量逐项实施（进度见 daemon
CLAUDE.md 变更记录）。
调研参考件：[Lakr233/FlowDown](https://github.com/Lakr233/FlowDown)（AGPL-3.0，★1.2k）。
只参考接口设计与交互形态，不抄代码。

---

## 1. 调研结论（FlowDown 参考件）

### 1.1 FlowDown 是什么

Apple 平台（iOS/iPadOS/macCatalyst）的**原生 AI 聊天客户端**：Swift 6 + UIKit，本地 MLX 推理 +
OpenAI 兼容端点接入，隐私立场激进（零遥测、零数据留存、源码可审计）。**它不是下载器**——
唯一的下载能力是「下载 AI 模型本体」（见 1.4）。

### 1.2 功能面盘点

| 功能面 | 形态 | 与 falcon 的关系 |
|---|---|---|
| AI 会话 | 多模型对话、Markdown 渲染、视觉/音频附件、模板 | falcon 不做（非定位） |
| 模型接入 | OpenAI 兼容 API、Ollama/LM Studio 本地推理、Apple Intelligence、`.fdmodel` 配置导入导出 | falcon 不做 |
| 工具调用（ModelTools） | 内建 16 个工具 + MCP 外接工具，同一条 tool-calling 管线 | **MCP 部分是本设计的参考核心** |
| 内建工具 | 日历(2)/提醒(5)/网页搜索+抓取(2)/定位(1)/长期记忆(5)/URL 元数据 | falcon 不做（非定位） |
| MCP | **纯客户端**：连接外部 MCP 服务器，把其工具注入模型 | 详见 1.3 |
| 下载 | 仅 HuggingFace 模型文件下载 | 见 1.4 |
| 外部自动化 | Apple Shortcuts（App Intents）、Live Activity 后台流式 | falcon 不做（平台绑定） |
| 同步 | iCloud（CloudKit）会话/配置同步 | falcon 不做 |
| 隐私 | 零遥测、零数据留存 | falcon 同立场（本就无遥测） |

### 1.3 它的 MCP 实现（调研重点）

**更正调研前提：FlowDown 没有「MCP server」——它是纯 MCP 客户端**，让 AI 模型调用外部
MCP 服务器提供的工具。`FlowDown/Backend/MCPService/`（~1670 行）+ 单元测试 6 文件，
包装官方 swift-sdk 的 `MCP` 包。官网 `.well-known/agent.json` 是 agent 发现卡（描述官网本体），
不是 MCP 端点。其外部自动化面实际由 Apple Shortcuts 承担。

客户端设计要点（对 falcon 反向设计有直接参考价值）：

- **传输只支持 Streamable HTTP**（POST `/mcp/`）：沙盒禁 stdio、SSE 已废弃——两者明确不做，
  不给用户模糊预期。每服务器配置：`endpoint` / `headers`（JSON map，认证凭据随每请求）/
  `timeout`（秒，默认 60）/ `nickname` / `isEnabled` + **工具级开关**（toolsEnabled/resourcesEnabled/
  templateEnabled 三组）。
- **连接管理健壮性三件套**（`MCPService.swift`）：
  ① 有界等待——`conversationWaitTimeout` 15s，会话准备阶段单个服务器挂死只损失它自己的工具，
  不拖死整轮对话（「a hung server loses its tools for this round instead of hanging the whole
  conversation setup」注释原文）；
  ② 会话丢失自愈——`callTool` 失败先尝试替换连接（`replaceConnectionIfSessionLost`），
  短等新会话后让本轮后续调用接上，替换失败保留死连接按普通工具错误收口；
  ③ 工具清单缓存——`toolInfoCache` 按 serverID 缓存 listTools 结果，未过期不重询。
- **接入 UX**：`Verify Configuration` 按钮 = 握手 + listTools 一次性验证，列表显示
  Connected/Connecting/Disconnected/Connection Failed 四态徽章；`.fdmcp` 文件导入导出/分享
  （服务器配置快照，可 AirDrop/文件打开直导）。
- **调用管线与确认**：MCP 工具与内建工具走同一 `ModelTool` 协议——模型发函数名+JSON 参数 →
  确认弹窗（显示服务器名/工具名/描述，全局 Skip Tool Confirmation 豁免）→ 执行 → 结果文本
  内联进推理面板、image/audio 内容转附件。**确认权在 host**，工具提供方不重复实现。

### 1.4 它的下载能力组织方式

`ModelManager+Hub.swift`：HuggingFace `/api/models/{id}/tree/main?recursive=1` 列文件树 →
逐文件交给 `Digger`（第三方下载库）多任务下载 → `HubDownloadProgress` 聚合整体进度/单文件
进度/速度/取消。无断点续传控制面、无队列、无多源——一次性树下载即弃。对 falcon 无直接
可取之处（通用下载引擎 falcon 已远超），唯一启示是「树浏览 + 批量下载 + 聚合进度」的交互
形态（对应 falcon 已有的资源浏览/批量添加）。

### 1.5 对 falcon 的反向定位

FlowDown 的方向：**让 AI 调用外部工具**。falcon 的方向相反：**把自己作为工具暴露给 AI**——
用户在 Claude Desktop / Claude Code / Cursor 等 MCP host 里说「把这个链接下到 D 盘、
下完告诉我」，host 经 MCP 调 falcon 的工具完成添加/查询/控制。falcon 侧只需实现
**MCP server**；工具确认、多轮编排、参数补全都由 host 承担，falcon 保持纯工具面。

---

## 2. 协议契约

### 2.1 传输与形态

| 项 | 决策 | 理由（沿 FlowDown 先例） |
|---|---|---|
| 传输 | **Streamable HTTP**：`POST /mcp`（JSON-RPC 2.0 请求，响应 `application/json` 或 `text/event-stream`） | MCP 当前主推形态；FlowDown 只做这一种，工程量与兼容面最优 |
| stdio | 阶段 2 可选薄壳（`falcon-mcp` 命令转发 daemon RPC） | AI 编码工具（Claude Code 等）生态大量假设 stdio；但 daemon 已有 HTTP 面，薄壳纯转发 |
| 端点 | daemon 同端口新增 `/mcp` 路由（RPC 端口 6800） | 复用既有监听/鉴权/token 配置，不多开端口 |
| 会话 | 实现 `Mcp-Session-Id` 响应头 + 后续请求校验 | Streamable HTTP 规范要求；会话表带上界防泄漏 |
| 鉴权 | `Authorization: Bearer <token>`，复用 daemon `rpc.secret`；daemon 未开 RPC 时 `/mcp` 一并关闭 | 单一事实源，不发明第二套凭据 |
| 初始化 | `initialize` → 返回 `capabilities: { tools: {} }` + `serverInfo: { name: "falcon", version }` | 客户端（host）按 capability 协商，只暴露 tools |
| 协议版本 | 协商 `protocolVersion`，支持当前稳定版；不认识的版本按规范回退 | |

### 2.2 结果与错误约定

- 每个工具返回 `content: [{type:"text"}]`（人类可读摘要）+ **`structuredContent`**
  （机器可读 JSON，与 text 内容一致）——host 的模型两用：文本进对话、结构体供程序消费。
- 业务错误（gid 不存在/任务非暂停态）返回 `isError: true` + 明确文本，**不用** JSON-RPC
  层错误码表达业务失败（协议层错误只留给协议违规）——与 daemon RPC 的 code 语义对齐翻译。
- 字节/时长一律显式单位命名（`total_length_bytes`、`seeded_seconds`），速度附 `bytes/s`
  语义说明在工具描述里，防模型误读。

---

## 3. 工具清单（10 个，映射既有 daemon RPC）

命名 `falcon_` 前缀，参数/返回 JSON Schema 全部显式（`additionalProperties: false`）。
映射列是既有 aria2 兼容 RPC 方法——MCP 层是**薄适配**，不新写业务逻辑。

| 工具 | 参数（要点） | 返回要点 | 映射 |
|---|---|---|---|
| `falcon_add_download` | `urls[]`（必填）、`output_dir?`、`filename?`、`options?`（proxy/limit/retry/seed 等 aria2 键位白名单） | `gid`、`task_id` | `aria2.addUri` |
| `falcon_list_tasks` | `status?`（active/waiting/stopped/all）、`offset?`、`limit?` | 快照数组（gid/状态/进度/速度/尺寸/输出路径） | `tellActive`+`tellWaiting`+`tellStopped` 聚合 |
| `falcon_get_task` | `gid`（必填）、`keys?` | 单任务全量快照（含 BT 做种扩展字段） | `aria2.tellStatus` |
| `falcon_pause_task` | `gid` | `gid` | `aria2.pause` |
| `falcon_resume_task` | `gid` | `gid` | `aria2.unpause` |
| `falcon_remove_task` | `gid`、`force?`（默认 false；force 走 `forceRemove`） | `gid` | `aria2.remove`/`forceRemove` |
| `falcon_pause_all` / `falcon_resume_all` | 无 | `OK` | `pauseAll`/`unpauseAll` |
| `falcon_get_global_stats` | 无 | 活跃/等待/暂停计数、上传/下载速度 | `aria2.getGlobalStat` |
| `falcon_get_task_files` | `gid` | 文件清单（路径/长度/完成片段） | `aria2.getFiles` |

阶段 2 追加（依赖阶段 1 稳定，不在首批）：

| 工具 | 说明 | 映射 |
|---|---|---|
| `falcon_set_global_option` / `falcon_get_global_option` | 全局限速/并发等 | `changeGlobalOption`/`getGlobalOption` |
| `falcon_stop_seeding` | BT 手动停止做种 | `falcon.stopSeeding` |
| 进度订阅 | 进度通知经 MCP `notifications`/resource 推送，替代模型轮询 | 复用既有 WS 事件桥 |

**轮询指引写进工具描述**：MCP 无推送（阶段 2 前），`falcon_add_download` 的返回描述明确
「用 falcon_get_task(gid) 轮询 status==complete」——模型自行决定节奏，host 侧有超时兜底。

**破坏性标注**：`falcon_remove_task`/`falcon_pause_all` 标 `annotations: { destructiveHint: true }`；
`falcon_add_download` 标 `readOnlyHint: false, openWorldHint: false`。确认交互交给 host
（FlowDown 的确认弹窗同理是 host 职责，工具方不重复实现 UI）。

---

## 4. 架构落点

- **形态归属**：MCP server 挂 **daemon**（`falcon-daemon` 包）。桌面 InProcess 模式不暴露
  MCP——MCP 的价值前提是常驻服务（AI host 随时连），桌面进程内引擎生命周期随 GUI，
  暴露无意义。daemon.json 新增 `mcp` 节（`enabled`，默认 false；endpoint 复用 rpc 节端口）。
- **实现路径**：daemon 既有 `JsonRpcServer` 的分发器直接复用——MCP 层做三件事：
  ① `/mcp` 路由接入（握手/会话/Streamable HTTP 帧）；② `tools/list` 静态清单（10 个 schema，
  编译期常量）；③ `tools/call` 的 name→aria2 方法参数翻译表。**零新增业务逻辑**。
- **健壮性沿 FlowDown 客户端三件套反向落实**（服务端视角）：`tools/list` 响应内存快取
  （静态清单，零成本）；会话表有界 + `Mcp-Session-Id` 失效即 404 清理（防泄漏）；
  单会话请求超时沿用 daemon RPC 既有处理。
- **stdio 薄壳**（阶段 2）：独立小二进制读 stdin 写 stdout，内部转发 daemon HTTP RPC——
  不链 libfalcon-core，避免第二个进程内引擎形态。
- **不做**：falcon 自身不做 MCP host/客户端（不连接外部 MCP 服务器——falcon 不是 AI
  会话产品，FlowDown 的 MCPService 没有镜像价值）；不做内建 AI 工具（日历/搜索/记忆）。

---

## 5. 安全

- 默认 `127.0.0.1` + Bearer token（复用 rpc.secret）；未配 secret 时 `/mcp` 拒绝一切请求。
- 工具面最小化：首批 10 个工具不含 `forceShutdown`/`saveSession`/`purgeDownloadResult`——
  AI 驱动场景无必要，缩小误操作面（需要时用户在 daemon RPC 直接调用）。
- 防提示注入的下界：工具描述不拼接任何运行时数据（静态字符串），进度/状态只从
  structuredContent 返回——模型不会把服务器返回内容当工具指令执行（host 侧职责，
  falcon 不放大）。

---

## 6. 功能差距对照表（FlowDown 有 / falcon 缺）

| # | FlowDown 能力 | falcon 现状 | 做/不做 | 理由 |
|---|---|---|---|---|
| 1 | MCP 客户端（连外部 MCP 服务器扩 AI 工具） | 无 | **不做** | falcon 非 AI 会话产品；falcon 方向是 MCP **server**（本文档） |
| 2 | MCP server / 把自身暴露为工具 | 无 | **做**（阶段 1：10 工具） | 本文档核心；让 AI host 驱动下载 |
| 3 | 工具确认弹窗 + Skip Tool Confirmation | 无 | **不做** | 确认权归 MCP host，工具方不重复实现（FlowDown 亦然） |
| 4 | `.fdmcp` 服务器配置导入导出分享 | 无 | **做简版** | README 提供各 host（Claude Desktop 等）的接入 JSON 片段 + 一键复制即可，无需自造文件格式 |
| 5 | 有界等待/会话丢失自愈/工具清单缓存 | 部分（RPC 层超时既有） | **做对应项** | 服务端镜像：会话表有界+失效清理、list 快取；客户端自愈无对应物 |
| 6 | HuggingFace 模型树浏览 + 逐文件下载 + 聚合进度 | 无专项 | **不做** | 通用多源下载引擎 falcon 已有（metalink/分段/BT）；HF 专项集成非定位，需要时用户贴直链/metalink 即可 |
| 7 | 内建 AI 工具（日历/提醒/定位/记忆/网页搜索抓取） | 无 | **不做** | 非 falcon 定位 |
| 8 | Apple Shortcuts / Live Activity / iCloud 同步 | 无 | **不做** | 平台绑定；外部自动化面由 MCP（跨平台）承担 |
| 9 | 零遥测/零数据留存 | 已有（无任何遥测） | 立场一致 | — |
| 10 | 多协议下载引擎（断点续传/多源/限速/做种） | **falcon 有，FlowDown 缺** | 已有 | 参考件无对应物，不构成差距 |

---

## 7. 实施边界

- **阶段 1**（一次增量）：daemon `/mcp` 路由 + initialize/握手/会话 + `tools/list`（10 工具）
  + `tools/call` 翻译层 + daemon.json `mcp` 节 + 单测（握手往返/清单 schema 校验/翻译表
  参数往返/错误语义）+ 真实 host 冒烟（MCP inspector 回环）。README 补接入片段（差距表 #4）。
- **阶段 2**（独立排队）：stdio 薄壳、进度订阅（notifications/resource）、全局选项与做种工具。
- 明确不做：MCP 客户端、内建 AI 工具、GUI 内嵌对话界面。
- 文档对账：实现落地时同步 daemon CLAUDE.md/README 标注「已实现」，本文档状态行改写；
  未实现前本文档保持「设计稿」字样（不写假文档）。

---

## 8. 实现对账（阶段 1，2026-10-07）

阶段 1 落地与本文档契约的差异如实披露（未列处逐条一致）：

- **响应形态**：阶段 1 只回 `application/json`，未实现 `text/event-stream` 响应流
  （进度订阅属阶段 2）；`GET /mcp`（SSE 监听位）回 405 + `Allow: POST, DELETE`。
- **协议版本**：支持 `2025-06-18`（默认）与 `2025-03-26`；客户端声明的更高版本
  回退默认版本。
- **`falcon_list_tasks` 聚合形态**：`status=waiting|stopped` 走 aria2 原生
  `(offset, limit)` 分页；`all`（默认）= `tellActive`（全量）+ `tellWaiting(0,100000)`
  + `tellStopped(0,100000)` 三队列合并后统一切片——aria2 的三个 tell* 队列没有
  贯通的分页游标，聚合只能在翻译层切。
- **鉴权基线**：`mcp.enabled` 且**未配 `rpc.secret` 时 `/mcp` 整体 403**（§5
  「未配 secret 拒绝一切请求」的落地形态）；配了 secret 走
  `Authorization: Bearer`，失败 401 + `WWW-Authenticate: Bearer`。注意与
  `/jsonrpc` 的语义差：后者未配 secret 是跳过校验放行。鉴权先于方法路由
  （无凭据 GET 得 401 而非 405）。
- **会话表**：进程内存表，上限 64（满逐最旧）；`initialize` 忽略客户端携带的
  会话头恒新建会话。
- **batch 与标量**：数组 batch 回 400 + -32600（2025-06-18 已移除 batch）；
  非 object 标量 body 回 200 + -32600 信封（落进 JSON-RPC 分发的请求形状校验）。
- **冒烟形态**：设计写「MCP inspector 回环」，实际以 curl 回环 9 项代替
  （initialize 握手 + tools/list 清单 + structuredContent 往返 + 通知 202 /
  DELETE 204 / 过期会话 404 / 鉴权 401 / 未配 secret 403 / GET 405）——
  本机无 inspector 运行环境。
- **顺带修复的产品缺陷**：`new_session_id()` 初版循环 4 次产出 64 hex（256 bit），
  与自身注释「32 hex（128 bit）」矛盾——改 ×2 后 32 hex，测试按本契约钉死。
