# TODO

> 本文件是需求清单，供人阅读：每条需求一行，完成项保留一行证据
> （commit 哈希 + 测试计数）。历史工作叙述见 `CLAUDE.md` 变更记录
> 与各 commit 信息，不在本文件复述。

## 目标

将单体 `libs/libfalcon` 拆分为四个职责清晰的库
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
libs/
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
- [x] vcpkg 阶段 2 残余：DLL 导出接线（共享构建产出零导出符号坏
  DLL，四端口已锁 `vcpkg_check_linkage(ONLY_STATIC_LIBRARY)`）。
  - 证据：commit `87686da`——MSVC `WINDOWS_EXPORT_ALL_SYMBOLS` 四包接线 +
    `FALCON_VERSION` 改 C++17 inline 变量消除 extern 数据 dllimport 面 +
    四插件恒 STATIC（消除 DSO missing from command line）；共享构建树
    `ctest` 3106/3106 绿，`nm -D libfalcon_protocols.so` 实测 1089 导出符号
    含 `create_*_handler` / `extract_info_hash`；commit `fea09e8` 解锁四端口
    `vcpkg_check_linkage` 与 protocols/storage 的 `BUILD_SHARED_LIBS=OFF`
    覆盖，REF/SHA512 钉 `87686da`，version-date 2026-11-26；vcpkg overlay
    实测 `libfalcon-core:x64-linux@2026-11-26` 静态装出 `libfalcon_core.a`
    3.4 分钟绿 + 87686da tarball SHA512 校验通过 + toolchain 注入
    `BUILD_SHARED_LIBS:UNINITIALIZED=OFF` 实证；CI run 45666563447
    linux/macOS/cl build 全绿。
- [x] vcpkg 阶段 2 残余：Windows 平台缺口（`falcon-daemon`
  `websocket_frame.cpp:328` MSVC 语法红 + `mock_ftp_server.hpp`
  POSIX-only 头）。
  - 证据：commit `87686da`——`websocket_frame.cpp` 语法修正 +
    `mock_ftp_server.hpp` Windows 分支（POSIX 头 `#ifndef _WIN32` 门控）；
    根与四包补 MSVC `/utf-8` 编译选项；桌面树增量重建绿，精灵全量
    `ctest` 唯一红 `MainIntegrationTest.EnableRpcFalsyVariantsAllRun`
    串行复跑绿 + 全量二进制 46/46（并行负载抖动，既有定性）；CI run
    45664628552/45666563447 `build (windows-latest, Release, cl)` 双腿
    Build+Test 全绿。
- [ ] vcpkg 阶段 3：overlay 端口上游化（默认不排期，触发判定注记见
  设计文档 §6）。

### 文档双语

- [x] `docs/` 根目录 12 份文档中英配对（`X.md`/`X.zh.md` + 语言切换器）。
  - 证据：2026-10-16 完成；`aria2c-migration-plan`（1326 行）过长仅
    英文侧占位并标注「暂无译版」。
- [x] `docs/design/` 中英配对（vcpkg_packaging_design / mcp_server_design /
  p2sp_network_design 等）。
  - 证据：2026-11-26 收口（docs 提交后补勾）；`docs/design/` 内
    mcp_server_design（250 行 zh）/ vcpkg_packaging_design（252 行 zh）
    双 .zh.md 均带 `[English] | **中文**` 切换器、内容为真实译版；
    p2sp_network_design 实际位于 `docs/` 根（782 行 zh 译版 + 切换器），
    已配对；子目录配对全清——prototypes/README、ui-sandbox/README、
    redesign-2026-11/{index,cold,graphite,warm}/README 四主题 README
    均 .zh.md 成对；docs/design 下无第三份英文设计稿。
- [x] `docs/api/` 中英配对。
  - 证据：2026-11-26 收口；`docs/api/`（CLAUDE.md 目录树中的 Doxygen
    生成位）从未落地为目录，无内容可配对——实际 API 文档为
    `docs/api/`（core/events/plugins/index 四份），EN/ZH 已成对
    见下条，配对目标以实际所在位置计量即满足。
- [x] `docs/`（VitePress 站源，对外文档）中英配对。
  - 证据：2026-11-26 收口；26 份 EN 内容文件与 `zh/` 下 26 份一一对集
    （api 4 + developer 10 + faq 1 + guide 4 + index 1 + protocols 6），
    零占位（无「暂无译版/TODO 翻译」），抽查 zh 均为真实译版
    （zh/guide/getting-started.md 173 行、zh/api/core.md 272 行）；
    `.vitepress/config.mts` `locales: {root, zh}` 双 locale 已接线。

### 功能对齐与可选增强

- [x] aria2 功能对齐 10 项全部落地：断点续传/多连接/限速/超时语义
  （停滞看门狗）/条件下载/自动改名/覆盖保护/客户端 TLS 证书/文件
  分配/文件哈希/做种份额。
  - 证据：2026-09-13 至 2026-09-30 逐项 commit（见 CLAUDE.md 变更
    记录）；SFTP（#2）2026-10-01 落地（libssh2 真实实现 +
    known_hosts 验证 + 真实 sshd 走查 4 项）。
