# Desktop 应用开发指南

## 变更记录 (Changelog)

### 2026-10-07 - B17 资源搜索配置界面化（表单对话框 + 配置模板 + 即时生效 + 走查 26 图）+ 附带修复 B26/B27 两 drives 缺陷
- **四腿落地**：
  - ① **表单对话框**（新 `search_engine_dialog.{hpp,cpp}`）：名称/站点地址/搜索路径/返回格式（html\|json）/请求间隔 ms/启用 + 查询参数多行「键=值」（值留空的 q/search/keyword 由 drives 侧自动填关键词）+ 5 个 HTML 正则选择器行（item/title/url/size/seeds），全字段 placeholder + tooltip；OK 前内联校验（名称必填/重名/http(s):// 前缀/参数行格式/item 选择器 html 必填/正则 ECMAScript 预检——与 drives 同方言，错误在表单期暴露不等搜索失败），失败红字错误标签不关框
  - ② **engines.json 模板**：首次生成内置 `_usage`（how_to 指引 + 逐字段说明）+ `global_settings` + 全 disabled 的 `example` 条目（html 形态可复制改）+ version；UI 保存经「读盘 → 改数组 → 写盘」保留 `_usage`/`version`/未知键
  - ③ **即时生效**：设置页增删改/启用开关即时写盘；`SearchService::run_search` 每次搜索重载目录 + `ResourceSearchManager::load_config`（手改 engines.json 同样免重启）
  - ④ **真实走查**（Xvfb :96 + 本地 JSON 搜索服务器 127.0.0.1:18097，26 图 `/home/cui/fb-shots/b17/04-26`）：首启模板生成 04 → 表单校验错误示例 06 → 保存后设置页列表 08/09 → 发现页搜索命中且「来源」列显引擎名 15/18 → 编辑改名即时生效 19-23 → 删除后设置页空 + 发现页提示「未启用任何搜索引擎…」（24-26）
- **验证**：build-desktop 增量零告警 + desktop 全部 8 二进制 112/112 + drives 168→169
- **附带 B26（drives，同批修复）**：`is_available` 原为正文嗅探 `find("404")` 子串——根页提到 404 字样的正常站点被误杀、真实 404 的 API 站点反放行，且 `search_all` 静默跳过零日志；改 `WebCrawler::get` 记录 `CURLINFO_RESPONSE_CODE`（新增 `last_status()`；顺带修 get() 入口不清 response_ 的残留 prepend 隐患），按状态码判定（0=unreachable WARN / ≥400=WARN+跳过 / 2xx-3xx 可达），不可用判定全部带引擎名 WARN
- **附带 B27（drives，同批修复）**：`parse_json_response` 三形态三份复制的裸 `get<T>()` 共享一个 try/catch——单字段类型错误（size 给成字符串）丢弃该引擎全部结果；改宽容提取 `json_str`/`json_int`/`json_size`（数字字符串经 stoi、"1.5 GB" 经 `detail::parse_size`、浮点守卫）+ 共享 `extract_json_item`：单字段错误只损失该字段；magnet 仅非空才覆盖 url。TypeErrorEngine 断言按新语义翻转 + 新增 /strsize 用例（净增 1）

### 2026-10-07 - B16 批量操作菜单按 tab 区分（下载中/已完成双菜单 + 逐项真实走查 17 图）
- **根因**：批量操作菜单未按视图区分——两个 tab 弹同一个菜单（含对当前 tab 无意义的项，如已完成的「全部暂停」、下载中的「清空完成记录」）
- **修法**（`download_page.cpp` `on_more_options_clicked`）：菜单在每次点击时现建，按 `view_mode_` 分支——**切 tab 菜单实时换**（同一按钮两 tab 菜单不同，走查实证）
  - 「下载中」：全部开始（Paused/Failed/Pending → `resume_requested`）/ 全部暂停（Downloading/Preparing → `pause_requested`）/ 全部取消（Pending/Preparing/Downloading/Paused 四态 → `remove_task_requested` 进回收站；**不动 Failed**——失败任务留「继续」重试入口；**已完成记录不受影响**）/ 删除任务（选中行；无选中禁用）/ 清空列表（全部下载中视图任务移入回收站）
  - 「已完成」：清空完成记录（`remove_finished_tasks_requested`，只删记录不动文件）/ 删除选中（无选中禁用）/ 重新下载（按原 URL 新建任务并立即开始 → 新信号 `redownload_requested(url)` → MainWindow 走与「新建任务」同一条 `add_download_task` 路径；无选中禁用）/ 打开所在文件夹（`QDesktopServices::openUrl`，无选中禁用）——**无开始/暂停类**
  - **选择语义**：`selected_record()` 仅表格视图有效 → 选择依赖项（删除任务/删除选中/重新下载/打开所在文件夹）在菜单 exec 前**值捕获**选中 id/url/dir，无选中置 disabled
