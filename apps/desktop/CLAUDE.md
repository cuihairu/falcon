# Desktop 应用开发指南

## 变更记录 (Changelog)

### 2026-09-18 - 导航信号重构 + 空壳入口删除 + 统一品牌视觉打磨（qt-ui-design 截图驱动）
- **三 tab 同界面根因**（不是过滤缺失）：每个 tab 双 connect——具体信号
  （downloadingTabClicked/completedTabClicked）先发、downloadClicked 后发，
  而 main_window 的 downloadClicked lambda 无条件 set_view_mode(Downloading)
  且连接顺序在前 → 覆盖具体 tab 刚设的模式；过滤谓词 should_show 本身完好。
  重构：删 downloadClicked 信号，三 tab 各只连自己的信号，各 lambda 自带
  setCurrentIndex + set_view_mode
- **空壳入口删除**（用户确认）：「云添加」（按 URL 域名筛任务的过滤视图，
  零后端）与「私人空间」整组（只发 downloadClicked 无任何隐私隔离）整体
  删除；download_page 删 CloudAdd 枚举与 cloud_domains() 域名表
- **循环切换按钮 → 页头分段切换器**：view_toggle_button_（循环切换）删除，
  hero 区新增 #viewSegmented 胶囊（两 checkable QPushButton + exclusive
  QButtonGroup）；set_view_mode 内 setChecked 单点同步（setChecked 不触发
  clicked，无递归），与侧栏 tab 双向同步
- **qt-ui-design 视觉审计落地**（离屏沙盒 12 张截图逐张验收，亮暗 × 6 视图）：
  ① 字号刻度收口 4 档（12 caption / 14 body / 16 title / 20 page·hero），
  删 11px（进度条内文字容不下）与 24px，数字展示位 summaryValue 归 20——
  进度百分比改外置 #progressPctLabel（6px 条高压不下 11px 文字且 chunk 上
  不可读）；② QGroupBox 标题骑线修复——title 加卡片同色 background 盖住
  边框线（fieldset legend 形态）；③ navTab 选中态强化——全部 navTab 加
  border-left: 3px solid transparent 占位防文字跳，checked 换 accent 条 +
  accent 文字；④ 页面头统一 hero 结构（下载/发现/设置/云盘/AddDialog 五处
  一致），英文眉题全删（DOWNLOAD CENTER/PREFERENCES/DISCOVERY/NEW TASK），
  云页 pageTitle 迁移为 downloadHero；⑤ 两 QSS 死选择器清零
  （heroEyebrow/pageTitle/headerLabel），105 选择器亮暗严格成对（脚本校验）
- **功能诚实化三连**：① 已完成/已取消任务行的暂停按钮隐藏（原为显示禁用
  占位 ⏸，两视图一致——终态无暂停语义）；② 发现页 "Type" 表头与详情
  Title/Size/Source/Type/Date/URL 全量中文化（前轮 MISS），表头统一左对齐
  （原 stretch 列标题居中悬空观感）；③ 新建下载对话框整体中文化
  （Add Download Task/Save/File name/Advanced/Cancel/Start 等，User Agent/
  Referer/Cookies 技术术语保留）
- **修复 update_action_buttons 死函数**：图标化重构后行按钮变 QToolButton，
  函数内 qobject_cast<QPushButton*> 恒失败 → 整函数 no-op（假更新的死代码，
  正是"功能是费的"同类）；整体删除（含 hpp 声明与两处调用）
- **修复 falcon_desktop_backend_tests 从未编译**：tests/CMakeLists 的
  include 路径少 src/（`services/download_backend.hpp` 恒找不到），而 CI
  组合性缺口使目标从未被暴露——全部 `FALCON_BUILD_TESTS=ON` job 均
  `FALCON_BUILD_DESKTOP=OFF`、desktop job 均 tests OFF；补
  `${CMAKE_CURRENT_SOURCE_DIR}/../src` 后 6 用例首次真实编译执行（6/6 通过，
  本机双前缀 CMAKE_PREFIX_PATH = 官方 Qt + build-ci vcpkg_installed 供 GTest）