- [x] aria2 V2-原生化：HTTP Digest（V1 CURLAUTH_ANY 已含，V2 经适配层
  401 回退 V1 兜底）、socks/HTTPS 代理（V2 判 Unsupported 回退 V1）。
  - 证据（三阶段全绿后推 main）：① HTTP Basic/Digest 原生 401 应答链
    `fcfdce3`（V2 引擎内 digest nonce/cnonce/response 计算，回退表摘除
    401 项）；② socks5 原生 `38f1cad`（greeting/auth/CONNECT 三步握手
    + 用户名密码认证，回退表摘除 socks 项）；③ HTTPS 代理（TLS-over-TLS
    隧道）`fa98a9e`（代理 TLS 握手 → CONNECT 隧道 → 隧道内 origin TLS
    双层泵 pump_tls_in_tls + Basic 仅交代理 + 证书校验；TLS-in-TLS 停机
    30s 挂死根修 kIdle motor + EOF-after-data 按已交付数据判推进）；
    新套件 DownloadEngineV2HttpsProxy 6/6 + 关联回归 218 过 +
    falcon_protocols_tests 全量 1047 过（4 skip 设计内）；适配层与
    metalink 门禁的 proxy 回退项同步摘除（剩余回退仅 cookie 引擎）。
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
- [ ] LANG-AUTO-001 [P2] 文档站浏览器语言自动识别（fleet-docs-audit-G2
  2026-11-14 调查项）：现况 = vitepress locales 双 locale（root=en-US /
  zh=zh-CN，`docs/.vitepress/config.mts:148`）+ 导航语言下拉手动切换 +
  页内显式互链；**无 rootLocale 自动跳转、无 navigator.language 探测 JS**。
  语言自动切换为增强项，默认「手动切换 + 显式链接」为受规基线，待用户拍板
  是否上自动识别（同舰队拍板项，勿单仓先行实施）。

## 完成定义

- [x] 仓库中不再存在承担全部能力的单体 `libfalcon`。
- [x] `core/protocols/storage/drives` 均有独立 target 和安装规则。
- [x] 协议实现不再使用 `falcon::plugins` 作为最终命名空间。
- [x] `core` 不再暴露对象存储和网盘客户端概念。
- [x] CLI/Daemon/Desktop 均能按需组合依赖（仅链 core + protocols）。

## 桌面 UI 重设计第二轮（用户令 2026-11-07 晚：falcon UI 重新设计）

- [x] UI-RD-001 [P1] 桌面 UI 重设计提案：先原型后实现（铁律）。产出=完整功能面原型集（主视图表格/网格、添加对话框、云盘、资源发现、设置、回收站、托盘菜单、亮暗双主题——13 视图面全覆盖，复用 ui-sandbox 真组件渲染管线与现有 design tokens），给出 2-3 个完整设计方向（warm console 迭代版 / cold utility 迭代版 / 新方向），每个方向出关键视图渲染图+对比说明，落 `docs/design/redesign-2026-11/`（含 index 对比页）。**只做原型，不改生产代码**；用户拍板方向后才排实现批次。原型图由 ui-sandbox 离屏渲染产出（真实 Qt 组件树），不许手绘假图。
  - 交付（commit `fc124bf`）：3 方向（A warm console II / B cold utility II / C graphite emerald）× 14 功能面 × 亮暗双主题 = 84 张原型图 + 每方向双语 README（token 表与 `ui_sandbox.cpp` 六映射表逐 hex 核对一致）+ index.md 对比决策页；全部 24 对色 WCAG 检查 ≥4.5:1（A 暗色 text 16.73 / B 17.03 / C accent 于窗底 9.78）；生产代码零改动（`--redesign` 沙盒模式）。待用户拍板方向后排实现批次。