- **取消/删除的产品语义（走查实证 ground truth）**：取消任务 → 回收站条目 `file_in_trash: false` + 部分文件 `<name>.bin.falcon.tmp` 留 Downloads（断点）；删除已完成任务 → 文件移入 `.falcon-trash/<id>_<name>` + `file_in_trash: true`；清空完成记录只删记录
- **真实走查**（Xvfb :96 + 64KB/s 限速 ThreadingTCPServer 真实下载，17 图 `/home/cui/fb-shots/b16/`）：下载中菜单 m1 / 已完成菜单 m2（无开始暂停类+无选中灰禁）/ 选中启用 m3 / 全部暂停 s6 / 全部开始 s7 / 打开所在文件夹 s8（xdg-open.log 铁证）/ 重新下载 s9（活动 0→1 + 悬浮窗出现 + 已完成 7→8）/ 删除选中 s10（8→7）/ 清空完成记录 s11（7→0）/ 全部取消 s13c→s14b（双任务下载中→0，误完成的 slow8/9 记录原样保留=「不动已完成」范围正确性铁证）/ 回收站 s15（3 项状态正确）/ 删除任务 s16a→s16（选中 slow10 删、slow11 保留下载中）/ 清空列表 s17（下载中→空态、已完成 2 不受影响）
- **测量备注**：走查用 16KB/250ms 限速（≈64KB/s）把 400MB 窗口拉到 ~100min——此前 4.2MB/s 档 97s 窗口在多轮 turn 间耗尽（全部取消点击时任务已自行完成，无物可取消）；多步 UI 序列（对话框添加+菜单点击+截图）必须链进单条 Bash 命令

### 2026-10-07 - B15 视图记忆真实走查闭环（实现为 e679035/470d0ef，本批补走查证据 + 登记回填）
- **实现已在库**（2026-10-04）：首次/无记录默认卡片（load_settings 缺省 grid）+ 顶栏切换即时落 QSettings（display_style_changed → sync）+ 设置页「任务列表视图」下拉框双向同步（QSignalBlocker 防回环、恢复默认回发即时生效）
- **本批真实走查（Xvfb :96 + openbox，7 图 /home/cui/fb-shots/b15-s1~s7）**：全新配置首启 = 卡片（钮显「列表视图」+ conf 无记录）→ 切列表 → conf 即时 `task_display_style=table`（QSettings 实落 FalconTeam/Falcon.conf——注意 desktop.conf 是另一文件，QSettings 组织/应用名落在 FalconTeam/Falcon）→ 重启记住列表（零点击）→ 设置页同步 +「卡片视图（默认）」标注 + 说明文字 → 设置页切回 → conf=grid → 重启卡片恢复
- **测量备注**：QSettings 落点按组织/应用名（FalconTeam/Falcon.conf）而非 `.config/falcon/desktop.conf`——desktop.conf 为应用自管配置（engines.json 同目录），走查断言 conf 状态时认对文件

### 2026-10-07 - B24 剪贴板「对话框关闭后失效」定性收口（干净环境不复现）+ B25 兜底定时器不触发登记 + 嵌套事件循环回归钉子
- **B24 定性（用户症状不复现，环境因由）**：Request B 置顶缺陷「第一个添加对话框关闭后剪贴板检测整体失效」——干净环境（Xvfb :95 + openbox 重建）逐字复现剧本：Escape 关闭对话框 → 复制新的不同链接 → 4s 内对话框重开（PB1-reopen-check.png），QClipboard::changed 主信号路径关闭后存活。用户观感解释 = 走查环境污染：卡死的 XTEST Escape 注入 + autorepeat rate 25（≈25Hz Escape 重放）→ QDialog::reject 循环 → 弹出即关（256ms 内消失）=「不弹窗」假象（干净环境对话框稳定打开 ≥35s）。/tmp 时间线证据 2026-10-06 停电丢失，定性以存活截图为准（如实记录）
- **B25 附带发现（登记待查）**：兜底轮询 QTimer 从不触发——check_clipboard 断点 12-15s 空闲窗 0 命中 ×2 + strace ppoll timeout=NULL，而 start() 确定执行（源码在位 + is_monitoring_ 实读）；主信号路径不受影响。已排除六类假设，未排除错误线程 start()（Qt 只告警不启动）；QTimer::timerEvent 断点在 Qt 6.8+ 无意义（QSingleShotTimer 重构）、发行版 Qt 无调试符号（`p *check_timer_` incomplete type）——续查需复建环境 + gdb 断 start
- **回归钉子**：`NestedEventLoopInHandlerDoesNotKillSubsequentDetection`（clipboard 11 → 12）——url_detected handler 内转 QEventLoop::exec（与生产 QDialog::exec 嵌套循环同构）后，第二个不同 URL 仍必须被检测到；钉住「对话框消费不杀监控」不变式（B24 场景的产品行为契约）
- **验证**：clipboard 12/12 + desktop 全部 8 二进制 112 用例回归绿（backend 26 / order 14 / url 8 / ipc 23 / storage 16 / update 7 / visibility 6）

