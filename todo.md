# TODO

> 本文件是需求清单，供人阅读：每条需求一行，完成项保留一行证据
> （commit 哈希 + 测试计数）。历史工作叙述见 `CLAUDE.md` 变更记录
> 与各 commit 信息，不在本文件复述。

## 目标

将单体 `packages/libfalcon` 拆分为四个职责清晰的库
（`libfalcon-core` / `libfalcon-protocols` / `libfalcon-storage` /
`libfalcon-drives`），目录结构、CMake target、C++ namespace 三层语义
保持一致。

拆分原则：

- 下载核心抽象只放在 `core`
- 标准下载协议实现只放在 `protocols`
- 对象存储和远程资源浏览只放在 `storage`
- 网盘、分享链、账号态能力只放在 `drives`
- 基础下载协议默认内置并随发行版集成
- 云盘分享解析、对象存储浏览、第三方远端能力走插件化扩展
- 主程序只依赖稳定接口，不硬编码具体厂商实现
- 插件启用状态由安装结果和配置共同决定

## 架构决策

- **A1 基础下载协议默认内置**：`http/https`、`ftp/ftps`、
  `magnet/bittorrent`、`ed2k`、`thunder`、`flashget`、`qqdl`、
  `hls/dash` 进默认构建产物，`DownloadEngine` 默认加载内置 handler，
  保留按构建选项裁剪与运行时禁用能力。
- **A2 云能力拆分为两类扩展点**：`StorageProvider`（对象存储/远程
  文件系统/可浏览资源：s3/oss/cos/kodo/upyun/webdav/sftp）与
  `DriveResolver`（分享链/提取码/账号态/直链解析：百度/阿里云盘/夸克/
  蓝奏云/天翼云盘）不得混用同一接口。
- **A3 主程序不再硬编码厂商**：Desktop UI / `StorageService` / URL
  检测层全部改由 registry / plugin manager 提供元数据。
- **A4 引入 SPI 风格插件系统**：支持官方插件目录扫描、配置启用/禁用、
  动态库加载（`.so`/`.dylib`/`.dll`）；跨库边界用稳定 C ABI +
  `plugin_api_version`/`falcon_version` 校验。
- **A5 配置驱动启用**：配置系统支持插件目录列表、启用/禁用 ID 列表、
  每插件独立配置块。

## 目标结构

```text
packages/
├── libfalcon-core/       target: falcon_core        alias: Falcon::core
├── libfalcon-protocols/  target: falcon_protocols   alias: Falcon::protocols
├── libfalcon-storage/    target: falcon_storage     alias: Falcon::storage
├── libfalcon-drives/     target: falcon_drives      alias: Falcon::drives
├── falcon-cli/
└── falcon-daemon/
```

依赖方向（禁止反向）：`falcon_protocols -> falcon_core`、
`falcon_storage -> falcon_core`、`falcon_drives -> falcon_core`。

## 需求清单

### 第一阶段：边界冻结

- [x] 冻结三类能力边界：下载协议进 `ProtocolRegistry`、存储浏览进
  `StorageProviderRegistry`、网盘解析进 `DriveResolverRegistry`。
  - 证据：2026-04-14 验收（当前优先级 P0/P1/P2 对应本阶段）；commit
    历史早于可见记录，以可验证树状态为准（`ProtocolRegistry`/
    `BrowserFactory`/`CloudLinkDetector` 现存）。
- [x] 明确 `core` 职责：下载引擎/任务模型/事件系统/调度器/插件注册
  接口；不再暴露云存储、远程资源浏览、网盘平台概念。
- [x] 明确 `protocols` 职责：标准下载协议 + `falcon::protocols::*`
  命名空间；不再使用 `falcon::plugins` 作最终命名空间。
- [x] 明确 `storage` 职责：对象存储协议 + 远程资源浏览；
  `falcon::storage::*`；`StorageProvider` 元数据与能力描述。
- [x] 明确 `drives` 职责：网盘/分享链/提取码/认证态/直链提取；
  `falcon::drives::*`；`DriveResolver` 元数据与分享链解析接口。
- [x] 清理硬编码点：`UrlDetector` 网盘规则迁移到 drives 层
  `CloudLinkDetector`，识别面 5→13 平台。
  - 证据：2026-09-29 迁移（desktop 删除全部硬编码网盘正则与
    `parse_*_url`，识别/文件名/展示名委托 `CloudLinkDetector`）。