- [ ] UI-RD-002 [P1] 桌面 UI 精修 + 三主题落地（2026-11-21 用户拍板「三套 ui 都是一样的，只是改了配色……根据这套 ui 精修下，把这几套主题都安装上，可以在配置里切换，默认第一个」）：warm 原型为唯一视觉真相，禁自由发挥。
  - [x] A1 形状参数进令牌体系（commit `9e7c6a5`）：三档圆角 token（卡片 12 / 控件 8 / 小件 4）经 `render_qss` 数值替换写入 QSS 占位符（两 QSS 各 18 处：5+7+6），渲染产物与旧字面值逐字节一致（视觉零变化批次）。**证据**：① 静态——占位符回填 12/8/4 后的模板与 HEAD 原版逐字节相等（light 12858B / dark 13157B 长度精确相等且内容 IDENTICAL）；② 像素——stash 四文件回 HEAD 重建沙盒出基线后比对，52 张中 44 张逐字节一致，余 8 张落在同一噪声集内（同一二进制连跑两次即有 7 张字节不同——演示时钟文本致 PNG 尺寸抖动，噪声值与 A/B 差集完全吻合）；③ 门禁——build-desktop 全量重建 148/148 零 error，本批 4 文件零告警，ctest 桌面 113 例全过（28 条 NOT_BUILT 为其他包测试二进制未纳入本桌面前端树，越界）；④ 残留字面 2px/3px 保持不令牌化——`radius=height/2` 几何派生惯例（QProgressBar 4→2、#taskProgressBar 6→3），非独立设计值。**已记窗口控制钮半径成对违例**：`fluent_light.qss` `#windowButton border-radius: 0`（注释「无边框贴边」）vs `fluent_dark.qss` `6px`（注释「与全局 8px 圆角语言统一，不再贴边直角」）——dark 已改圆角 light 未跟；字面保留到 B 项对照 warm 原型裁决后同步。
  - [x] A2 三 family 全量令牌化（commit `4f0fef5`）：Warm 亮暗两列迁移到变体表列（变体表为唯一事实源——`make_tokens(family, theme)` 三 family switch，Warm 暗色窗口 #1b1714→#16120f / 卡片 #262019→#221c16 / 文字 #f2ede8→#f7f2ec、亮色窗口 #f6f4f1→#f7f5f2 / 文字 #292524→#262220 一并重调），Cold/Graphite 四组合已一致零迁移；checkbox 指示器 SVG 按 per-(family, theme) 生成 12 枚（`checkbox-{checked,unchecked}-{cold,graphite,warm}-{dark,light}.svg`，qrc 12 别名），`{{checkbox_*}}` 占位符经 `family_key`+`theme_key` 接线（切 Cold/Graphite 后不再残留鲑橙）；顺带修复关闭钮 hover 白叉不可着色——新增 `x-on-light.svg`（stroke #ffffff 预烘，QSS `icon:url()` 上下文 currentColor 不可控）。**证据**：① 静态——qrc 12 别名逐一在位 + 变体表三 family switch 双主题列齐；② 门禁——build-desktop 全量重建 195/195 零 error（本批零告警），desktop 8 测试二进制 113/113；③ 沙盒——`--redesign` 3 方向 × 14 功能面 × 亮暗 = 84 PNG 逐张像素级核对（暖窗 #16120f / 冷 #131519 / 石墨 #111418 精确色命中）。
  - [x] A3 family 贯穿三站点（commit `8c0c218`）：`theme_manager.cpp` `apply_stylesheet`、`icon_utils.cpp` role_color、`speed_float_widget.cpp` restyle 的局部 QSS 均改取当前 family（现取 Warm 或无 family 通道）。**证据**：① 静态——三站点取色点全部经 `tokens_for(family, theme)` 双参（theme_manager `apply_stylesheet`/`stylesheet`、icon_utils `role_color`、speed_float `restyle`），`family_changed` 信号与 `apply_family` 槽对称接线（局部 QSS 消费方跟随）；② 门禁——build-desktop 增量重建绿（本批零新告警），desktop 8 测试二进制 113/113。
  - [x] A4 设置页三级切换（commit `8ec9aa2`）：family 下拉（Warm Console II / Cold Utility II / Graphite Emerald）+ 亮暗 + 跟随系统，选择即存 QSettings（`FalconTeam/Falcon.conf`）热生效。**证据**：① 静态——三站点接线齐（ThemeManager `ThemeMode` 枚举/`set_theme_mode`/`detect_system_theme`/`eventFilter` ThemeChange 跟随 + `mode()`/`family()` 访问器；SettingsPage `set_theme_{mode,family}_display` QSignalBlocker 回写（程序化回写不回环）+ `theme_{mode,family}_changed` 信号，下拉索引与枚举值对齐；MainWindow 构造器读 `theme_family`/`theme_mode` 首同步 + `on_theme_{mode,family}_changed` 槽 → ThemeManager + QSettings 三键 `theme_mode`/`theme_family`/`theme`（theme 键保持旧读取路径与顶栏切换路径一致）+ sync；模式下拉不跟随 `theme_changed`（显式选择语义——「跟随系统」下解析出的亮暗不移动下拉）、家族下拉跟随 `family_changed`）；② 门禁——build-desktop 增量重建绿（本批零新告警），desktop 8 测试二进制 113/113；GUI 行为面待 C 项 Xvfb 走查截图（视觉类条目待用户亲验）。
  - [x] A5 默认 warm（commit `4f8a2f7`）：无 QSettings 的全新安装默认 Warm Console II + 亮色。**证据**：① 静态——MainWindow 构造器读路径缺省 `theme_family="warm"`/`theme_mode` 回落旧 `theme`→`"light"`（A4 已落）+ ThemeManager 自身默认态收口（`current_theme_` Dark→Light，与 `current_mode_(Light)` 自洽，独立构造/单测不再报「Dark 主题 + Light 模式」）；② 门禁——build-desktop 增量重建绿（零告警），desktop 8 测试二进制 113/113；③ 像素——Xvfb :95 + `XDG_CONFIG_HOME` 空目录模拟全新安装，1200×800 截图中主导背景 `#f7f5f2`（A2 Warm 亮色窗口 token 精确命中）+ accent `#c2410c` 2388 px，零 Cold/Graphite 背景残留；QSettings 文件启动后仅含 clipboard 键（主题缺省不落盘，用户改选才写）。
  - [x] B 布局精修对照 warm 原型（`docs/design/redesign-2026-11/warm/` 28 张）：差异清单逐项落本文件并逐项勾。方法=归档 28 面对生产 QSS 修复后复渲染（`--redesign` 同管线 84 张取 warm 28 张）双阈值像素普查（pct30=像素差>30 / pct150=强差异>150 + 强差异 bbox 定位）+ 形状属性全表成对审计。清单与裁决：
    - [x] B1 完全一致 12/28 面（0.000% 双阈值）：add_dialog / settings_about / settings_search / settings_seeding / settings_trash / tray_menu × 亮暗——零差异收口。
    - [x] B2 时钟带抖动 12/28 面（强差异 237–257 px，bbox ≈(1061,20,1249,35) 顶栏时钟行）：cloud / discovery / download_completed(_grid) / download_grid(_table) × 亮暗——sandbox demo 时钟文本为渲染时刻时间戳（归档 2026-11-08/09 vs 复渲 2026-11-22），文本换字即整段像素差，bbox 恒锁时钟行、其余区域零强差异，非布局漂移；其中 cloud/discovery/download_completed_grid 三亮色面另含搜索框光标闪烁 16 点 (144,19)–(144,30)（复渲采样落光标显示相位，n30 795 vs 779），同源 demo 抖动。
    - [x] B3 trash × 2 面（强差异 483/503 px，bbox (1012,21,1249,337)）：时钟 + 回收站 demo「删除时间」列时间戳（归档 2026-11-07/05/06 vs 复渲 2026-11-21/19/20），bbox 纵向恰到该列文本底——时间戳文本抖动，非漂移。
    - [x] B4 settings × 2 面（强差异 light 6169 / dark 3620 px）：唯一结构性差异 = A4 新增主题选择行——**归档原型（2026-11-08/09）早于 A4（`8ec9aa2`，2026-11-21），判定为预期功能差异而非漂移**；下游各分组 ~2px 垂直位移为新增行推开内容所致，同属 A4 交付面（A4 门禁已 113/113）。
    - [x] B5 唯一真漂移修复（commit `101d757`）：`fluent_light.qss` `#windowButton/#closeButton` `border-radius: 0` → `6px`，注释同步 dark 同款「圆角胶囊按钮(与全局 8px 圆角语言统一,不再贴边直角)」——A1 记录的成对违例（上文「dark 已改圆角 light 未跟」）按原型裁决收口：warm 原型两主题窗口钮同为圆角胶囊，light 必须跟 dark。静息态按钮 `background: transparent; border: none;`，半径在静态渲染不可见（B6 复验零新增差异即证），视觉生效面为 hover/pressed 背景出现时与 dark 一致的 6px 圆角。
    - [x] B6 复验三件：① 形状属性全表成对审计（border-radius/border-width/padding/min-width/min-height/margin/width/height）修复后 0 处亮暗不一致；② 修复后 28 面复渲双阈值普查与修复前逐面同性质（B1–B4 分布不变、零新增差异）；③ 桌面门禁 113/113（QT_QPA_PLATFORM=offscreen，#3108–3220）。
    结论：28 面除 A4 预期功能差异（B4）与已记录 demo 时间戳抖动（B2/B3：时钟文本、光标相位、删除时间）外，唯一真漂移 = B5 半径成对违例，已修——warm 原型对照收口，无其他布局漂移。
  - [ ] C 证据与验收：三 family × 亮暗切换截图入 `docs/acceptance-evidence/ui-rd-002/`；**视觉类条目待用户亲验，agent 不清项**。