### 2026-10-04 - B23 复选框「隐形」修复（QSS data-URI 不支持 → qrc SVG url 引用 + 亮暗双主题像素级复验）
- **根因**：双 QSS `QCheckBox::indicator` 的 `url(data:image/svg+xml;base64,…)` 形态——**Qt QSS 不支持 data: URI**，图标加载失败零渲染，勾选/未勾选两态全部不可见（2026-09-28 warm console 批次「checkbox 内嵌 base64 SVG」引入，沙盒目测验收漏检；功能点击不受影响故用户感知为「看不见开关状态」）
- **修法**：4 个 16×16 SVG（checked = accent 实底 rx=3 圆角方块 + 对比色勾；unchecked = 中性描边空心框）入 `resources/icons/checkbox-{checked,unchecked}-{light,dark}.svg` + `resources.qrc` alias；双 QSS 指示器改 `image: url(:/icons/…)`。色值：light checked 底 #c2410c + 白勾 / unchecked stroke #a29a92（= disabled token）；dark checked 底 #ffa07a + #27140a 勾 / unchecked stroke #6e665e
- **像素级复验（Xvfb :93 真实应用，非沙盒）**：亮色 checked 7 处各 16×16 px=196 @ #c2410c + unchecked 环 #a29a92 x32 精确色；toggle 往返（取消后与基线 0 diff）；QSettings 持久化跨重启（sound/daemon=true → 重启 ☑）；暗色 checked 4 处 @ #ffa07a + disabled ring #6e665e x32 + light accent 零残留 + theme=dark 重启保持
- **归档刷新**：ui-sandbox 52 张重新生成（旧截图 checkbox 零渲染形态过时）
- **如实披露**：unchecked ring 对比度 light ≈3.0:1 / dark ≈2.5:1（与既有 disabled token 同源，未另开新 token）；disabled checkbox 无独立 :disabled 指示器图像（复用 unchecked SVG，色彩恰同为灰阶语义）

### 2026-09-30 - 设置项接入下载选项（connection_timeout/retry_count 保存却不消费的收口）
- `show_add_download_dialog`/`add_download_task` 两站点把设置页
  连接超时（QSettings `connection_timeout_seconds` 默认 30）与重试
  次数（`retry_count` 默认 3）写入 DownloadOptions——GUI 路径此前恒
  用引擎默认，用户设置零生效；save 侧既有，load 侧补
  `set_connection_timeout`/`set_retry_count`（QSpinBox 纯转发）
- 超时语义随引擎 P0 修正为停滞看门狗（见根 CLAUDE.md 同日条目）；
  本接线仅编译验证 + build-desktop 110/110（无 settings_page GUI
  自动化测试）

### 2026-09-30 - QApplication 级测试基建首发 + clipboard_monitor/storage_service 首批单测（desktop 78 → 104 用例）
- **基建（QApplication 级首例）**：clipboard 测试带 QApplication + 平台选择
  main——环境已给平台（xvfb-run 的 xcb / 桌面会话）优先，无显示环境回落
  `QT_QPA_PLATFORM=offscreen`；**QClipboard 在两平台均可用**（探针实证：
  setText/文本往返 + changed 信号 offscreen 下同样触发），验证口径 =
  xvfb-run（xcb）与 offscreen 双跑（本机补装 libxcb-cursor0/icccm/image/
  keysyms/render-util0 五个 xcb 卫星包后 xcb 平台插件可加载）。此后所有
  需要平台层（剪贴板/窗口系统）的桌面单测沿用此 main 模板
- **新 target `falcon_desktop_clipboard_tests`（10 用例）**：检测语义双路径
  钉住——生产主路径 QClipboard::changed 直连 check_clipboard（setText 同步
  触发）、轮询 QTimer 是兜底；**去重与重启重检用 40ms 小间隔定时器路径
  独立验证**（不依赖「同值 setText 是否再发 changed」的平台差异语义）；
  null clipboard 构造/启停安全（守卫路径真实触发）；URL 检测（last_url
  字段 is_valid/protocol/decoded_url/file_name 全断言）；非 URL 纯文本
  忽略（**contains_url 对嵌入 URL 的文本会命中，必须用纯非 URL 文本**）；
  stop 后剪贴板变化不再派发。**每用例唯一 URL**（序号后缀）排除剪贴板
  单例跨用例残留对 dedup 观察的干扰