- 设置/云盘/发现页全量中文化（~120 处 tr 字符串）；本机验收以
  falcon-ui-sandbox 截图为准（/tmp/ui_after 12 张），手工项：tab 点击真切换、
  分段器与侧栏双向同步、终态行无暂停钮

### 2026-09-18 - Windows 启动先弹终端修复（WIN32_EXECUTABLE 变量名笔误）
- **现象**:Windows 下启动 falcon-desktop 先弹一个控制台终端再出 GUI
  (nightly PE 头 Subsystem=3 WINDOWS_CONSOLE 实证;正常 GUI 应用应为
  2 WINDOWS_GUI 无控制台)
- **根因**:CMakeLists 顶部 `set(WIN32_EXECUTABLE TRUE)` 变量名笔误——
  CMake 读取的变量是 **CMAKE_WIN32_EXECUTABLE**,普通变量
  WIN32_EXECUTABLE(与 target 属性同名)从不被读取 → add_executable
  未带 WIN32 标志,exe 一直以 CONSOLE 子系统链接(自项目创建以来从未
  生效过);MSVC 不报错、不告警,与资源缺陷同类——编译绿掩盖
- **修复**:add_executable 之后 `set_target_properties(falcon-desktop
  PROPERTIES WIN32_EXECUTABLE TRUE)`;WIN32_EXECUTABLE 属性置位后
  Qt6::Core 自动挂接 Qt6::QtMain 完成 main→WinMain 入口转发(无需手动
  链接)。沙盒 falcon-ui-sandbox 是开发工具,保留控制台打印截图路径,
  不置此属性。验证:PE 头 Subsystem 须为 2(验证脚本见上一条目方法)

### 2026-09-18 - Nightly Windows 包资源编入实证（发布包二进制验证方法）
- **验证方法**（无 Windows 环境对发布包做资源闭环验收）：下载 nightly
  Windows zip → 解包 → Python 直接分析 falcon-desktop.exe。三个陷阱
  逐一踩过，下轮直接用正确姿势：
  ① **UTF-16BE 资源名搜索会被 UTF-16LE 代码字面量错位误报**——.rdata
  里 load_qss 的 ":/styles/..." 与 icon_utils 的 ":/icons/..." 字符串
  交错字节恰好构成 BE 序列（此前命中位置全是误报）；真名字数组须搜
  `[2B len][4B hash][BE 名字]` 条目结构（fluent_light.qss = `00 10
  05 47 53 a3`），Windows 包实证名字数组完整（icons/styles/两 QSS/
  27 图标名）在 0xdd3aa，紧跟最后一个数据 entry（数组布局 data→name）
  ② **资源体压缩形态随平台 rcc 而异**：本机 Linux rcc 用 ZSTD
  （28 b5 2f fd，qrc_resources.cpp 直接可见）、nightly Windows rcc 用
  zlib level 9——只搜一种压缩头必然漏
  ③ **zlib 头随压缩级别变化**：78 01/5e/9c/da 四种全扫并逐流试解压
- **决定性判据**：zlib 78 da 流解压出两份 QSS，字节数与源文件逐一
  精确相等（fluent_light 12358 / fluent_dark 12569）；27 个 SVG 以
  `[4B BE 长度头][明文]` 标准 rcc entry 编入（upload.svg 头 = `00 00
  01 a7`）。包布局核对：qt.conf（`Plugins = plugins`）+ qwindows.dll
  + Qt6Svg.dll + imageformats/qsvg.dll + CRT 全齐
- nightly run 35291193607 三平台打包全绿，66b8144 资源修复在生产包
  闭环确认；对照教训：编译绿 ≠ 资源在（编译不查资源正是上次缺陷
  逃逸的根因），发布包二进制验证才是终点证据