## 源码分层分析 2026-11-07

分析提交 `28143a0b2673e065498137d9558c01e735e8298a`（先通读源码再归纳，层=运行时职责而非目录名；双语正文在 `docs/developer/{glossary,glossary.zh}.md` 与 `docs/{developer,zh/developer}/architecture.md`）。

### 抽象层清单（7 层）

- L1 引擎领域与调度：任务/队列/并发/事件/协议路由，进程内存态（`libs/libfalcon-core/src/download_engine.cpp:372`、`libs/libfalcon-core/include/falcon/download_engine.hpp:16`）。
- L2 协议与传输数据面：V1 契约 + V2 命令引擎宿主，传输与分段工件，CMake 选择（`libs/libfalcon-protocols/src/builtin_protocol_handlers.cpp:39`、`libs/libfalcon-protocols/include/falcon/protocols/v2_engine_host.hpp:1`）。
- L3 远端资源与云访问：对象存储/远端浏览、网盘分享解析、选择器搜索、可选加密 SQLite 配置（`libs/libfalcon-storage/include/falcon/storage/resource_browser.hpp:81`、`libs/libfalcon-drives/include/falcon/drives/cloud_storage_plugin.hpp:89`、`libs/libfalcon-drives/src/config_manager.cpp:210`）。
- L4 呈现层与本地应用状态：Qt 页面/后端选择、QSettings、trash.json、engines.json（`apps/desktop/src/services/download_backend.hpp:33`、`apps/desktop/src/main_window.cpp:281`、`apps/desktop/src/services/trash_store.cpp:117`）。
- L5 服务装配与进程生命周期：CLI 前台单次运行与 daemon 长生命周期、任务恢复、P2SP 接线、排水（`libs/falcon-cli/src/main.cpp:864`、`libs/falcon-daemon/src/main.cpp:300`、`libs/falcon-daemon/src/main.cpp:340`）。
- L6 控制面协议与持久化：HTTP/WS/JSON-RPC/MCP 复用方法分发器，WAL SQLite 任务库与事件监听器（`libs/falcon-daemon/src/rpc/json_rpc_server.cpp:575`、`libs/falcon-daemon/src/rpc/mcp_server.cpp:473`、`libs/falcon-daemon/src/storage/task_storage.cpp:25`）。
- L7 P2SP 集会与数据面：内存集会状态、独立 HTTP/WS 权威、PEM 身份、daemon 公告与 Range 数据服务（`libs/falcon-swarmd/src/rdv/swarm_rdv_state.hpp:3`、`libs/falcon-swarmd/src/common/swarm_protocol.hpp:30`、`libs/falcon-daemon/src/daemon/swarm_data_service.cpp:193`）。