- **新 target `falcon_desktop_storage_tests`（16 用例）**：配置持久化三件套
  （跨实例 round-trip——save 后新实例 ctor load 回读 + is_connected 恒 false
  不虚标 / 同名原位更新不追加 / remove 只删指定项）；连接管理五件
  （webdav 未注册协议 → false + "Unsupported protocol" 错误、
  **http://127.0.0.1:1 回环连接拒绝秒败**（不出网）、回环 MockHttpServer
  连接成功——GET ?max-keys=1 探测请求真实到达断言 + connected 信号 +
  connected_storages、disconnect 清态发信号、断开未知名字仍发信号（幂等
  语义）、remove 已连接配置先断开再删）；无 browser 守卫四件（list 空
  回调 + "Browser not connected" 错误、upload 排队失败回调、五资源操作
  默认值返回）；request_download 协议 URL 表（s3/oss/cos/kodo/qiniu→
  **kodo 归一**/upyun/webdav 兜底七行 + download_requested 信号断言）；
  回环 mock 目录列举（Contents JSON 三项 → 隐藏过滤 + name 排序 + 字段
  映射 + directory_loaded 信号 + **服务器侧 list-type=2 请求真实到达**）；
  upload 本地文件打开失败 → "Failed to open local file"
- **线程语义（本批基建经验）**：list_directory 回调在 QtConcurrent worker
  线程直调（QMutex 编组共享状态收割）；upload_file 回调恒经
  QueuedConnection 回主线程（plain 成员即可）；worker 线程 emit 的信号经
  context-object connect 排队送达——spin_until（processEvents 驱动）统一
  收割；**fixture 非 QObject 时 connect 必须 `QObject::` 限定**（静态成员
  查找不到裸 connect）
- **QSettings 隔离**：IniFormat UserScope setPath 重定向临时目录 +
  setDefaultFormat + org/app 名（首个 QSettings 构造前）+ 每用例
  SetUp clear()——持久化用例不污染真实用户配置；StorageService ctor 即
  load_configs，隔离必须先于 fixture 构造
- **网络红线**：成功路径全走回环 MockHttpServer（storage 包共享基建，
  include storage tests/unit 目录）；失败路径 127.0.0.1:1——与 storage
  包 browser mock 测试同口径，零外网
- **验证**：两二进制 offscreen 与 xvfb-run（xcb）双跑全绿（clipboard
  10 + storage 16）；事件驱动用例 10 轮压测零失败；既有 78 用例回归全绿
  （backend 26 + url 8 + order 14 + update 7 + ipc 23）；build-cov 全量
  ctest 全绿门禁 + 铁账复核（desktop 不进 build-cov 分母——本批零生产
  改动，分母/miss 不变）；storage_service.cpp 存量 5 告警
  （QtConcurrent::run nodiscard 等）经生产 target 复证实为既有非本批引入

### 2026-09-30 - 覆盖率补缺：IPC 服务器回环测试 + 检查更新版本比较测试（desktop 48 → 78 用例）
- **选矿**（todo 勾选清零后的补缺轮；desktop 不进 build-cov 分母，按测试
  分布法盘点）：零测试桌面模块中挑两个收口——`ipc/http_server.cpp`（315 行，
  此前仅离屏 + curl 手工冒烟）与 `services/update_checker.cpp`（d9f471a
  新增即零测试）。其余零测试模块如实披露不在本批：clipboard_monitor /
  storage_service / search_service / download_service（UI 胶水层，需
  QApplication 级基建）；GUI 拖拽排序纯逻辑层复核确认已有 14 用例钉住
- **新 target `falcon_desktop_ipc_tests`（23 用例，真实 QTcpServer/
  QTcpSocket 回环全链）**：自动化 09-19 批次的手工冒烟（health 静态 JSON /
  tasks/stats 无 provider 503 + provider 透传 200 / OPTIONS CORS 三件套 /
  404 / 405 / 小写方法分发）+ POST /v1/add 全语义（202 + download_requested
  字段级断言——url/filename trim、referrer/user_agent/cookies 原样；非
  object JSON / 缺 url / 纯空白 url / 错路径 400×3+404）+ 解析边界（半截
  头挂起无应答补终结行后应答、垃圾请求行、Content-Length 非数字/负数、
  body 短传补齐后 202、300KB 无换行垃圾 413——尺寸检查先于解析不需要
  合法结构）+ 生命周期（stop 置零端口重启可用、**同端口二次 bind 失败**——
  QTcpServer 默认不设 SO_REUSEADDR，bind 语义实证）+ **回归钉子
  `PostAddRespondsBeforeDispatchingSignal`**：download_requested 处理器内
  嵌套 processEvents 自旋等 202 到达（模态对话框 exec 的精确事件循环形
  态——嵌套 processEvents 合法），「先应答再派发」修复从此有自动化防线