### 第二阶段：先拆 target，再搬目录

- [x] 顶层构建拆分：四 package 目录 + 各自 `CMakeLists.txt`，顶层改
  `add_subdirectory()`。
- [x] 建立四个库 target：`falcon_core/protocols/storage/drives`，可
  单独编译且只依赖 core。
- [x] 调整应用层链接关系：CLI/Daemon/Desktop 按需链接对应库，不因
  未启用 drives 而强带入网盘逻辑。

### 第三阶段：物理目录重组

- [x] 迁移为新目录布局：公共头路径与命名空间按库对齐
  （`<falcon/storage/resource_browser.hpp>` 等）。
  - 证据：2026-04-15（Phase 3）；24 个头文件迁移到 namespace 对齐
    路径，45+ 源文件/测试文件 include 路径更新。

### 第四阶段：接口收口

- [x] 收口 `core` 公共 API：头文件不再出现 S3/OSS/COS/Kodo/Upyun/
  CloudStorage/ResourceBrowser。
- [x] 收口配置管理：通用配置设施留 core/应用层；对象存储配置进
  storage；网盘账号与 token 配置进 drives。
- [x] 废弃旧 `falcon::plugins`：协议实现迁移到 `falcon::protocols::*`，
  新代码不再使用旧命名。
  - 证据：2026-04-15（Phase 4）；22 个文件迁移，24 个 shim 头文件
    保留并清理 `#warning`。

### 第五阶段：安装与分发

- [x] 完善安装导出：单一 `FalconConfig`（`find_package(Falcon)` 入口）
  + `FalconTargets` 导出全部 target；条件 `find_dependency` 经
  `FALCON_PACKAGE_NEEDS_*` CACHE INTERNAL 标志。
  - 证据：commit `2cd0dc2`（2026-10-13）；验收 = 外部消费工程
    （BT-off/BT-on 双前缀）`find_package(Falcon)` 冒烟
    （SHA256 空串 / BT info-hash 往返）。
- [x] 设计 `vcpkg` 包拆分：四 overlay 端口
  （`libfalcon-{core,protocols,storage,drives}`）+ feature→
  `FALCON_ENABLE_*` 映射 + `version-date` 滚动策略。
  - 证据：设计 `e62f4f1`；阶段 1 四包 standalone config
    `13433cf/05e44fc/75752e9/6ea88ae` + 标记 `3b8be34`；阶段 2 端口
    `be23cfa`（2026-10-17，x64-windows-static 实测）。
  - 验收：① 四包独立 configure+install → 单一消费工程四
    `find_package` 冒烟 `engine ok version=1.2.3 protocols=11
    presets=19`；② 单体路径 `find_package(Falcon)` 同绿；③
    `libfalcon-storage[core,s3]` 装后 S3Browser 125 处、其余浏览器
    0 处（nm 核对）；④ `libfalcon-protocols[bittorrent]` 装后
    bt 冒烟（libtorrent 2.0.11 真构建 23min）。
- [ ] vcpkg 阶段 2 残余：DLL 导出接线（共享构建产出零导出符号坏
  DLL，四端口已锁 `vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`）。
- [ ] vcpkg 阶段 2 残余：Windows 平台缺口（`falcon-daemon`
  `websocket_frame.cpp:328` MSVC 语法红 + `mock_ftp_server.hpp`
  POSIX-only 头）。
- [ ] vcpkg 阶段 3：overlay 端口上游化（默认不排期，触发判定注记见
  设计文档 §12）。

### 文档双语

- [x] `docs/` 根目录 12 份文档中英配对（`X.md`/`X.zh.md` + 语言切换器）。
  - 证据：2026-10-16 完成；`aria2c-migration-plan`（1326 行）过长仅
    英文侧占位并标注「暂无译版」。
- [ ] `docs/design/` 中英配对（vcpkg_packaging_design / mcp_server_design /
  p2sp_network_design 等）。
- [ ] `docs/api/` 中英配对。
- [ ] `docs/src/`（VitePress 站源，对外文档）中英配对。

### 功能对齐与可选增强