### 2026-09-17 - 资源链路致命缺陷修复 + 网格视图两缺陷 + 离屏截图沙盒
- **资源从未编入二进制**(离屏截图首跑曝光的三重缺陷,上一轮 UI 重做的
  遗留):① `qt_add_resources(falcon-desktop ...)` 在 add_executable 之前
  调用(无效调用)且 resources.qrc 从未进 target sources → AUTORCC 不
  触发,**任何平台的二进制里都没有资源**——CI 三平台绿掩盖(编译不查
  资源),运行时 `:/styles/*.qss` 与 `:/icons/*.svg` 读取全部落空仅
  WARNING,Fluent 样式与图标实际从未生效;② resources.qrc 的 prefix 与
  file 相对路径叠加成双层前缀(`:/styles/resources/styles/x.qss`),与
  代码读取路径 `:/styles/x.qss` 不一致——即便编入也读不到;③ qrc 引用
  构建产物 .qm 造成构建顺序死锁(无 LinguistTools 的环境
  "No rule to make target .qm")
- **修复**:删无效调用,resources.qrc 进两个 target 的 sources 交
  AUTORCC;全部 file 加 `alias=basename` 收口资源内路径;qrc 删 /i18n 节
  (main.cpp:46 对 qrc 加载失败本有 exe 旁回落,翻译保持软依赖)。
  验证手段:`rcc --list` 只列磁盘路径,资源树形态须写探针程序
  `QDir(":/")` 递归列出——UTF-16BE 名字数组 strings 不可见
- **网格视图两处真实缺陷**(沙盒截图曝光):主布局 task_table_/
  grid_container_ 无 stretch + 尾部 addStretch 吃光剩余空间 →
  QScrollArea 初始 sizeHint 近 0,网格视口被压扁、卡片只露顶部
  (表格靠自身 sizeHint 侥幸可用);改两视图 stretch=1 互斥占满,
  删尾部 stretch。sync_task_grid 按 QHash 迭代无序 → 每次切换卡片
  顺序抖动;按 task id 排序(与表格视图一致)
- **离屏截图沙盒**(`FALCON_BUILD_UI_SANDBOX`,默认 OFF):
  dev/ui_sandbox.cpp 按 MainWindow 布局组装真实组件(TopBar/SideBar/
  四页面/StatusBar)注入演示任务,`QT_QPA_PLATFORM=offscreen` 下亮暗
  两主题 × 5 视图 = 10 张 png;切页经侧栏按钮真实 click(信号 +
  QButtonGroup 选中态同步),不用真 MainWindow(引擎/网络/托盘零
  副作用);snap 前 processEvents + sendPostedEvents(LayoutRequest)
  保证布局收敛。供无显示环境设计验收/视觉回归,可挂 CI 出快照

### 2026-09-17 - UI 结构性重做（Fluent 体系 + Lucide 图标 + 无边框窗口）
- **样式收口**：删除 theme_manager.cpp 978 行内联 QSS、styles.hpp（死代码）、
  main.qss（编入 qrc 从未加载）三套矛盾样式；新增 fluent_light.qss /
  fluent_dark.qss **严格成对编写**（改一份必须同步另一份，11 节同构）+
  theme_tokens.hpp 语义色 token（QPalette 与 QSS 同源）+ icon_utils
  （TokenIconEngine 绘制时取当前主题色，换肤自动换图标颜色）；QSS 选择器
  只认代码真实 objectName（双向脚本校验）
- **图标系统**：resources/icons/ 27 个 Lucide SVG（stroke="currentColor"），
  `icons::themed(Id, ColorRole)` 返回主题感知 QIcon；Qt6::Svg 为 REQUIRED
  依赖（vcpkg qtsvg / apt qt6-svg-dev / nightly EXTRA_QT_MODULES 三处同步）
- **无边框窗口**：TopBar mousePressEvent→startSystemMove、双击→最大化；
  MainWindow qApp 级 eventFilter + resize_edge_for（6px 边缘带 8 向缩放，
  必须挂 qApp——中央控件吞事件）；changeEvent 同步 TopBar set_maximized
  （Square↔Restore 图标）
- **交互修复**：TopBar 搜索→DownloadPage::set_text_filter（should_show
  单点过滤）、视图切换→toggle_display_style（DownloadService::
  request_refresh 从 private 提升 public 供刷新钮用）；SideBar 三组
  QButtonGroup 合并单一 exclusive nav_group_、set_queue_count 吃真实
  stats；StatusBar 死按钮全删