- **新 target `falcon_desktop_update_tests`（7 用例，静态纯函数零网络）**：
  `is_newer_tag_version` 全语义（三段 bump 全真 / 相等无论 v/V 前缀空白
  包裹全假 / 更旧假 / 两段式 tag "v0.2" 按 0.2.0 比较 / 8 种垃圾 tag 全假
  绝不误报）+ `current_version` 与 FALCON_VERSION_STRING 单一事实源钉死；
  **当前版本动态推导不硬编码——升版后本文件零改动**；check_for_updates()
  本体访问真实 GitHub API（外网红线），与 Kodo 官方域名用例同姿态不覆盖
- **可测性接缝**：`UpdateChecker::is_newer_tag_version` private → public
  static（行为零变化；版本比较是检查更新唯一可离线测试面，request_refresh
  提升 / detail 提升重构同先例）
- **事件循环纪律（本批核心基建经验）**：QTcpSocket::waitForReadyRead 不
  处理事件——同线程的 QTcpServer/readyRead 处理器永不触发，回环测试必然
  死锁；所有等待必须 processEvents 驱动（`spin_until`：processEvents
  (AllEvents, 50) + msleep(10) + QElapsedTimer 预算）；服务器每应答即
  disconnectFromHost（一连接一请求，helper 每请求开新连接）
- **验证**：desktop 78/78（backend 26 + url 8 + order 14 + update 7 +
  ipc 23）；ipc 套件 10 轮压测零失败；build-cov 全量 ctest 全绿（desktop
  不在该树分母，纯全绿门禁）；AUTOMOC 继承（父目录 set(CMAKE_AUTOMOC ON)
  子作用域生效）使 Q_OBJECT 生产源直接编进测试 target 零额外设置



### 2026-09-30 - 任务拖拽排序（表格 InternalMove 手动换位 + QSettings 持久化 + 过滤/视图模式共存）
- **纯 C++ 排序算法层**（`services/task_order.{hpp,cpp}`，无 Qt 依赖，单测
  钉死全语义）：`sort_ids(order, ids)`——排序键 (order 位置, id)，order 外
  任务排尾按 id 升序，**空序退化 = 纯 id 升序 = 改动前行为零变化**（旧表格
  插入循环与旧网格 std::sort 都是 id 升序）；`move_to`（行号换位，同位/
  越界原样返回）；`apply_drag`（可见子集按新序回填 order 成员位，非成员
  冻结——被过滤/另一视图的任务位置不动）；`serialize/deserialize`（逗号串
  "3,1,2"；**stoull 对 "-3" 回绕接受的坑**——解析前先验 token 纯数字，
  负号/空白/垃圾全跳过）
- **`TaskTableWidget`**（`widgets/`，QTableWidget 子类）：DragEnabled +
  AcceptDrops + InternalMove + DropIndicatorShown；**dropEvent 覆写不调
  base**——QTableWidget 的 InternalMove 落点是 cell-paste 语义（把单元格
  内容搬到目标格），不是行换位，必须 accept 后自己发
  `reorderRequested(from_row, to_row)`；`startDrag` 前后发
  `dragSessionChanged(true/false)` 包夹（QDrag::exec 同步阻塞在 startDrag
  内，落定/取消/拖出窗外三种收尾全覆盖）
- **DownloadPage 接线**：`update_tasks` 在拖拽会话期间挂起快照（行被重建
  会使落点行号失真），会话结束补刷最后一份；`on_table_reorder` 收集可见
  行 id 序列 → move_to → apply_drag → **剪掉已消失任务 id**（防 QSettings
  无限增长）→ 全量重插（rebuild_table_rows，与 set_view_mode 同款——就地
  搬 cellWidget 的进度条/操作钮过于侵入）→ 发 `task_order_changed` 串；
  网格视图 `sync_task_grid` 同走 sort_ids（卡片顺序跟随手动序，网格本身
  不支持拖拽——如实披露的取舍）
- **持久化**：MainWindow 拖拽即写 QSettings `desktop/task_order`（不等
  save_settings——排序是即时动作且退出时机不可控）；启动 load_settings
  读回（**value 必须在 endGroup 之前读**——endGroup 后读的是根节点）→
  `set_task_order` → 有行则立即重排，无行等首份快照 rerender 自然生效
- **测试**：新 target `falcon_desktop_order_tests` 14 用例（sort 空序/
  成员优先/未知排尾/重复取首现；move 双向/同位/越界；apply_drag 回填/
  空序种子/追加尾/防御；序列化往返 + 垃圾容错）——纯逻辑层全语义钉住，
  GUI 拖拽机制本身经编译与沙盒构建验证（Qt DnD 无自动化测试基建）；
  回归：desktop 全部测试 48/48（order 14 + url 8 + backend/trash/
  catalog 26）