### 超大文件候选（>1000 行）

- [x] LAY-001 [P1] 拆分 `libs/libfalcon-protocols/src/commands/http_commands.cpp`（3640 行）——HTTP 多连接分段、TLS/代理、文件分配与 V2 重试命令混合在单 TU（`libs/libfalcon-protocols/src/commands/http_commands.cpp:61`）（commit `c7c23ae`）。
- [x] LAY-002 [P1] 拆分 `libs/falcon-daemon/src/rpc/json_rpc_server.cpp`（1979 行 → 967 行）——aria2 方法分发 `dispatch_rpc` 拆到独立 TU，共享内部辅助（序列化/鉴权/分页）落 `json_rpc_server_detail.hpp`（commit `bfbabba`；build-desktop 全量 ctest 3219/3219）。
- [ ] LAY-003 [P1] 拆分 `apps/desktop/src/pages/download_page.cpp`（1666 行）——任务表格/网格视图、筛选、操作菜单与 QSettings 记忆集中（`apps/desktop/src/pages/download_page.cpp:81`、`apps/desktop/src/pages/download_page.cpp:847`）。
- [ ] LAY-004 [P1] 拆分 `apps/desktop/src/main_window.cpp`（1372 行）——窗口装配、侧栏/页面创建、托盘、IPC、设置与 40+ 信号槽共存（`apps/desktop/src/main_window.cpp:187`、`apps/desktop/src/main_window.cpp:935`）。
- [ ] LAY-005 [P1] 拆分 `apps/desktop/src/pages/settings_page.cpp`（1346 行）——设置项 UI 与 engines.json 编辑/校验/持久化混合同页（`apps/desktop/src/pages/settings_page.cpp:791`、`apps/desktop/src/pages/settings_page.cpp:910`）。
- [ ] LAY-006 [P1] 拆分 `libs/libfalcon-protocols/src/incremental_download.cpp`（1246 行）——增量差分/全量回退/哈希校验与流控同文件（`libs/libfalcon-protocols/src/incremental_download.cpp:1055`）。
- [ ] LAY-007 [P1] 拆分 `libs/libfalcon-protocols/plugins/http/http_handler.cpp`（1182 行）——HTTP 头探测、分段入口、V1/V2 分流与 curl 适配同文件（`libs/libfalcon-protocols/plugins/http/http_handler.cpp:1`）。
- [ ] LAY-008 [P1] 拆分 `apps/desktop/src/pages/cloud_page.cpp`（1178 行）——云连接表单、配置持久化与目录浏览 UI 共存（`apps/desktop/src/pages/cloud_page.cpp:24`、`apps/desktop/src/pages/cloud_page.cpp:911`）。
- [ ] LAY-009 [P1] 拆分 `libs/libfalcon-core/src/task_manager.cpp`（1140 行）——队列/调度/恢复/清理/做种与统计集中在单实现（`libs/libfalcon-core/src/task_manager.cpp:191`）。
- [ ] LAY-010 [P1] 拆分 `libs/libfalcon-drives/src/cloud_storage_plugin.cpp`（1132 行）——12+ 厂商插件与 HTTP 抓取/解析混居（`libs/libfalcon-drives/src/cloud_storage_plugin.cpp:320`）。
- [ ] LAY-011 [P1] 拆分 `libs/libfalcon-protocols/src/download_engine_v2.cpp`（1079 行）——命令执行、socket 事件、节流窗口、任务状态与清理同引擎（`libs/libfalcon-protocols/src/download_engine_v2.cpp:458`）。
- [ ] LAY-012 [P1] 拆分 `libs/falcon-cli/src/main.cpp`（1075 行）——参数装配、交互控制、进度显示与 P2SP 公告在单 TU（`libs/falcon-cli/src/main.cpp:864`、`libs/falcon-cli/src/main.cpp:1047`）。
- [ ] LAY-013 [P1] 拆分 `libs/libfalcon-protocols/plugins/metalink/metalink_handler.cpp`（1013 行）——Metalink 解析/镜像选择/下载流程同文件（`libs/libfalcon-protocols/plugins/metalink/metalink_handler.cpp:1`）。
- [ ] LAY-014 [P1] 拆分 `libs/libfalcon-protocols/plugins/bittorrent/dht_node.cpp`（1008 行）——DHT 节点路由/令牌/桶刷新与 UDP 网络态共文件（`libs/libfalcon-protocols/plugins/bittorrent/dht_node.cpp:1`）。

### 目录/分类不合理处