- **教训**：src/widgets/ 下的文件 include src/utils/ 头必须用
  "../utils/xxx.hpp"（相对 include 只找同目录）；QIconEngine/QWindow/QStyle
  使用前必须完整 include（QIcon 前向声明不够，override 全部失效）
- 三平台 Qt6 CI 全绿（run 35247334598）

### 2026-09-12 - Daemon 事件流驱动（WebSocket 通知接入）
- `DaemonRpcBackend` 从 HTTP `JsonRpcClient` 切换到 `WebSocketRpcClient`
  （daemon 新增的 WS JSON-RPC 客户端）：与 daemon 维持单条 WebSocket
  长连接，全部 RPC（控制/查询）与服务器通知共用；断线后 `call()` 自动
  重连握手
- `IDownloadBackend` 新增可选能力 `set_wake_callback`（默认无操作）：
  daemon 通知（aria2.onDownloadStart/Pause/Complete/Error/Stop +
  falcon.onProgress）到达即触发回调，传空解除注册（返回后保证无在途
  调用，析构时序安全）
- `DownloadService` 刷新路径升级为事件驱动：`start()` 注册唤醒回调 →
  `request_refresh()` 置位 + 唤醒 worker；worker 在 fetch 期间到达的
  重复事件自动合并（bool 标志），500ms 周期轮询降为兜底（后端不可达
  重连、无事件源的进程内后端仍走周期驱动）
- 析构顺序加固：`~DownloadService` 先 `stop()`（worker join）再解除
  唤醒回调；`~DaemonRpcBackend` 先 `client_.disconnect()`（join WS 读
  线程）再让成员析构——否则成员逆序析构会先销毁 `wake_callback_` 而
  读线程可能正在调用它
- 修复 `falcon_desktop_backend_tests` 潜在链接缺陷：测试 harness 使用
  `JsonRpcServer` 但目标只链接了 `falcon_daemon_rpc_client`（不含
  server 符号）——补链 `falcon_daemon_rpc`（静态库按需拉入成员，与
  client 库的同源文件无重复定义冲突）
- 新增用例 `DaemonWakeCallbackOnNotification`（addUri → daemon 通知 →
  wake 回调全链路）；本地等价目标编译验证 6/6 通过

### 2026-09-11 - 下载服务层与后端抽象（Daemon RPC 集成）
- 新增 `services/download_backend.{hpp,cpp}`（纯 C++，不依赖 Qt）：
  `IDownloadBackend` 接口（add/pause/resume/remove/set_priority/
  apply_global_settings/fetch_tasks/fetch_stats）+ 两个实现
  - `InProcessBackend`：进程内直接持 `DownloadEngine`（旧行为）
  - `DaemonRpcBackend`：经 aria2 兼容 JSON-RPC 连接 falcon-daemon
    （`falcon_daemon_rpc_client` 库；addUri 时把输出目录/文件名/连接数/
    UA/Referer/限速/Cookie 映射为 aria2 选项；gid ↔ TaskId 按 16 位 hex 转换）
- 新增 `services/download_service.{hpp,cpp}`（QObject）：后台 worker 线程
  按 500ms 周期轮询 `fetch_tasks`/`fetch_stats`，以快照（`TaskSnapshot`）
  经 `tasks_refreshed`/`stats_refreshed` 信号推给 UI；命令队列
  （mutex+condvar）串行化 add/pause 等操作；完成/失败事件由前后两轮快照
  diff 得出（仅当亲眼见过未完成状态才通知，首轮为基线）——两个后端统一路径
- `main_window` 改造：移除 `IEventListener` 直连引擎的旧路径，改为持
  `DownloadService`；启动时按设置页 daemon 开关创建后端（进程内 vs RPC，
  切换需重启）；`load/save_settings` 持久化 daemon 模式/RPC URL/secret
- `download_page` 重写为快照驱动：`update_tasks(vector<TaskSnapshot>)`
  全量重建行数据（删消失行、重排行号）；新增行内暂停/继续按钮与右键
  优先级子菜单（Low/Normal/High/Critical → `set_priority`）