### 2026-09-29 - ui_sandbox 原型模式 + 设计资产入库（prototypes 两版 / ui-sandbox 52 张）
- **`--prototype` 模式**：一次运行出主视图设计原型两版（暗色 1200×800，「下载中·表格」视图全要素）；`falcon-ui-sandbox --prototype <目录>`
- **cold utility 变体（会话内换肤，生产零触碰）**：`kColdHexMap` 14 对 hex 热→冷映射 + checkbox base64 SVG 整串替换（python 生成绝不手抄）+ `cold_palette()` 重建 QPalette（镜像 palette_for 冷值）；色板/对比度/已知边界详见 `docs/design/prototypes/README.md`
- **资产入库**：`docs/design/prototypes/`（warm-console-dark / cold-utility-dark + README——落地 B 只需把映射值写进生产 token/QSS）+ `docs/design/ui-sandbox/`（52 张矩阵 + README）；本机重新生成命令两处 README 均在位
- **验证**：PIL 像素级双图核对（bg 精确命中、accent px 计数、鲑橙残留恰为 logo bbox）+ 视觉模型复核布局完整；生产 theme_tokens/fluent_dark.qss 零 diff（git status 实证）

### 2026-09-29 - UrlDetector 网盘识别迁移 drives 层（拆库 A3 收口，识别面 5 → 13 平台）
- **架构对齐**（todo.md §4.1「UrlDetector 仍在 UI 层硬编码网盘规则」+ 决策 A3「URL 检测层不再把网盘规则散落在 UI 代码」）：网盘识别规则唯一事实源收敛到 drives 层 `CloudLinkDetector`——desktop 删除全部硬编码网盘正则与 parse_*_url 网盘方法，识别/文件名/展示名全部委托（detect_platform / extract_file_id / normalize_url / 新增 platform_display_name 目录）；UI 侧只保留 UrlProtocol↔CloudPlatform 双向映射与 parse_cloud_url 组装
- **drives 层增量**：`CloudPlatform` 补 `TianyiCloud`（枚举 + kPatterns `/t/` 分享路径 + extract_file_id case——对齐 desktop 既有识别能力）；新增 `platform_display_name()` 13 平台英文名目录（"Baidu Pan"/"Tianyi Cloud"/…，Unknown→"Unknown"）；TianyiCloud 不加 ICloudStoragePlugin（识别元数据与插件矩阵解耦，12 默认插件断言不变）
- **desktop 语义**：识别面 5 → 13 平台（微云/115/PikPak/MEGA/Google Drive/OneDrive/Dropbox/Yandex 随 drives 目录全量解锁）；网盘判定优先于 http/https/ftp（既有语义保持）；无 scheme 裸域名网盘链接经 drives normalize_url 补 `https://` 后识别（增强非破坏——旧 pattern 锚定 `^https?://` 不识别）；thunder/qqlink/flashget/magnet/ed2k 私有协议解析原样保留（legacy 行为零变化）
- **测试**：新 target `falcon_desktop_url_tests`（url_detector_test.cpp，Qt6::Core + Falcon::drives）8 用例——legacy 五平台「平台名 (分享码)」文件名逐字迁移锚 / 8 新平台识别 / 网盘优先于 HTTPS / 裸域名 normalize 路径 / get_protocol_name 目录 / contains_url / 私有协议回归（magnet dn/xl + thunder base64 解码 + ed2k）；drives 侧 cloud_storage_coverage_test +3 用例（Tianyi detect 三形态 / extract / platform_display_name 13 平台目录完整性——非空互异集合断言）
- **测量级教训**：手写 thunder 测试链接的 base64 期望值必须用工具生成不做心算——初版 "QUFBaHR0…" 解码为 "AAAhttp…"（base64 组 "QUFB"="AAA" 三字符），legacy 剥 2 字符 "AA" 后残留前导 'A' 断言红；Python 生成规范 `base64("AAhttp://example.com/a.zipZZ")` 替换后绿
- **验证**：build-desktop 四 target 构建绿；falcon_desktop_url_tests 8/8（新）+ falcon_drives_tests 167/167（含 3 新）+ falcon_desktop_backend_tests 26/26 回归全绿