- `libs/libfalcon-drives/include/falcon/{cloud_storage_plugin.hpp,config_manager.hpp,resource_search.hpp}` 与 `libs/libfalcon-drives/include/falcon/drives/` 双份同名公共头并存（根 `falcon/` 兼容转发 + `falcon/drives/` 实际定义，见 `libs/libfalcon-drives/include/falcon/drives/config_manager.hpp:38`）。历史兼容转发可保留，但新增公共 API 应只落 `falcon/drives/`，避免两个真值源。
- `libs/libfalcon-protocols/src/commands/http_commands.cpp`（3640 行）是 V2 命令面，却按「commands」目录名放在 protocols/src 下，而其配套头在 `include/falcon/protocols/commands/http_commands.hpp`（979 行），公共接口与私有实现目录语义不一致；且 `incremental_download.cpp`/`download_engine_v2.cpp` 等 V2 传输实现未归入 `commands/`，命令目录并未覆盖全部 V2 文件。
- `docs/` 已完成整理：历史设计稿已合并（仍有价值的进入 `developer/` 与 `api/`）或删除，`ARCHIVE.md` 已移除；VitePress 源直接位于 `docs/` 根目录，核心文档与 `ui/` 设计文档分模块维护。
- `libs/libfalcon-protocols/plugins/*` 中每个插件既有 `*_plugin.cpp` 又有 `*_handler.cpp`（如 `http/http_plugin.cpp` 与 `http/http_handler.cpp`、`sftp/sftp_plugin.cpp` 与 `sftp/sftp_handler.cpp`），目录名按插件聚合，文件级再按 plugin/handler 命名，两层粒度并存；建议统一为目录内文件职责命名，避免同一插件两种命名习惯。

## Codex 深审发现（2026-11-13，luna profile 近5 commit 审核，0C/1M/2m，待修）

- [ ] 将 active family 贯穿 QPalette、图标与速度悬浮窗（`apps/desktop/src/utils/theme_manager.cpp:153`、`apps/desktop/src/utils/icon_utils.cpp:63`、`apps/desktop/src/widgets/speed_float_widget.cpp:256`）——消除 Cold/Graphite 切换后的混合主题。
- [ ] 为 `ThemeManager::set_family()` 增加回归测试（`apps/desktop/tests/CMakeLists.txt:21`）——断言 QSS、Palette、图标 token 与悬浮窗随家族一致变化，并覆盖缓存失效。
- [ ] 恢复 `pnpm/action-setup@v6.1.1`（`.github/workflows/docs-check.yml:31`、`.github/workflows/docs-deploy.yml:29`）——撤销依赖补丁回退。
- [ ] 修正 UI-RD-001 的功能面数量记录（`todo.md:215`）——与 `fc124bf` 实际交付的 14 个功能面、84 张图保持一致。
- [ ] 补齐或重新约定 `todo.md` 已勾选项证据（`todo.md:3`）——使“commit 哈希 + 测试计数”的完成标准可就地验证。
- [x] OWRT-001 [P1] OpenWrt .ipk 包装 + LuCI 传输面板（2026-11-14 用户令）：①可行性侦察=核心风险 vcpkg 依赖 musl 编译（逐端口核实，跑不通缩小包面 daemon+cli 先行，swarmd 不硬凑）②packaging/openwrt 三分包（falcon/luci-app-falcon/app-meta-falcon 照 meshlet 先例，procd init UCI→daemon.json 渲染/conffiles/幂等）③LuCI 传输面板四卡（服务状态/活动传输 tellActive/全局统计/最近日志）+操作面（addUri 加任务/暂停/继续/删除——daemon aria2 兼容 JSON-RPC 直接当数据源，零引擎改动）④真机 192.168.5.11 验收（装/升/conffile/四卡/真实小文件下载任务走通）⑤nightly musl 双架构出包（aarch64 vcpkg 卡住则 x86_64 先行如实标注）+app-meta（iStore 后续）。验收：musl 编译结论+真机 opkg 全链路+LuCI 加下载任务走通+结构检查（data.tar.gz 整树归档——meshlet opkg 父目录 bug 教训）。
  - ① 结论（2026-11-15 侦察）：PASS——**方案 FALCON_USE_SYSTEM_LIBS=ON 系统库直链，零 vcpkg**。alpine:3.20（musl 1.2.5）容器内全源码构建成功：GNU 13.2.1，configure 2.3s exit 0，build [73/73] exit 0，产物 falcon-cli 1482808 B / falcon-daemon 2183312 B；strptime/timegm 系统自带（vendored libmetalink 的 MSVC compat 补编路径 musl 不需要）；SQLite3 3.45.3 系统包。包面裁剪：FALCON_ENABLE_{BITTORRENT,NAT}=OFF + FALCON_BUILD_{SWARMD,DESKTOP,TESTS,EXAMPLES}=OFF，HTTP/FTP/SFTP/WebDAV/METALINK=ON——aarch64 无需再探（已绕开 vcpkg 交叉编译主风险），⑤双架构出包路径打通。
  - ② 进展（2026-11-18）：**全静态 musl 构建打通 + 三分包结构检查过**。alpine:3.22 容器 `-static` 全静态链接成功（falcon-cli 9546296 B / falcon-daemon 11745528 B，`file` 报 statically linked、`ldd` 拒读 not a dynamic executable、expat xmlparse.c 调试串嵌入二进制=静态 expat 直证）；冒烟 = `falcon-cli --version` → v0.2.0 + daemon `aria2.getVersion` JSON-RPC 有效应答。静态配方五要素：命令行缓存种子强解析静态库（CMAKE_FIND_LIBRARY_SUFFIXES 在 project() 内被平台初始化覆写，种子是唯一权威）+ 链接尾 `-l` 短形式（musl `-static` 固定 libc 搜索列表，c-ares 需精确名 `-l:libcares.a`）+ `CMAKE_PROJECT_INCLUDE` 预建 `expat::expat` UNKNOWN IMPORTED 指向 libexpat.a（根因：alpine expat-dev 的 CMake config 包只导出 SHARED target，libmetalink 的 `if(NOT TARGET expat::expat)` 守卫跳过 FindEXPAT 模块路径，-DEXPAT_LIBRARY 种子永不被消费 → 带 soname 的 .so 进链接行即 "attempted static link of dynamic object"）+ `PKG_CONFIG_STATIC=1`（libssh2 经 PkgConfig::LIBSSH2 转 .a，SFTP ON）+ `-static` 经 `CMAKE_EXE_LINKER_FLAGS_INIT`。`packaging/openwrt/build-ipk.sh` 产出三分包：falcon_0.2.0-1_x86_64（9.2MB，conffiles=/etc/config/falcon+/etc/init.d/falcon，postinst 幂等+条件重启）/ luci-app-falcon_0.2.0-1_all（依赖 falcon+curl）/ app-meta-falcon_0.2.0-1_all（依赖 luci-app-falcon）；结构检查三 ipk 外层 = debian-binary+control.tar.gz+data.tar.gz，data.tar.gz 整树目录项齐全（meshlet opkg 父目录 bug 教训），procd init UCI→/var/etc/falcon-daemon.json 渲染逻辑在位。
  - ③⑤ 交付与 CI 验收（2026-11-18 收口）：LuCI 三文件（controller 270 行/cbi 86 行/status.htm 212 行，commit 264edb0）——四卡数据源全部走 daemon aria2 兼容 JSON-RPC（getGlobalStat/tellActive/tellWaiting/日志 logread 经 shell），操作面 addUri/pause/unpause/remove；lua5.1 -p 双文件语法过。nightly run 44289801554 全 7 job 绿，release 12 资产含三 ipk（x86_64 + aarch64 qemu 仿真构建全绿）+ SHA256SUMS；本机 `sha256sum -c` 四 ipk 全 OK（含 aarch64）。**真机验收曝光两缺陷随 3a92913 修复**：① procd init 缺 `config_load falcon`（config_get 读空缓存恒返默认 enabled=0 → 服务恒 "active with no instances"；对照 custos/herald/persona/aria2 四在役 init 全有 config_load）；② ipk tar 未归零属主（CI runner uid 1001 骑进归档，真机呈现为不存在 uid；六处 tar 补 `--owner=0 --group=0`）。
  - ④ 真机验收（2026-11-18，iStoreOS 24.10.8 x86_64 @ 192.168.5.11）：三分包 opkg install 顺序 falcon→luci→meta 干净；文件布局全在位（conffile/init/双 bin/LuCI 三文件/meta 两文件）；服务修复后 running（procd 托管）+ 渲染 /var/etc/falcon-daemon.json 正确 + RPC aria2.getVersion 应答；**真实小文件下载走通**——工作机 python http.server 供源，LuCI addUri → gid 0000000000000001 → /tmp/falcon-dl/sample-128k.bin 131072B 落盘，sha256 与源逐字节一致，tellStatus complete；LuCI 索引实证（清 /tmp/luci-indexcache* → curl 触发 dispatcher 重建 → 缓存含 falcon）+ 四卡数据源方法面全应答；升级链 = 重建 ipk（root:root 归属 + config_load 修复，二进制与 CI 字节一致 sha256 三方对拍）force-reinstall → opkg 报 "Not deleting modified conffile /etc/config/falcon" → UCI 修改（download_dir/enabled）存活 → restart running；装后文件属主 root:root（init 1001 残留为旧包一次性痕迹，已 chown 收口）。