- [x] aria2 功能对齐 10 项全部落地：断点续传/多连接/限速/超时语义
  （停滞看门狗）/条件下载/自动改名/覆盖保护/客户端 TLS 证书/文件
  分配/文件哈希/做种份额。
  - 证据：2026-09-13 至 2026-09-30 逐项 commit（见 CLAUDE.md 变更
    记录）；SFTP（#2）2026-10-01 落地（libssh2 真实实现 +
    known_hosts 验证 + 真实 sshd 走查 4 项）。
- [ ] aria2 V2-原生化：HTTP Digest（V1 CURLAUTH_ANY 已含，V2 经适配层
  401 回退 V1 兜底）、socks/HTTPS 代理（V2 判 Unsupported 回退 V1）。
  - 注：Referer 已于 2026-10-11 原生化收编（V2 引擎发该头，适配层与
    metalink 门禁同步摘除）。
- [ ] 日志库可选增强：FMT 风格调用点迁移到 spdlog 原生 fmt 语法、
  异步 sink / 文件轮转 sink 配置化。
- [ ] 增量下载可选增强：rsync rolling-hash 算法、增量结果端到端哈希
  校验。

### P2SP 共享网络

- [x] 阶段 1+2 收口：falcon-swarmd 目录服务 + 节点侧 SwarmClient +
  daemon 公告/查询/RPC + 入站 `GET /by-sha256/<hex>` 数据服务 + swarm
  源注入 V2 镜像池 + NAT 过滤。
  - 证据：2026-09-25 至 2026-10-10（见 CLAUDE.md 变更记录）；e2e 4/4
    全绿，daemon 门禁全套件绿。
- [ ] P2SP 阶段 3（可选，按需求触发）：动态加入源/mDNS/R3/多群组，
  触发条件见设计文档 §12/§13 延后表。
- [ ] NAT/防火墙穿透评估：路线 A（公共 BT DHT 桥接）/路线 B（Falcon
  私有 DHT）落地前补一轮评估（STUN/打洞或 UPnP 映射；纯 DHT 公告对
  NAT 后节点不可达）。

### 桌面端

- [x] 桌面应用（Qt6）：Fluent 设计、亮暗主题、无边框窗口、表格/网格
  任务视图、云存储浏览、资源搜索、双后端（进程内引擎 / daemon RPC +
  WebSocket 事件刷新）、做种状态列、任务拖拽排序、warm console 重
  设计、IPC 只读端点 + 浏览器扩展 0.2.0。
  - 证据：build-desktop 全量 ctest 2610/2610（见 CLAUDE.md 变更记录）。
- [ ] 更多私有协议与网盘直链解析（百度/阿里云盘/夸克等；需点名目标
  网盘与接入方式）。
- [ ] 移动端支持。

### 质量与发布

- [x] 测试覆盖率基线收口：行覆盖 68.1% → 96.3%+，全量 ctest 2500+
  过零失败。
  - 证据：2026-09-13 至 2026-09-28 批次 A1-A11/Z2 等（见 CLAUDE.md
    变更记录）；build-cov 全量 ctest 2500/2500。
- [x] 故障注入框架：`injection.hpp`（编译期零成本）+ 37 个注入点 +
  泄漏式注入接线修复 15 处。
- [x] Windows 桌面 Qt 插件搜索修复（qt.conf + CI 产物整目录打包）+ 资源
  链路致命缺陷修复（`qt_add_resources` 无效调用，资源从未编入二进制）
  + 离屏 UI 截图沙盒。
- [x] daemon MCP server 阶段 1+2：`POST /mcp` 10+3 工具 + stdio 薄壳
  `falcon-mcp` + `GET /mcp` SSE 进度订阅。
- [ ] UI-AUDIT（P2）：原生桌面界面按 impeccable 流程补审（层级/排版/
  对比度 a11y/布局/边界态），报告+截图入 `docs/review/`，坏损当场修。
- [ ] DOC-GIF（P2）：配置开关效果动态图——枚举全部「开启后有效果」的
  开关/配置项，自动流水线生成 off/on 对比 + GIF，落 `docs`「配置效果」
  页（双语）逐开关嵌入，兼作配置效果回归测试。

## 完成定义

- [x] 仓库中不再存在承担全部能力的单体 `libfalcon`。
- [x] `core/protocols/storage/drives` 均有独立 target 和安装规则。
- [x] 协议实现不再使用 `falcon::plugins` 作为最终命名空间。
- [x] `core` 不再暴露对象存储和网盘客户端概念。
- [x] CLI/Daemon/Desktop 均能按需组合依赖（仅链 core + protocols）。