### 2026-09-28 - 做种 UI 全链路（做种列 + 双设置入口 + 手动停止 + 哨兵默认协议）
- **下载页 7 列**：新增做种列（200px——170px 下「做种中 · 1.20 · 2时15分」mid-token 折行「2时15/分」；单行完整显示 `状态 · ratio · 时长`），数据来自 TaskSnapshot seed 扩展字段（ratio 分母 max(downloaded, total_size)）；**诚实性**：快照仅 seeding_active 布尔量，达标与手动停止不可区分 → 统一显「已停止」+ tooltip「做种已结束（达标或手动停止）」；非 BT 任务恒「—」
- **操作面**：已完成视图操作列对做种任务显「停止做种」钮（→ `DownloadService::stop_seeding` → backend → RPC falcon.stopSeeding）；网格卡片复用速度槽位显做种摘要 + tooltip（Completed 任务速度恒「—」，槽位天然空闲）；做种结束托盘通知「做种已停止」（notifications 开关门控）
- **设置双入口**：设置页「做种（BitTorrent）」组（ratio 0-999 step 0.05 默认 1.0 + 时长 0-525600 分钟 special「不限时」，reset_to_defaults 补 1.0/0）+ 添加对话框高级区做种两行（set_seed_defaults 预填全局值）；load/save_settings 持久化 seed_ratio/seed_time_minutes；启动与设置变更两路 apply_seed_defaults 到活动后端
- **哨兵协议**：`DownloadOptions::seed_ratio < 0` = 「用户未显式设置」——IPC/扩展/发现页路径 add_download_task 置 -1.0，backend with_seed_defaults 仅负值时填全局默认（两字段同填）；对话框 >= 0 原样透传（用户确认值不被覆写）
- **backend 层**：IDownloadBackend 增 stop_seeding/apply_seed_defaults（daemon 路径映射 falcon.stopSeeding；InProcess 后端直调 DownloadTask）；TaskSnapshot seed 五字段双后端直通
- **测试 6 → 10**：SeedingHandler（seed:// 协议做种形态同构 BT 收口语义）+ 4 用例（快照 RPC 往返与手动停止全链 / 错误路径 / 哨兵 vs 显式 / addUri 钳 0），20 轮压测零失败；`engine_.get_task()` 返回智能指针，测试用 `auto` 不能 `auto*`
- **ui_sandbox 28 → 36 张**：demo 做种任务 ×2 + 已完成网格视图 ×4 + 设置页做种组特写 ×4（QGroupBox 标题匹配 grab——滚动区折叠线以下整窗截图不可见）；36 张逐张目测通过
- **验证**：build-desktop 全量重建零 error + backend tests 10/10；daemon 五套件 + protocols 回归绿

### 2026-09-28 - UI 重设计「warm console」（theme_tokens 双主题全表 + 双 QSS 数值全表 + 形状三档收口）
- **设计语言**：与文档站同源 warm console（暖石/暖黑中性底 + falcon 橙单强调）；重设计-preserve（IA/布局/组件零变化只换皮）；taste-skill 代行（`popular-web-designs` 不在本环境可用清单）；字体保持系统原生栈（桌面 hinting 优于自托管 web 字体，品牌一致性经色彩/形状达成）
- **token 全表**（theme_tokens.hpp，palette_for 派生 QPalette 自动跟随）：light window #f6f4f1 / card #ffffff / text #292524 / secondary #57534e / disabled #a8a29e / accent #c2410c（hover #9a3412 / pressed #7c2d12）/ divider #e7e5e4；dark window #1b1714 / card #262019 / text #f2ede8 / secondary #b5aca3 / disabled #6e665e / accent #ffa07a（hover #ffb28c / pressed #e0875e）/ accent_text #27140a / divider #37302a；danger #c42b1c 双主题不变；checkbox 内嵌 base64 SVG 重生成（改色不改形）
- **QSS 结构性修正**：① dark `primaryButton:pressed` 删除 `color:#ffffff` 白字翻转（鲑橙 pressed #e0875e 仍为亮底，CTA 双主题统一「亮底深字」）；② dark `navTab:checked` 文字 #ffffff → #ffa07a（与 light accent 文字成对）；③ 控件族暖化（#333333→#312a24、#4d4d4d→#4a423a、#d9d9d9→#dcd7d1、#f9f9f9→#f5f2ee、#ececec→#edeae5、#3d3d3d→#3a322b、#5a5a5a→#554b41、checkbox stroke #8f8f8f→#a29a92）
- **形状单一规则**：按钮/输入 4→8px · 卡片/面板（QGroupBox/QMenu/Hero/任务卡）8→12px · 小件（工具钮/行内钮/菜单项/tooltip/navTab/段钮）4px 保持 · 条形（进度/滚动）高度半圆保持
- **托盘菜单**：qApp->setStyleSheet 全应用作用域自动继承，零额外改动
- **验证**：增量重建零告警 + backend tests 6/6；ui_sandbox 28 张双主题×双尺寸——bg 像素精确命中（#f6f4f1/#1b1714 八点全过）、主视图/对话框/设置页/网格目测验收；双 QSS 110 规则成对、26 个旧 hex 零残留、accent 各 12 处成对；BEFORE /tmp/ui_before vs AFTER /tmp/ui_warm