- [x] EXT-FIX-002 [P1] 浏览器扩展下载接管误伤修复（2026-11-14 用户报「有些 url 没有下载的东西也弹下载界面」）：根因=interceptExtensions 默认空数组+文案「留空=全部接管」→默认接管浏览器一切下载。修复：①默认改安全白名单（zip/exe/mp4/pdf 等真实下载类型，支持 * 通配恢复全接管）②接管前大小下限（默认 1MB 可配）③嗅探降噪（content-type 为 text/html/json 时不按 URL 扩展名判媒体）④options 双语文案更新+升级兼容（interceptTouched 标记区分默认/用户清单）。验收：五项手测清单逐条实测落证据。
  - [x] ① 默认状态下载 zip → 接管：`shouldIntercept({url:'https://x/a.zip',fileSize:5MB}, defaults)==true`（含恰好 1MB 的 pdf 边界）
  - [x] ② 默认状态下载小图片 → 不接管：双重拦截——扩展名 png 不在白名单 `shouldIntercept==false`，且 `meetsSizeFloor(2048, 1048576)==false`；`.html`/大 `.txt` 同样 false
  - [x] ③ 配置 `*` → 全接管：`shouldIntercept({url:'https://x/i.png',fileSize:50MB}, {...defaults,interceptExtensions:['*']}==true`（旧行为可恢复）；`[' .PNG ','*']` 空白/点前缀/大写归一化后仍命中
  - [x] ④ 低阈值配置生效：`meetsSizeFloor(512,1024)==false`、`(1024,1024)==true`、`(1,0)==true`（0=不限）；未知尺寸 `undefined`/`-1` 视为满足（避免小但未报尺寸的下载被永久跳过）
  - [x] ⑤ 嗅探列表不再出现 content-type=text/html 误报：`classifyMediaUrl('https://x/video/play.mp4','text/html; charset=utf-8')==null`、`('…/data.mp4','application/json')==null`、`('…/a.mp3','text/plain')==null`；对照 `('…/a.mp4','video/mp4').kind=='video'`、无 ct 路径 `('…/a.mp4','').kind=='video'` 不变
  - 验证方式：无浏览器环境，用 vm 沙箱打桩后执行**真实 service_worker.js**（非复制逻辑）跑 27 项断言全过；`node --check` 四 JS 全过 + ESM 解析确认无 top-level await（MV3 红线）；i18n 双 locale 各 60 键集合/占位符一致，options.js 30 个 DOM id 与 19 个 i18n 键全部可解析；manifest 版本保持 0.2.0 未动
