# BUGS.md — 用户 bug 登记簿

> **流程制度（2026-10-01 起，用户令）**：用户汇报的每条 bug **置顶登记于此**，
> 优先修复，修完逐条回报。**每轮工作汇报必须单列「用户 bug 清单及状态」小节，
> 漏一条视为不合格。** 不许再出现「用户多次汇报一直被无视」的情况。

## 状态图例

`待处理` → `修复中` → `已修复（commit）` → `已到用户 builds`（修复进入用户可
获取的构建/发布后才算最终闭环）

## 活跃清单（置顶， newest first）

| # | bug 描述（用户口径） | 登记 | 状态 | 根因与修复 |
|---|---|---|---|---|
| B23 | 设置页所有复选框「隐形」：勾选/未勾选都看不见勾选框（用户看不见开关状态；2026-10-04 B21 Xvfb 走查发现，非用户汇报但按纪律置顶登记） | 2026-10-04 | 待处理 | 根因：`fluent_light.qss`/`fluent_dark.qss` 的 `QCheckBox::indicator` 用 `url(data:image/svg+xml;base64,…)` data-URI 形态——**Qt QSS 不支持 data: URI**，图标加载失败零渲染，勾选/未勾选两态均不可见（勾选态本应是橙色实心方块也测不到像素）。证据：真实应用（Xvfb :93 亮主题）设置页两态均无指示器（功能点击不受影响——QCheckBox 点文字仍翻转）；offscreen 沙盒 `settings_light_1200.png` 同象限复现 → 主题级缺陷非环境特有，系 2026-09-17 Fluent 批次「checkbox 内嵌 base64 SVG」遗留，沙盒目测验收漏检。修法（供修复批次）：SVG 落 qrc 资源文件以 `url(:/icons/…)` 引用，或改 QSS 原生 border/background 绘制两态方块；修复后须像素级复验（截图 diff 前后） |
| B19 | 剪贴板监听没生效：复制下载链接后没有弹窗提示下载。主流下载器行为 = 监听剪贴板识别 http(s)/磁力/ed2k 链接，弹「是否新建下载」提示（显示链接 + 确认/取消，可选不再提醒）；后台运行/最小化到托盘时也要生效（B14 复测仍不行的再报告） | 2026-10-04 | **已修复（本轮，真实走查闭环）** | 根因三层：① `clipboard_monitoring_enabled` 持久化默认 **false**——开箱即关，用户不进设置页永远不生效（主流下载器默认开）→ load_settings/reset_to_defaults 默认 true；② `start()` 清空去重基线——开启瞬间对既有剪贴板内容弹窗，默认开后变成每次启动都可能弹旧链接 → start() 以当前剪贴板内容播种基线（只对**监听期间新复制**弹窗）；③ 主窗隐藏到托盘时弹出对话框无前台激活——部分 WM 下对话框藏在其他窗口后面用户看不到「没反应」→ exec() 前显式 show/raise/activateWindow。验证：clipboard 测试 9 → 11（基线播种 + 重启不重弹两钉子）offscreen/xvfb 双跑绿 + desktop 全部 8 二进制回归绿；真实应用走查（Xvfb :91 + openbox 真窗口管理器）：新配置启动 → conf `clipboard_monitoring_enabled=true` 落盘（默认开实证）→ 前台复制链接弹窗全字段预填 → **主窗最小化后复制链接照样弹到前台**（xdotool 找到对话框窗口 + 截图）——「是否新建下载」提示 + 链接/协议/文件名/保存路径齐全 |
| B20 | 下载中/已完成页签与左侧菜单不同步：右边切「下载中/已完成」左边菜单不跟高亮，反向点左菜单右边也不切——要求双向联动 | 2026-10-04 | **已修复（本轮，真实走查闭环）** | 根因：双向只接了一个方向——侧栏→页签有既有接线（downloadingTabClicked/completedTabClicked → set_view_mode），**页签→侧栏完全没线**：`DownloadPage::set_view_mode` 只同步页内胶囊按钮 setChecked，不发任何信号，侧栏高亮永不跟随。修复三文件：① `set_view_mode` 记录实际变化（changed）并在变化时发新信号 `view_mode_changed(mode)`（不变化不发，防冗余）；② `SideBar::set_active_download_tab(completed)`——QSignalBlocker + 两钮显式 setChecked（setChecked 不触发 clicked 即两个信号源不回流；不依赖 QButtonGroup 排他行为在信号阻断下的实现细节）；③ MainWindow connect `view_mode_changed` → `set_active_download_tab(mode == Completed)`。回环终止论证：页签点击 → set_view_mode → 侧栏 setChecked（被阻断，无 clicked）→ 链终止；反向侧栏点击 → set_view_mode → view_mode_changed → set_active_download_tab 设置的目标态与当前一致（QSignalBlocker 下无回流）→ 链终止。验证：build 绿 + desktop 全部 8 二进制回归绿；真实应用走查（Xvfb :91 + openbox）：三个方向逐一点击截图——页点「已完成」→ 侧栏「已完成」橙色高亮跟随 + 内容区切换；侧栏点「下载中」→ 页头页签切回「下载中」+ 内容区切回；侧栏点「已完成」→ 页头页签跟随（既有方向无回归） |
| B21 | 浮窗三条（一起修）：① 浮窗太大——缩小尺寸（默认紧凑，显示链接/进度/操作按钮即可），别遮内容；② 右键菜单缺失——浮窗右键给常用项（开始/暂停、复制链接、打开所在目录、关闭浮窗）；③ 没有设置可以关闭浮窗——设置里加开关（显示/隐藏，默认显示但可关，跟随托盘退出不残留） | 2026-10-04 | **已修复（4b07962）＋ Xvfb 深度交互走查闭环（2026-10-04）** | ① 紧凑横条重构（与 B22 同一重构，见下行）：168×168 水位计画布 → 布局约束横条，默认档 Small=260px 宽（设置页「大小」下拉与 load 默认同翻 0，高度内容驱动 ~74px），标题行 = 任务文件名（中缀省略 + tooltip 全名）+ 暂停/继续钮，行 2 进度条 + 百分比，行 3 速度｜任务数两格；② `contextMenuEvent` 右键四项：暂停/继续任务（随预览任务运行态）、复制链接、打开所在目录、关闭浮窗（穿透开启时窗口对输入透明，菜单本就不可达——如实边界）；③ 开关即时接线（与 B14 同病收口）：设置页复选框 `toggled → float_widget_toggled` 信号 → MainWindow 处理器显隐 + 即时落盘 `desktop/float_widget_enabled`（沿 clipboard 范式，勾选后直接关机不丢）；右键「关闭浮窗」= 回写复选框 setChecked → 同一 toggled 链统一收口（无第二路径）；托盘退出显式 `speed_float_->close()`（独立顶层窗，QApplication::quit 不保证收窗）；数据面 `on_tasks_refreshed` 装配 TaskPreview（第一条 Downloading，否则第一条 Paused）+ 暂停/继续经 download_service 与下载页行内按钮同路径。验证：offscreen 沙盒 `--speed-float` 6 张（亮暗 × idle/mid/fast——预览省略/暂停钮/进度 42%/87%、无任务占位、两格无叠字逐张目测）+ build-desktop 全量 ctest 2610/2610 绿；**Xvfb :93 + openbox 真窗口管理器深度走查（2026-10-04）13 项全过**：默认形态/拖拽精确落位 + conf 即时落盘/重启位置保留 ×3/IPC 真实下载 4 连接/暂停钮冻结（3s 双帧一致 + 服务器 CLIENT GONE）/继续钮新连接续跑/右键菜单 4 项/菜单暂停-继续/复制链接 xclip 回读逐字节/打开所在目录 stub xdg-open argv 铁证/关闭浮窗 IsUnMapped + conf=false/设置开关复开 IsViewable；**穿透双向**：勾选+应用 → 浮窗中心右键出 openbox 根菜单（falcon 菜单不可达 = 输入穿透铁证）+ 拖拽纹丝不动，取消后菜单回归。未走到：托盘退出路径（Xvfb 无托盘宿主，代码路径已静态核对）|
| B22 | 浮窗速度和任务数量文字重叠：速度显示和任务数量挤在同一位置叠字。布局分格排开（间距/换行/分栏任选），字号间距调好，缩小浮窗尺寸时也不许叠（跟 B21① 一起验证） | 2026-10-04 | **已修复（本轮，与 B21 合并修）** | 根因：全部文本由 QPainter 按基线手算定位（速度基线 + descent + 4px gap 排副行），**漏算副行字体 ascent → 副行上浮 ~13px 与速度行叠字**；字体度量/翻译文案变长时无布局约束必然挤压。修复：QPainter 画布整体退役（水波纹/定时器/双通道裁剪绘制 ~200 行删除），改 QWidget 布局——速度与任务数进 QHBoxLayout 两个独立单元格（QLayout 单元格互斥占据，结构性消灭叠字，任何档位/翻译长度都不重叠），进度条独立一行，文件名行 `elidedText` 中缀省略（resizeEvent/showEvent/改档位三处重算）；尺寸三档改只定宽 {260,320,380}、高度交还布局。附带定性：`WA_TranslucentBackground` 在该窗上实测致局部 QSS 背景不绘制（Xvfb 直取窗口像素）——取消该属性，卡片底不透明 theme card 色 + QSS border 描圆角，配色 restyle() 局部样式表全取 theme_tokens 双主题表。验证：最小档（Small=默认）沙盒截图逐张目测无叠字 + 全量 ctest 绿（同 B21 行） |
| B1 | 下载页三张统计卡（活跃任务/已完成/当前速度）高度太高被布局拉伸；「下载中」hero 卡同样拉伸占大块——要固定高度紧凑横条（宽度三等分不动） | 2026-10-01 | **已修复**（本轮） | 布局无固定高度约束，纵向空间随窗口/字体度量拉伸。`download_page.cpp`：hero `setFixedHeight(78)`、统计卡 `setFixedHeight(72)` + 收纵向 margins；前后对比截图核验（沙盒 52 张资产同步刷新） |
| B2 | 下载完成后记录从列表消失（不能在已完成视图找到） | 2026-09-30 前多次汇报 | **已修复**（adf0e58，2026-10-01） | TaskManager 默认每 60s 清理终态任务，进程内后端直接读引擎内存 → 完成任务一分钟后蒸发。`set_cleanup_interval` 设 1 年等效禁用，终态保留至用户「清除已完成」手动清理 |
| B3 | 「已完成」统计卡计数恒 0（今天明明下载过东西） | 2026-10-01 | **已修复**（同 B2 根因，本轮合并查证） | 统计链（`update_summary_cards` ← `task_records_` ← 后端 fetch）无过滤、计数逻辑正确；0 的唯一来源 = B2 的 60s 清理把 Completed 任务从快照源里先擦掉了。当前 HEAD 链路核实：InProcess 后端返回全量任务、daemon 后端 tellActive+Waiting+Stopped 三段全覆盖 |
| B4 | 失败的任务在界面里凭空消失（两个视图都找不到，无法重试） | 2026-09-30 | **已修复**（adf0e58） | `task_visibility` 判定层：下载中视图 = 活动态 + **Failed**（可「继续」重试）；此前 Failed 不属于任一视图的显示集合 |
| B5 | 有任务和没任务时窗口内容跳来跳去、任务区体积突变 | 2026-09-30 | **已修复**（adf0e58） | 空态卡片自然高度直挂 + 表格 stretch 混排 → 下载页改 QStackedWidget 三页栈恒占同一 stretch=1 区域 |
| B6 | 多链接批量下载无一成功（HEAD 识别大小正常，等待后弹窗失败） | 2026-09-30 | **已修复**（2ee5c8e + adf0e58） | 双层超时缺陷：① `timeout_seconds` 被映射 CURLOPT_TIMEOUT（总时长硬帽 30s 杀死一切慢而健康的下载）→ 改 LOW_SPEED 停滞看门狗；② SegmentDownloader 30s worker 汇合硬帽 → 改无限期等待（worker 有界性由 curl 层保证） |
| B7 | Windows 启动先弹一个终端窗口 | 2026-09-18 | **已修复** | `set(WIN32_EXECUTABLE TRUE)` 变量名笔误，CMake 读的是 `CMAKE_WIN32_EXECUTABLE` → exe 一直以 CONSOLE 子系统链接。改 target 属性 WIN32_EXECUTABLE |
| B8 | 界面 logo 鸟头朝向疑虑（应为朝左） | 2026-09-27 | **已定性（非缺陷）** | 资产三处字节级一致均朝左 + 线上部署 commit 核对 + 像素分析；用户所见朝右为浏览器缓存。若复现请清缓存后再报 |
| B9 | 浏览器扩展发送下载后浏览器重复下载同一文件 | 2026-09-19 | **已修复** | `/v1/add` 的 202 应答被模态添加对话框 `exec()` 绑架推迟 → 扩展 1.5s 超时误判不可达不取消浏览器下载。修复：先写 202 应答再派发信号 |
| B10 | 原型图反复说要在文档中展示（快速预览上面），一直没做——图埋在 docs/design/ 子目录里用户看不到 | 2026-10-01（用户口径「反复说」） | **已修复（线上验收通过）** | 首版画廊仍不合意，用户明确验收口径：**走马灯/轮播**（自动播放+左右箭头+圆点），位于「快速预览」节**上方**，设计稿（warm/cold 全部关键屏）+ 实况截图，不进 README。落地 `ShowcaseCarousel.vue`（7 屏轮播，Vite 静态导入修子路径 404），commit f7a8318。线上验收（2026-10-02）：cuihairu.github.io/falcon 部署 chunk 含组件，无头浏览器实测 `.showcase` 在位、7 slides/7 imgs 全部加载、左右箭头 + 圆点在位、自动播放已进第 2 屏，截图留档 |
| B11 | 任务暂停之后点「继续」无法再恢复下载（用户给了真实复现 URL：nightly AppImage 直链） | 2026-10-02 | **已修复** | 根因两层：① `pause()` 转发 `downloader->cancel()`——析构无条件清段文件，断点从未保留，resume 只能全量重下；② 退出归类按任务状态——resume 抢跑竞态下旧实例退出时任务已被置回 Downloading，被误判为 Failed（「点继续后任务立即失败」）。修复：`cancel_preserve_segments()`（preserve 标志先置位再 cancel，析构跳过清理）+ `was_failed()` 作失败权威 + 孤儿 attempt 守卫 + 指针比较 EraseGuard + 取消路径 `sweep_orphan_segment_files()` 终局清扫。验证：CLI/daemon 真实 URL 走查（两轮/三轮暂停→继续，段文件保留、进度从暂停点接着走、成品 SHA256 对拍一致、零残留）+ PauseResumeE2E 3/3 + falcon_http_tests 111 + protocols 828 + daemon main 33 全绿 |
| B12 | 悬浮显示下载速度的悬浮窗（用户置顶，此前多次汇报「还是没有」） | 2026-10-02 | **已修复（真实走查闭环）** | `SpeedFloatWidget` 水波纹水位计（33ms 定时器 ~30fps）：① 常显速度主行 + 可选活跃任务数/总进度副行；Tool+Frameless+StaysOnTop 置顶、拖拽停靠、position_changed → QSettings float_pos_x/y 持久化（走查实测落盘 1030/715 精确复现拖拽位）；② 设置页悬浮速度窗节 6 控件（开关/显示任务数/显示总进度/大小 小132中168大208/透明度 30-100/鼠标穿透 WindowTransparentForInput）；③ 水位 = sqrt(速度/10MB) 指数逼近平滑（τ≈0.28s），双层正弦波相位速率 ∝ 速度（快则涌急），速度文本按水面裁剪干/湿双通道绘制任何水位可读，theme_tokens 双主题跟随；④ 真实走查：限速服务器 8MB/s↔1MB/s 交替供流，IPC POST /v1/add 全字段预填对话框同屏截图，35s 录屏 + 14 帧抽帧水位实测（高相 3.7-4.7MB/s → 水位 0.65-0.73，低相 924KB/s → 0.36，静息 0.11 起步）随真实速度起伏；成品 80MB SHA256 对拍一致；ui_sandbox --speed-float 离屏 12 张亮暗双主题 |
| B13 | 「还没有任务」空态被挤压在一起、大小不对、高度塌了——「下载中」「已完成」两个 tab 都有问题 | 2026-10-02（补充合并同口径） | **已修复（本轮）** | 空态卡自然高度 ~90px 塌缩（无最小尺寸约束）+ 字号同 14px 无层级 + 已完成 tab 文案与下载中相同。`download_page.cpp`：① `kEmptyStateMinHeight=232` + margins(24,36,24,36) 留白 + 既有双 stretch 包夹垂直居中保持（宽度本就随 layout 撑满）；② 双 QSS 成对新增 `#emptyStateTitle` 20px 600 文字主色 / `#emptyStateBody` 13px 次级色（沿字号刻度四档）；③ `update_empty_state()` 按视图动态文案——已完成 tab「还没有已完成的任务」+ 对应正文。过程中发现并修正布局陷阱：layout 层 `setAlignment(AlignCenter)` 使 wordWrap 标签按 sizeHint 紧凑宽度分配 → 正文缩窄列三行折行，改为标签内部居中 + layout 默认横向撑满（正文按卡片全宽折行）。验证：真实应用亮暗双主题 × 两 tab 前后四图对账（before 塌缩/折行/无层级/文案相同 → after 232px/撑满/居中/20-13px 层级/单行正文/新文案），build-desktop 全量 ctest 2607/2607 绿 |
| B14 | 「监听剪贴板」复制了链接不弹下载确认框 | 2026-10-02（优先级高） | **已修复（本轮，真实走查闭环）** | 根因两层：① 设置页复选框「自动检测剪切板链接」**从未连接** `clipboard_monitoring_toggled` 信号——勾选只翻 UI 勾选态，ClipboardMonitor 从不启停（持久化链路也没接），「开关」是愿望式控件；② 开关变化不即时落盘。修复：`settings_page.cpp` 复选框 `toggled` 即发信号（沿搜索引擎复选框即时生效范式）；`main_window.cpp` handler 内即时写 QSettings `desktop/clipboard_monitoring_enabled`（沿悬浮窗位置范式，不等「应用」按钮，勾选后直接关机不丢）。真实走查（Xvfb :88 前后截图 + conf oracle）：勾选 → conf=true → 复制本地服务器 http 直链 → **确认框必弹且信息完整**（协议/URL/文件名/保存路径/连接数/做种/UA/Referer/Cookies 全字段）→ **同链接重复制不重弹（去重照旧）** → 换链接重弹（证监听存活、去重按链接粒度）→ 关闭复选框 conf=false 复原。附带观察（既有行为，非本缺陷范围，待用户裁定是否要改）：a) 开启瞬间会对既有剪贴板内容做初始扫描并立即弹框；b) 确认框弹出期间检出的新链接会叠开第二个框而非更新当前框 |
| B15 | 下载任务列表默认视图应为卡片视图 | 2026-10-02 | 待处理 | 默认进卡片视图（文件名/大小/速度/进度条/操作钮），列表视图保留可切换；用户手动切换后记住选择（QSettings），首次/无记录默认卡片；设置项标注默认值；截图回传 |
| B16 | 批量操作菜单没按 tab 区分，两个 tab 都错 | 2026-10-02 | 待处理 | 「下载中」：全部开始/全部暂停/全部取消/删除任务（+清空列表）；「已完成」：**没有开始/暂停类**——清空完成记录/删除选中/重新下载/打开所在文件夹；切 tab 菜单实时换。两 tab 各点开菜单截图，逐项验证生效 |
| B17 | 「下载设置 → 资源搜索」配置不人性化（配置文件连例子都没有） | 2026-10-02 | 待处理 | ① 界面增删改搜索源（表单+placeholder+字段说明），不逼用户手写配置；② 配置文件内置示例条目/模板注释；③ 保存即时生效 + 输入校验（URL 格式/必填）；④ 真实走查：新增→搜东西能用→编辑→删除全流程截图 |
| B18 | 已完成任务双击不执行文件——迅雷是双击直接打开/运行下载的文件 | 2026-10-02 | 待处理 | 已完成任务行/卡片双击 = 用系统默认程序打开文件（QDesktopServices，跨平台）；右键菜单补「打开文件」（保留打开所在文件夹）；设置页加「双击行为」选项（打开文件默认/打开文件夹）；文件不存在/被移动时明确提示不静默；真实走查截图 |

## 观察项（非缺陷但用户关注）

| # | 事项 | 状态 |
|---|---|---|
| W1 | 修复到达用户的节奏：B2–B7 的修复已入 main，但用户机器上运行的 nightly 构建 **早于修复 commit**——「已修复 ≠ 已到用户」。需确认用户侧升级到含 adf0e58/2ee5c8e 的构建后逐条闭环 | 待确认 |

## 历史教训（为什么有这个文件）

2026-10-01 用户原话：「我列出多少次了，每次 falcon 都无视我的 bug 汇报。」
上述 B2/B4/B5/B6 在修复前的多轮会话中被反复汇报却未被置顶处理（部分被
「设计留白」之类理由搪塞）。制度性纠正：**用户 bug 汇报 = 最高优先级**，
登记 → 修复 → 逐条回报，三步缺一不可。