### 2026-09-28 - logo 全链路替换（qrc SVG + .ico/.icns/hicolor 资产）+ 顶栏 slogan 删除 + DaemonAddTaskRoundTrip 抖动修复
- **应用图标资产**（渲染源 assets/falcon.svg，头朝左 + 鲑橙 #ffa07a，cairosvg 逐尺寸矢量直渲）：`resources/icons/falcon.svg`（qrc `/icons` 前缀 alias）、`resources/windows/falcon.ico`（替换旧朝右深橙 4 帧 BMP——新 7 尺寸 PNG 帧 16..256，Pillow append_images 打包）、`resources/macos/falcon.icns`（手写 PNG-in-ICNS 容器，icp4/icp5/ic07..ic14 十 chunk）、`resources/linux/hicolor/{16..512}x*/apps/falcon-desktop.png` 九尺寸 + `falcon-desktop.desktop` 启动器（Icon=falcon-desktop）
- **运行时引用点**：main.cpp `setWindowIcon(QIcon(":/icons/falcon.svg"))`（qrc 内联）；main_window.cpp 托盘 SP_ComputerIcon → qrc logo；top_bar.cpp 品牌区 brand_mark_ 从蓝「F」色盒换 24px 真 logo pixmap + #brandMark 双主题 QSS `background: transparent`（蓝盒描边/圆角删除）；**「高速下载工作台」slogan 删除**（brand_subtitle_ 成员 + title_stack + 双 QSS #brandSubtitle 规则整链移除，顶栏单行化）
- **构建接线**：Windows exe 内嵌图标依赖顶层 `enable_language(RC)`（app.rc 引用本就接线，但 project(LANGUAGES CXX) 下 .rc 被静默忽略——PE 解析零资源段实证）；desktop CMakeLists 新增 `UNIX AND NOT APPLE` 分支 install .desktop + hicolor 到 `share/applications`、`share/icons`；Info.plist.in 补 CFBundleIconFile=falcon（bundle 目标未启用，前瞻）；nightly AppImage 图标 cp 仓库 hicolor 256px 替换 touch 占位
- **既有测试抖动修复**：DaemonAddTaskRoundTrip 立即断言 Downloading 撞 addUri 返回与 daemon worker 出队的异步窗口（20% 失败率）→ 2ms 轮询等待（5s 预算）后断言其余字段，20/20 压测绿
- **界面内 icon 审计结论**：qrc 27 个 Lucide 线性图标为现行 Fluent 体系功能组件（非旧品牌资产，UI 重设计批随设计语言统一），保留；唯一旧品牌资产即「F」盒 + 旧 .ico，已换
- **验证**：增量重建零告警（icon_utils.hpp -Wcomment 顺带收口）；backend tests 6/6；ui_sandbox 28 张双主题 × 双尺寸截图目测验收（logo 在位、slogan 消失、960 布局不破）


### 2026-09-19 - 浏览器扩展 IPC 只读查询端点 + /v1/add 应答被模态对话框绑架缺陷修复
- **只读查询端点三件套**（浏览器扩展任务面板数据面）：`GET /v1/health`
  （无副作用连通性探测，静态 JSON）/ `GET /v1/tasks`（任务快照数组）/
  `GET /v1/stats`（全局统计）。数据源经 `JsonProvider`
  （`std::function<QByteArray()>`）由 MainWindow 注入——快照在 GUI 线程
  刷新（on_tasks_refreshed/on_stats_refreshed 缓存到
  latest_task_snapshots_/latest_stats_），QTcpServer 信号同在 GUI 线程，
  回调内直接读缓存无并发问题；provider 未注入时 503 兜底。状态串对齐
  aria2 风格（Pending/Preparing→waiting、Downloading→active、Paused→
  paused、Completed→complete、Failed→error、Cancelled→removed），序列化
  用 Qt JSON（qulonglong→QJsonValue 歧义，须显式 qint64）。OPTIONS 预检
  补 GET 方法声明，全部 JSON 响应统一带 CORS 头
- **修复 /v1/add 应答被模态对话框绑架**（扩展联调回环冒烟曝光，离屏启动
  真实 falcon-desktop + curl 实测）：`emit download_requested` 是同线程
  直连，on_download_requested 弹模态添加对话框 `exec()` 阻塞到用户操作，
  202 应答被推迟到对话框关闭之后——扩展 1.5s 超时必然先到，误判"Falcon
  不可达"→ 不取消浏览器下载 → 文件重复下载。修复：先 write_json(202)
  再 emit（202 = "请求已受理"，任务是否创建由对话框决定）；修复前 POST
  3s 超时零字节，修复后即时 202
- **验证方法**：`QT_QPA_PLATFORM=offscreen` 启动真实 falcon-desktop +
  curl 回环——/v1/health、/v1/tasks（空数组）、/v1/stats、POST 202、
  OPTIONS 预检、404/405 逐项断言

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
- 云盘链接（13 平台，识别/文件名/展示名全部委托 drives 层
  `CloudLinkDetector`——UI 侧只保留 UrlProtocol↔CloudPlatform 映射，
  识别规则唯一事实源在 drives）

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

- [x] 任务拖拽排序
- [ ] 下载队列管理
- [ ] 下载速度限制
- [ ] 计划任务（定时下载）
- [ ] 下载完成后操作（打开文件、关机等）
- [ ] 多语言支持（i18n）