- `settings_page` 新增 Connection 区 Daemon mode 开关 + RPC URL + secret
- 新增 `apps/desktop/tests/`：`falcon_desktop_backend_tests`（纯 C++，
  DaemonRpcBackend × 真实 JsonRpcServer 回环 5 用例），不依赖 Qt
- CMake：desktop 硬依赖 `falcon_daemon_rpc_client`（需 nlohmann_json + CURL，
  保持 `FALCON_BUILD_DAEMON=ON`）

## 概述

Falcon Desktop 是基于 Qt6 的跨平台桌面下载管理器，采用迅雷风格的 UI 设计。

## 技术栈

- **Qt 6.2+**: UI 框架
- **C++17**: 编程语言
- **CMake**: 构建系统
- **libfalcon**: 核心下载库
- **falcon_daemon_rpc_client**: daemon RPC 客户端与 aria2 快照转换（可选 daemon 模式）

## 架构设计

```
┌─────────────────────────────────────────────────────┐
│                    MainWindow                       │
├──────────┬──────────────────────────────────────────┤
│ TopBar   │                                          │
├──┬───────┴──────────────────────────────────────────┤
│  │      ┌──────────────┐    ┌───────────────────┐   │
│  │      │              │    │                   │   │
│  │      │  SideBar     │    │   ContentStack    │   │
│  │      │              │    │                   │   │
│  │      │ - 下载中     │    │ - DownloadPage    │   │
│  │      │ - 已完成     │    │ - CloudPage      │   │
│  │      │ - 云盘       │    │ - SettingsPage   │   │
│  │      │ - 发现       │    │                   │   │
│  │      │ - 设置       │    │                   │   │
│  │      └──────────────┘    └───────────────────┘   │
│  │                                              ┌───┴───┐
│  │                                              │StatusBar│
├──┴──────────────────────────────────────────────┴──────┤
│              System Tray (QSystemTrayIcon)              │
└─────────────────────────────────────────────────────────┘
```

## 组件说明

### MainWindow

主窗口类，负责：
- 管理所有页面和组件
- 处理窗口关闭（最小化到托盘）
- 持有 DownloadService（见下"下载服务层"），连接其信号
- 连接信号/槽

### SideBar

侧边导航栏，提供：
- 下载中/已完成切换 + 云盘/发现/设置导航

### DownloadPage

下载管理页面，支持：
- 表格视图（传统列表）
- 网格视图（卡片布局）
- 任务过滤（下载中/已完成，页头分段切换器与侧栏双向同步）
- 任务操作（暂停/继续/删除）

### CloudPage

云盘管理页面：
- S3/OSS/COS 等对象存储连接
- 远程资源浏览
- 文件上传/下载

### DiscoveryPage

资源发现页面：
- 多资源类型搜索（磁力/HTTP/网盘/FTP）
- 搜索结果展示
- 一键下载

### SettingsPage

设置页面：
- 剪切板监听配置
- 下载设置（并发数、保存目录）
- 连接设置（超时、重试）
- Daemon 模式（开关 + RPC URL + secret；重启后生效）
- 通知设置
- 主题切换

## 服务层

### DownloadService + IDownloadBackend（下载服务层）

所有下载操作与状态获取都经这一层，UI 不直接接触引擎或 RPC：

```
┌─────────────┐  信号(跨线程排队)   ┌──────────────┐
│ MainWindow  │ ◄───────────────── │ DownloadService│
│ DownloadPage│  TaskSnapshot 快照  │  (worker 线程) │
└─────────────┘                    └───────┬───────┘
                                           │ 命令队列 + 500ms 轮询
                              ┌────────────┴────────────┐
                              │    IDownloadBackend      │
                              ├──────────┬──────────────┤
                              │ InProcess│  DaemonRpc   │
                              │ Backend  │  Backend     │
                              │ (引擎直连)│ (JSON-RPC)   │
                              └──────────┴──────────────┘
```

- **线程模型**：QObject 本体在主线程；内部 `std::thread` worker 排空命令
  队列后 fetch 快照并 emit 信号（跨线程自动 QueuedConnection；自定义类型
  需 `Q_DECLARE_METATYPE` + `qRegisterMetaType`）