- [x] NIGHTLY-EXT-002 [P1] nightly 补浏览器扩展产物（Chrome/Edge MV3，2026-11-14 用户令）：nightly-build.yml 增 extension 打包步——apps/browser_extension 打 zip（manifest mv3 0.2.0：service_worker/_locales en+zh_CN/batch/options/popup 全量）资产 falcon-nightly-extension-chrome.zip + Edge 同包另名（Chromium 内核通用，双资产名覆盖两商店口径）+ SHA256SUMS；发布进 nightly release。验收：首跑产物可解压加载（chrome://developer mode load unpacked 口径说明进 README）。
  - 证据：nightly-build.yml create-nightly-release 增打包/结构校验/SHA256SUMS 三步（zip 根含 manifest 本机实证：20 entries 71677B，manifest 0.2.0/mv3/default_locale en + _locales en 与 zh_CN 各 60 键 + 7 必备文件逐一 test -f 过）+ release files 清单与下载说明表补双 zip/SHA256SUMS 行；README 双语 Quick Start 增浏览器扩展安装小节（解压 → chrome://extensions/edge://extensions 开发者模式 → Load unpacked 选解压目录；Edge 可直接用 chrome 包）；workflow_dispatch 首跑结果见 CLAUDE.md 变更记录。
- [x] EXT-FF-001 [P1] Firefox 扩展真适配 + 独立打包（2026-11-15 用户令「火狐单独的出插件打包」，同令 Chromium 收敛单包不回退）：①共享 API shim `api.js`（`var api = typeof browser !== "undefined" ? browser : chrome`，var 落全局对象保跨脚本可见）——45 处 `chrome.*` 调用点全改走 `api.*`（worker 23 + popup 15 + options 3 + batch 4），业务逻辑零改动；②manifest 变体构建期注入（`build_firefox.sh`：`background.scripts: [api.js, service_worker.js]` 替代 service_worker + `browser_specific_settings.gecko.id=falcon@cuihairu.github.io` + `strict_min_version 115.0` + del minimum_chrome_version）——Firefox 稳定版从未支持 MV3 service worker，event page 是其 MV3 background 形态；③webRequest `extraHeaders` Chromium 专属（Firefox 拒绝该值），两监听 spec 改引擎门禁三元（Firefox 暴露这些头无需该标志）；④基座 manifest 删 `"type": "module"`（死配置：全库零 ESM import，且 module worker 里 importScripts 抛异常）；⑤五 JS `node --check` 全过 + 本机双包构建/结构自检（chromium 8 必备文件 + firefox manifest 形态 jq 断言）+ `web-ext lint` 门禁；⑥README 双语 Firefox 安装小节（about:debugging 临时加载，重启失效）。
  - 证据：交付 commit 2ee6de6/6d4d78c；CI 验收（2026-11-18 复核）——nightly run 43665513328 五 job 全绿（Create Nightly Release 29s），release nightly 八资产 = 双扩展 zip + SHA256SUMS + 五桌面包（旧 chrome/edge zip 按名清扫）；firefox zip 下载核对：sha256 对 SHA256SUMS OK、manifest 落 zip 根、`background.scripts=api.js+service_worker.js`、gecko id falcon@cuihairu.github.io、strict_min_version 115.0、data_collection_permissions required=[none]、minimum_chrome_version 已删、api.js shim 在位、无构建脚本泄漏；chromium 包形态不变（service_worker 形态/minimum_chrome_version 保留）；CI Firefox 腿 web-ext lint 零 error 门禁（2ee6de6 run 43664988980 绿）。
- [ ] EXT-AMO-001 [P2] 扩展上架 addons.mozilla.org（AMO 正式版，2026-11-15 立项随 EXT-FF-001）：阻塞=需用户提供 Mozilla 账号（addons.mozilla.org 注册）+ AMO 审核（首次审核通常数天）。准备面已就绪：gecko.id 固定（falcon@cuihairu.github.io）+ strict_min_version 115.0 + web-ext lint 门禁；上架即解决 Firefox 临时加载重启失效问题（README 已注记）。验收：AMO 提交→审核通过→签名 xpi 可永久安装。
- [ ] SAFARI-001 [P3] Safari Web Extension 立项（2026-11-15 用户令「safari 先加入计划」）：转换链 `xcrun safari-web-extension-converter`（Web Extension → Xcode appextension 工程）+ Apple 开发者签名 + Xcode 构建链。约束如实记录：唯一可用 mac 环境是 GitHub runner macos（无私有 mac）；runner 上转换可行但**签名分发需开发者证书**（无证书只能本机临时跑），先探 runner 可行性（converter 是否预装/Xcode 版本/无证书产物形态）再排期实施。api.js 适配层同样适用（Safari 14+ 支持 Web Extension API）。验收：runner 转换冒烟报告 + 签名可行性结论。