- **刷新驱动**：daemon RPC 后端经 WebSocket 事件流收到通知即回调
  `set_wake_callback` 触发立即刷新（事件驱动）；周期轮询（500ms）仅作
  兜底（重连、无事件源的进程内后端）。worker 忙碌期间到达的重复事件
  自动合并为一次刷新
- **事件派生**：完成/失败不靠回调，由前后两轮快照 diff 得出（仅当亲眼见过
  未完成状态才通知），两个后端行为完全一致
- **后端选择**：应用启动时按设置页 daemon 开关创建，运行中不可切换
- **纯 C++ 边界**：`download_backend.{hpp,cpp}` 不含 Qt，可独立于 Qt
  编译测试（`apps/desktop/tests/`）

### SearchService

搜索服务，支持：
- 磁力链接搜索
- HTTP 资源搜索
- 网盘资源搜索
- 后台线程搜索

### StorageService

存储服务桥接层：
- 连接/断开云存储
- 列出远程文件
- 下载远程文件

### ThemeManager

主题管理器：
- 亮色/暗色主题
- 完整 QSS 样式表
- 运行时切换
- 配置持久化

## 工具类

### UrlDetector

URL 检测器，支持：
- 标准协议（HTTP/HTTPS/FTP）
- 磁力链接（Magnet）
- 私有协议（Thunder/Flashget/ED2K）
- 云盘链接（百度/阿里云/夸克/天翼/蓝奏云）

### ClipboardMonitor

剪切板监听器：
- 自动检测下载链接
- 可配置检测延迟
- 可启用/禁用

## 样式指南

### QSS 样式

使用 QSS 实现主题样式：

```cpp
// 亮色主题
QWidget {
    background-color: #ffffff;
    color: #333333;
}

QPushButton#primaryButton {
    background-color: #0078d4;
    color: #ffffff;
}

// 暗色主题
QWidget {
    background-color: #1e1e1e;
    color: #e0e0e0;
}
```

### 对象命名

为特定 QSS 样式设置 objectName：

```cpp
button->setObjectName("primaryButton");
table->setObjectName("taskTable");
card->setObjectName("taskCard");
```

## 信号/槽连接

### 跨页面通信

使用信号/槽机制：

```cpp
// DiscoveryPage 发出下载请求
connect(discovery_page, &DiscoveryPage::direct_download_requested,
        this, &MainWindow::on_direct_download_requested);

// MainWindow 处理下载
void MainWindow::on_direct_download_requested(const QString& url, bool start) {
    add_download_task(url, start);
}
```

## 编译与运行

### 依赖

```bash
# Ubuntu/Debian
sudo apt install qt6-base-dev qt6-tools-dev

# macOS
brew install qt@6

# Windows
# 从 qt.io 下载 Qt6 installer
```

### 构建命令

```bash
# 配置
cmake -B build -S . \
  -DFALCON_BUILD_DESKTOP=ON \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.x.x/gcc_64

# 编译
cmake --build build --target falcon-desktop

# 运行
./build/bin/falcon-desktop
```

## 开发规范

### 文件命名

- 页面: `xxx_page.{hpp,cpp}`
- 组件: `xxx.{hpp,cpp}`
- 对话框: `xxx_dialog.{hpp,cpp}`
- 服务: `xxx_service.{hpp,cpp}`

### 类命名

- 页面: `XxxPage`
- 组件: `XxxBar`/`XxxWidget`
- 对话框: `XxxDialog`
- 服务: `XxxService`

### 成员变量命名

使用下划线后缀：

```cpp
class DownloadPage : public QWidget {
private:
    QTableWidget* task_table_;
    QPushButton* new_task_button_;
    DownloadViewMode view_mode_;
};
```

## 未来计划

- [ ] 任务拖拽排序
- [ ] 下载队列管理
- [ ] 下载速度限制
- [ ] 计划任务（定时下载）
- [ ] 下载完成后操作（打开文件、关机等）
- [ ] 多语言支持（i18n）
