# Falcon 下载器 - 项目架构文档

## 变更记录 (Changelog)

### 2026-09-18 - Nightly Windows 包资源编入实证（发布包二进制验证方法沉淀）
- **验证目的**：66b8144 资源修复只有"编译绿"证据（编译不查资源，正是
  上次缺陷逃逸的根因），触发 nightly（run 35291193607，三平台打包全绿）
  下载 Windows zip 对 exe 做二进制级验收：①包布局核对（qt.conf/
  plugins/platforms/qwindows.dll/Qt6Svg.dll/imageformats/qsvg.dll/CRT）；
  ②exe 内定位 rcc 名字数组与数据区；③压缩流全算法解压验证 QSS 内容。
  结论：资源在生产包闭环——名字数组完整 + 两份 QSS 解压字节与源文件
  精确相等（12358/12569）+ 27 个 Lucide SVG 明文 entry + 布局齐全
- **三个搜索陷阱**（本轮逐一踩过，下轮直接用正确姿势）：
  ① UTF-16BE 资源名搜索会被 .rdata 里的 UTF-16LE 代码字面量**错位
  误报**（load_qss 的 ":/styles/..." 字符串交错字节恰好构成 BE 序列，
  命中位置全是 theme_manager/icon_utils 的路径字面量）——真名字数组
  须搜 `[2B len][4B hash][BE 名字]` 条目结构（fluent_light.qss =
  `00 10 05 47 53 a3`）；② 资源体压缩形态**随平台 rcc 而异**：本机
  Linux rcc 用 ZSTD（`28 b5 2f fd`）、nightly Windows rcc 用 zlib
  level 9——只搜一种压缩头必然漏；③ zlib 头随压缩级别变化
  （78 01/5e/9c/da），四种全扫并逐流试解压才不漏
- **决定性判据**：名字数组条目结构完整 + 解压流字节数与源文件逐一
  精确相等 + 图标以 `[4B BE 长度头][明文]` 标准 rcc entry 存在；
  exe 内数组布局 data → name 紧邻（名字数组紧跟最后一个数据 entry，
  可反推数据区位置）

### 2026-09-17 - 桌面资源链路致命缺陷修复（资源从未编入二进制）+ 离屏 UI 截图沙盒
- **资源从未编入二进制**(离屏截图首跑曝光,上一轮 UI 重做的遗留):
  `qt_add_resources` 在 add_executable 之前调用是无效调用且 resources.qrc
  从未进 target sources → AUTORCC 不触发,任何平台二进制里都没有资源;
  CI 三平台绿掩盖(编译不查资源),运行时 QSS/图标读取全部落空仅
  WARNING——Fluent 样式与图标实际从未生效。另有两层:qrc 的 prefix 与
  file 相对路径叠加成双层前缀与代码读取路径不一致(全部 file 加 alias
  收口);qrc 引用构建产物 .qm 在无 LinguistTools 环境构建顺序死锁
  (删 /i18n 节,翻译保持 exe 旁回落软依赖)。修复后 resources.qrc 进
  两个 target sources 交 AUTORCC。教训:`rcc --list` 只列磁盘路径,
  资源树形态须探针程序 QDir(":/") 递归实证
- **网格视图两处真实缺陷**(沙盒截图曝光):下载页主布局视图容器无
  stretch + 尾部 addStretch 吃光剩余空间 → QScrollArea 初始 sizeHint
  近 0,网格视口被压扁卡片只露顶部;改表格/网格 stretch=1 互斥占满。
  sync_task_grid 按 QHash 迭代无序 → 卡片顺序每次切换抖动;按 task id
  排序
- **离屏 UI 截图沙盒**(`FALCON_BUILD_UI_SANDBOX`,默认 OFF):
  apps/desktop/dev/ui_sandbox.cpp 离屏渲染主窗布局 × 亮暗两主题 ×
  5 视图 = 10 张 png,供无显示环境设计验收/视觉回归(可挂 CI 出快照);
  本机以 aqtinstall 官方 Qt 6.10.3(6.10 起 Linux 架构名
  linux_gcc_64,qtsvg 已是基础归档)+ 双前缀 CMAKE_PREFIX_PATH
  (官方 Qt 在前 + build-ci vcpkg_installed 在后、不用 vcpkg toolchain
  防 manifest 接管)完整编译桌面应用与沙盒

### 2026-09-17 - 桌面 UI 结构性重做（Fluent/Win11 设计体系 + 内嵌 Lucide SVG 图标 + 无边框窗口修复）
- **根因诊断**：三套互相矛盾的样式并存（theme_manager.cpp 978 行内联 QSS
  唯一生效、styles.hpp 396 行死代码、main.qss 220 行编入 qrc 从未加载），
  亮暗两套规则集严重不对齐（dark 缺 navTab/taskTable、light 缺
  QScrollBar/QDialog）；无边框窗口零拖动/缩放实现；窗口控制钮 "_"/"[ ]"/"X"
  文字钮吃通用按钮 QSS 与 setFixedSize 冲突；TopBar/StatusBar 大量零连接
  死按钮；侧栏三组 QButtonGroup 互不排他；footer 假数据 "12" 与
  "Preview UI" 徽章；18 处 QStyle::standardIcon 与文字按钮混杂
- **Fluent 设计地基**：fluent_light.qss / fluent_dark.qss 严格成对编写
  （11 节，选择器经脚本与代码 setObjectName 双向校验零死选择器），
  theme_tokens.hpp 语义色 token 单一事实源（QPalette + Fusion 基座与 QSS
  同源）；删除 styles.hpp 与 main.qss
- **内嵌图标系统**：27 个 Lucide v0.294.0 SVG（ISC）入 qrc，
  QSvgRenderer 渲染 + currentColor 替换着色；TokenIconEngine（QIconEngine
  子类）绘制时取当前主题 token——换肤后已存在的按钮图标自动换色；
  QPixmapCache 按 id/色/尺寸/DPR 缓存。Qt6::Svg 转 REQUIRED，CI apt 加
  qt6-svg-dev、vcpkg 加 qtsvg(!linux)、nightly EXTRA_QT_MODULES 补 svg
  （漏掉 = AppImage 编译不报错运行时崩）
- **无边框窗口 chrome**：TopBar 空白区 startSystemMove 拖动 + 双击最大化；
  qApp 级 eventFilter 实现 8 向边缘缩放（6px 感应带 + startSystemResize +
  方向光标；必须 qApp 级——中央控件吞自身鼠标事件、单控件 filter 拦不到
  孙辈；谓词 window()==this 天然排除菜单/tooltip 独立窗口）；窗口钮图标
  化（Minus/Square↔Restore/X，changeEvent 同步最大化状态）；最小尺寸
  960×640
- **交互诚实化**：TopBar 搜索框接通下载任务过滤（DownloadPage::
  set_text_filter 单点 should_show，表格/网格双视图生效）、视图切换钮接通
  toggle_display_style（非下载页禁用）、刷新钮接通 request_refresh（从
  private 提升 public——线程安全置位语义，后端回调本就从外部线程调用）；
  StatusBar 删 4 个零连接按钮/信号与 detection badge；侧栏三组合一 exclusive
  nav_group_（选中态与页面一致）；footer 显示真实活跃任务数
- **各页 Fluent 化**：18 处 standardIcon → 主题感知 SVG；行内"暂停/删除"
  文字钮与卡片操作钮图标化（保留 taskId property 机制）；表格文件名列
  Stretch 伸缩；删 4 处 setFixedHeight(34) 与 3 处硬编码字体（收口到
  #pageTitle/#sectionTitle/#emptyStateTitle/#cardFileName）
- **CI 首轮曝光五类编译错**：src/widgets/ 下裸相对名 include 找不到
  icon_utils.hpp（改 ../utils/ 路径）；QIcon/QStyle/QWindow 仅前向声明导致
  TokenIconEngine override 失效与 startSystemResize 不可用（补
  QIconEngine/QStyle/QWindow 完整 include）；本机无 Qt6 无法预编译，
  此类错误靠 CI QT6 job 收敛
- 三平台 Qt6 编译全绿（run 35247334598），falcon_desktop_backend_tests
  零风险（backend 层零改动）；手工验收项：拖动/8 向缩放/双击最大化/亮暗
  切换图标换色/搜索过滤/列伸缩

### 2026-09-17 - 覆盖率批次 V：行 94.9% → 95.1%（V2 引擎命令防御直调 + 多段超时清理/多段 pause→resume abandon + 四云官方域名兜底）
- **11 新用例，cov 全量 ctest 2106 清单 100% 通过（exit 0 零失败）
  + ASan 11 新用例零告警**；miss 841 → 816（净收敛 25 行）：
  - protocols http_commands_coverage 3（HttpResponseCommand/
    HttpDownloadCommand 的 execute(nullptr) 防御直调——handle_result
    纯状态机不触 engine 可安全断言 FAILED；set_global_speed_limit(0)
    的"全局限速: 取消"日志分支）
  - run_test 1（task_id=999 的抛异常命令——find_group 失败的
    fail_group_of_command 无组收口 + 引擎存活）
  - edges 2（**多段段命令超时清理**——段 1 连接读头后挂死 8s 内
    poll 等 EOF（客户端 sweep 关 fd 即 break，不拖 server.stop()），
    timeout_seconds=1 任务级超时 → cleanup_completed_commands 复用
    fail_group_of_command 的 is_multi_segment 分支 finish_segment(
    false)，既有黑洞用例全是单连接任务；**localhost 域名下载**——
    resolve_host 成功路径 + 事件唤醒重入，成品逐字节一致）
  - pause 2（**重试窗口内暂停**——端口 1 连接拒绝 + retry_delay
    3600s，HttpRetryCommand 以 NEED_RETRY 回队轮询期间 pause_task，
    下一轮 execute 入口 PAUSED 守卫静默收口，组保持 PAUSED 无错误
    消息；**多段 pause→resume abandon**——恢复初始连接带断点
    Range 收 206 → determine_download_strategy 经 has_resume_state
    进 schedule_resume_download → is_multi_segment 防御触发 abandon_
    resume + 全新无 Range 下载；路径铁证：resume 后新连接数增量 >
    Range 连接数增量 ⇒ 存在无 Range 全新初始连接，与单连接续传的
    "恢复连接必带 Range"可区分）
  - storage 3（kodo/cos/upyun connect 无 endpoint/api_domain 的官方
    域名兜底分支——rs.qbox.me 真实 401/{bucket}-{appid}.cos.ap-test
    .myqcloud.com DNS 失败/v0.api.upyun.com 兜底赋值+官方域名拼接，
    一个用例双收兜底与拼接两处，URL 拼接先于请求故覆盖与请求结果
    无关）
- **六项撤销定性（写测试前核对访问性/调用图，铁证齐全）**：①
  thread_pool.hpp:47 submit-after-stop throw——stopped_ 唯一置位点
  在析构（thread_pool.cpp 全文），无公开 stop 方法 → 无合法调用
  窗口（UAF）；② notify_segment_failure 的 engine-null 防御（1158-
  1166）——private 方法且调用者恒传有效 engine；③ handle_redirect
  的 engine-null 防御（1554）——同上（编译期 private 访问错误实证，
  撤销已写用例；execute 开头 null 防御先 return，1296 调用点 engine
  恒非空）；④ window_recovery_point 381 的 `return {}`——调用点
  守卫（total>=task_limit / speed_window_bytes_>=limit）+ prune 同
  步递减 total → samples.empty() 组合不可达；⑤ download_engine_v2
  .cpp:769 状态守卫出口行——需 PAUSED 组挂起命令逃脱 sweep 存活到
  超时清理，与"sweep 正确收走"不变量互斥（`if (!group) return;`
  实际在 760-761 且已被异常命令路径覆盖）；⑥ http_commands 1976-
  1978 FAILED 守卫 + daemon 683-694 放弃（低价值/进程级）
- **一项未命中（诚实记录）**：http_commands.cpp:690 resolved_ip_
  缓存命中——localhost 回环连接首次 connect 立即成功，无
  EINPROGRESS 挂起→事件唤醒的二次 execute 重入；同用例的
  resolve_host 成功路径已收（引擎日志"localhost 解析为 127.0.0.1"
  铁证）
- **98% 结构性不可达定量更新**：miss 816 = header 实例水分 ~212
  （logger.hpp 127 + thread_pool.hpp 31 为主，跨 TU 内联展开必然
  miss）+ 历史逐批定性不可测 .cpp ~604（OOM 注入/平台分支/伪影/
  竞态/结构不可达/官方域名深层分支）；可测矿点至此收尽，**95.1%
  为当前口径收口值**（行 95.1% / 函数 98.6% / 分支 53.9%，批次
  C 同款 gcovr 两树合并口径）

### 2026-09-16 - 覆盖率批次 T+U：行 82.7% → 94.9%（daemon 启动/停机边界 + 四库冷门残矿 + V2 适配器收口）+ redirect_stdio fd 顺序缺陷修复
- **49 新用例，cov 全量 ctest 2096 清单 100% 通过（exit 0 零失败）
  + ASan protocols 736 零告警**：
  - daemon main_integration 8（非法 --http-engine 报错退出、限速启动
    生效 + SIGHUP 热更、坏 task db 降级继续服务不退出、直连 db 预置
    Downloading 记录重启自动 start、SIGHUP 不可热更项 4×"restart
    required" + 未知节告警、--daemon 默认 pid 路径不落盘（从 log 文件
    启动行提取孙进程 PID）、传输中 SIGTERM 排水干净退出）；rpc
    client 6 + coverage 1 + storage 1 + listener 1 + task_storage 3 +
    ws 帧边界 2
  - core 9：download_engine 4（构造器限速生效、add_task_as_id 冲突
    防御、输出名变体、建目录失败抛异常）；protocol_registry 1（null
    handler 注册忽略）；logger_spdlog 4（to_spdlog_level 全枚举 + 非法
    兜底、FalconConsoleSink 六级别 tag 直驱 + flush 双通道、log_*
    函数级门禁 + FMT 四链、format_log_message 分支矩阵含 null 指针
    双形态与未闭合 '{'）
  - drives config_manager 10（无主密码初始化拒绝、库独占锁下读写
    失败、删表/改视图后全方法失败、重名 update、坏 extra JSON 容
    错、导入非 JSON 明文拒绝、非 string extra 值容错）
  - storage 三 browser mock 各 1（kodo stat 坏 JSON 容错、s3 配额坏
    JSON 容错、upyun 非数字 size 判目录）
  - protocols 5：v2_http_adapter 3 参数化（HEAD 放行 GET 404 的组错
    误传播、同引擎 PAUSED 组 resume 续跑、引擎侧 pause_all 的 V1 状
    态对齐——V1 参数化侧设计内 skip）；http_commands_edges 2（**TLS
    垃圾 record 硬失败**——服务器 SSL_free 后在原始 socket 裸发未知
    record 类型，客户端 SSL_read 得 SSL_ERROR_SSL 协议违规路径，与
    ZERO_RETURN/SYSCALL 的 EOF 语义分流；**代理空 authority 重定向
    失败**——`http:///x` 经 absolute-form 原样透传代理，302 非绝对
    Location 的基准 URL 提取空 authority 按失败收口，恰 1 次连接 +
    请求行透传断言）
- **两个生产缺陷修复（daemon）**：① redirect_stdio 的 fd 顺序缺陷——
  close(0/1/2) 后打开的 log 文件恰好落在 STDOUT_FILENO 上，末尾无条
  件 close(log_fd) 把 stdout 关掉，daemon 模式全部 INFO 日志 write(1)
  得 EBADF 静默丢弃（log 文件只剩 stderr 输出）；补 `log_fd >
  STDERR_FILENO` 守卫，DaemonModeDefaultPidFileUsed 从 log 提取 PID
  的断言同时回归此缺陷。② stop_persistence 判空——坏库降级路径
  task_listener 未创建，停机回调裸调 shutdown() 即崩溃
- **三个测量级定性发现（http_commands.cpp 调用图铁证）**：①
  write_to_segment 越界丢弃分支（2314-2318）**结构不可达**——
  check_completion 在 receive_data 循环内每次写入后同轮判定收满即
  return OK，write_to_segment 永远不会在 downloaded ≥ length 时被进
  入（防御代码）；② SSL_write 失败分支（1090-1097）**时序不可达**——
  execute 的 TLS_HANDSHAKING case 握手完成同轮 fallthrough 立即发
  送，客户端 SSL_connect 完成（收到服务器 Finished）后 SSL_write
  先于服务器的 RST 到达网络；③ verify_ssl 失败在握手层先拒，
  833-840 的 post-handshake 检查永不到达
- **98% 行覆盖结构性不可达的定量分解**（官方 gcovr 口径 16561 行 →
  98% 预算 miss ≤331，实际 miss 841）：header 实例水分 212 行
  （csv/print-summary 把每个 TU 的 header 实例行拼接计数，logger.hpp
  127 + thread_pool.hpp 31 + 其余小头——测试 TU 被排除、未测 TU 的
  内联展开必然 miss）+ 历史批次逐一定性不可测 .cpp ≈630 行
  （curl/sqlite/EVP OOM 注入防御、Windows 平台分支、gcc 行归属伪
  影、时序竞态窗口、同轮短路结构不可达、接口零调用方），212+630≈841
  与实测自洽
- **全包覆盖率（批次 C 同款 gcovr 口径）：行 94.9% / 函数 98.6% /
  分支 53.7%**（批次 S 82.7/96.7/46.1，涨幅 +12.2/+1.9/+7.6）

### 2026-09-15 - 覆盖率批次 S：config 13→0 + event_dispatcher 20→0 + resume_control 16→2 + password_manager 16→2 + event_poll 3 行收敛（六小文件 64 行真矿清账）
- **31 新用例三树绿**（cov 全量 ctest **2043 清单 100% 通过零抖动**
  + ASan 三套件 421/734/27 零告警）：daemon config 10（非 rpc 节
  的「节非 object」×3 与各节键类型错误 ×7——此前只测过 rpc 节的
  两种失败形态）；core dispatcher 4（dispatch_sync /
  clear_listeners / get_listener_count / is_running 四个全库零
  调用公开方法——既有 DispatchSyncDoesNotQueue 实测的是关异步后
  dispatch() 的同步路径，并非 dispatch_sync 本身）；core
  password 3（HOME 空时哈希文件落 cwd 兜底、无回调控制台分支、
  length=1 的 required_sets 提前 break）；protocols
  request_group 14（resume 控制文件 save/load 解析边界：空
  path/父目录缺失/garbage 行/total 空值-半数字-非数字/segments
  非数字/seg 四元组缺字段/缺 total/零 total/段数不符/CRLF 容错
  与魔数拒绝对照/remove 空 path no-op）；protocols event_poll 3
  （epoll 对已关 fd 的 MOD 得 EBADF、poll 对已关正整数 fd 的
  fcntl 探测失败、双注册单就绪时 revents==0 跳过）
- **测量级发现**：resume 控制文件的 \r 裁剪只作用于 body 行——
  魔数比较先于裁剪，完整 CRLF 文件被魔数直接拒绝（严格语义，
  测试以「魔数 LF + body CRLF 成功 / 全 CRLF 拒绝」对照固化）；
  prompt_password 的 POSIX termios 分支无条件执行（tcgetattr 失
  败仅忽略返回值），cin.rdbuf 替换即可全量覆盖
- 剩余全部定性（六文件 29 行不可测）：password 2（RAND_bytes
  失败注入）；resume 2（写中途失败）；epoll 14（create1 失败 ×2、
  实例未创建结构分支、EINTR/等待失败、未知 fd 竞态）；poll 9
  （nfds_t 超量、EINTR、poll 失败、未知 fd 竞态）
- **全包覆盖率（批次 C 同款 gcovr 口径）：行 82.7% / 函数
  96.7% / 分支 46.1%**（批次 R 82.4/96.3/45.9）

### 2026-09-15 - 覆盖率批次 R：request_group 31 → 2 + cloud_storage_plugin 75 → 20 + http_commands 145 → 135（NEED_RETRY 可测化 + WS 测试挂死修复）
- **27 新用例三树绿**（cov + ASan，protocols 717 / drives 152 /
  daemon_rpc 25）：request_group 13（URL→文件名推导、socks5 代
  理拒、resume 控制文件三态校验、Manager 调度丢弃与孤儿补插
  队）；http_commands_edges 8（零字节下载、超大响应头、拆分计
  划回退单连接、chunked 大小行四变体帧错误、**32MB 突发-静默-
  再突发 NEED_RETRY 全链**——64×64KB 轮上限让出 + execute 尾部
  update_progress 速度计算分支）；cloud_storage 6（经新增
  `CloudStorageManager::plugins()` 只读访问器离线直调四件套存根
  与 display_name 错误路径）
- **NEED_RETRY 可测化方法论**：阻塞 send 剧本服务器把节奏同步
  到客户端排水（背压恒小永不触发轮上限）——**深灌（每段 16MB
  预灌）+ 客户端直写（enable_disk_cache=false）** 构成内核积压
  单调增长，单次 execute 确定性读满 64 轮；静默 >1s 后再让出顺
  带覆盖速度计算分支
- **修复 WsServerTest 断言失败即挂死**（全量 ctest 实证：负载下
  通知 5s 未到 → ASSERT 提前返回 → release() 漏调 → 无界
  cv_.wait → 析构 join 卡死 1:59:16）：BlockingHandler 的
  cv_.wait 改 wait_for(30s)（TwoStageHandler 同款），一处修复
  覆盖全部四处 release-after-ASSERT 用法——flake 仍会红但不再
  无限挂
- 测量级发现：execute 的 2065-2076 第二完成收口结构性不可达
  （download_complete_ 全部置位点伴随 receive_data 返回 OK，
  2091 同轮收口先行，入口检查恒假）；cloud_storage 剩余 20 行
  全为 map/quota 初始化字面量的 gcc 行归属伪影（断言通过即执
  行铁证）+ 2 行结构不可达防御
- **全包覆盖率（批次 C 同款 gcovr 口径）：行 82.4% / 函数
  96.3% / 分支 45.9%**（批次 Q 81.9/94.9/45.5）；全量 ctest
  2010 清单仅 2 例既有并行抖动（DownloadEngineTest.ResumeTask /
  TaskManagerPriorityTest.PriorityDequeueOrder，串行复跑即过）

### 2026-09-14 - 覆盖率批次 Q：resource_browser 系 75 → 9 + resource_search.cpp 47 → 9（detail 提升重构）
- **24 新用例双树绿**（cov + ASan，全量 ctest 1983 零失败）：storage
  侧 resource_browser_edges_test.cpp 17 用例（工厂注册边界与运行时
  is_supported 探测 PRIVATE 宏、format_tree 树枝/收尾分支与
  max_depth、format_table 的 ls 风格 d/-/l 标识、format_custom 列
  选择与 15 字符分隔线、normalize_path 盘符与相对 ".." 实现语义、
  join_path 空侧/盘符根、get_parent_path 根与裸名、is_valid_path
  控制字符与 tab/DEL 边界）；drives 侧 resource_search_coverage_
  test.cpp +7（validate_url 前缀白名单、parse_magnet_link 哈希与
  dn 解码、url_decode 非法转义容错、provider 层 filter 排序与截
  断、空 URL 请求前快速返回）
- **detail 提升重构**（仓库既有 detail 模式，生产行为零变化）：
  resource_search.cpp 的 validate_url/url_decode/parse_magnet_link
  提升到 detail 命名空间（头文件补声明，含漏声明的 parse_size，
  测试前向声明块删除）；**删除 NoCrawlerTag 孤儿构造函数**（零引
  用死代码）；**删除 set_headers 死防御 if**（headers_ 单调用点，
  curl_slist_free_all 对 NULL 安全）
- **三个测量级发现**：① ISearchProvider::validate_url/get_details
  生产全库零调用方（grep core/cli/daemon/desktop/drives 实证，
  Manager 不转发）——实现体结构不可达，属接口完整性方法；②
  normalize_path 对相对路径组件也前置 "/"（"a/b/.." → "/a"、
  "../x" → "/../x"、唯一特例 "a/.." → "."）——测试忠于实现并注
  释说明；③ is_valid_path 的 `<32` 检查不含 DEL(0x7f)
- 剩余 18 行全部定性：resource_search 9（curl OOM ×1 + 接口零调
  用方 ×8）；resource_browser 7 行归属伪影（并列实参计数矛盾铁
  证：调用行 8 命中 + lambda 行 28 命中 + info 实参行 #####）；
  utils 2 死代码/不可达（join_path 162 恒假条件、get_parent_path
  197 与盘符分支矛盾）
- **全包覆盖率（批次 C 同款 gcovr 口径）：行 81.9% / 函数 94.9% /
  分支 45.5%**（批次 P 81.4/94.4/45.1）；1 例 WsRpcClientEdge/
  PerformanceLargeBatch 并行抖动串行复跑即过（两树各 1 例）

### 2026-09-14 - 覆盖率批次 P：json_rpc_server.cpp 68 → 15 miss（RPC 分发边界 + WebSocket 协议路径）
- **21 新用例三文件**（cov + ASan 双绿，全量 ctest 1958 零失败，
  新增 21 条）：RPC 分发层参数形状与"合法 gid 无任务"变体全簇
  （changePriority/tellStatus/getFiles 族/pause 族/
  removeDownloadResult，含活动任务拒绝 code 1、终态移除 OK、整
  数值 max-concurrent-downloads、getOption 自定义 header 回显）、
  bind 占口 start 失败（POSIX only，Windows SO_REUSEADDR 可双绑
  定）、半截 HTTP 头写端关闭；WS 升级 path 白名单 404、握手
  CORS 回显、ping→pong/pong 忽略、坏操作码 1002 close、慢分发
  +RST 双失败路径（set_shutdown_handler 滞留会话线程 400ms +
  SO_LINGER{1,0} RST——广播命中死 fd 与应答发送失败两条注销路
  径）、Preparing→Downloading 通知、进度节流窗口到期恢复、引擎
  移除后通知退化为仅 gid；storage 侧 unpauseAll 收集/落库链与
  tellWaiting storage 回落并集
- **两个测量级发现**：① 既有 gid 用例的 "00000000000ffffffc"
  是 17-18 字符被长度检查拒绝——命中非法 gid 路径而非"合法 gid
  无任务"，后者须用 16 字符 "00000000000000ff"；②
  json_rpc_server.cpp:372（Preparing→Downloading 分支）#####
  为 gcc 行归属伪影——task_manager worker 层调 handler->
  download 前已置 Downloading（task_manager.cpp:831），handler
  内 set_status(Preparing) 即产生该通知，探针实证 status 2→1→2
  + 371/374 计数相等的执行序矛盾铁证（`||` 链指令归属 371 行）
- 剩余 15 miss 全部定性：伪影 ×2（372/987）、OOM/发送失败注入
  ×6、时序竞态 ×2（Pause failed/Remove failed 仅任务消失瞬间
  可达）、结构不可达 ×1（1321——add_task 失败恒抛异常）、全枚
  举兜底 ×1
- **全包覆盖率（批次 C 同款 gcovr 口径）：行 81.4% / 函数
  94.4% / 分支 45.1%**（批次 O 81.2/94.5/44.5，函数 -0.1 为边
  缘函数计数翻转）；json_rpc_server.cpp 单 target 无多编译水分

### 2026-09-14 - 覆盖率批次 O：incremental_download 46 → 11 + file_hash 23 → 12 + OpenSSL 宏 PUBLIC 化
- **修复公共头 ODR 隐患**：`FALCON_USE_OPENSSL`/`FALCON_ENABLE_
  OPENSSL` 从 falcon_protocols 的 PRIVATE 改 PUBLIC——
  http_commands.hpp 等公共头按宏条件声明成员，消费方 TU 必须
  与库同布局；同时修复 13 个 FileHash 测试因测试 TU 看不到宏而
  运行时 skip 的问题（全量 ctest skip 60 → 11，余量设计内）
- **删除死代码 mergeFile**（49 行）：private 零生产调用方，唯
  一"消费者"是被整块注释的测试
- incremental_download.cpp gcov miss **46 → 11**：6 新用例
  （CRLF 行裁剪/零分块两态/未知哈希算法降级/无效远程哈希列表
  回退/Range 短传干净失败/目录读失败）挂 `falcon_protocols_
  tests`；file_hash.cpp **23 → 12**：未知算法枚举防御 +
  get_hash_length default
- **修复 Windows SEH 崩溃（CI 曝光的生产缺陷）**：calculate 的
  switch 无 default，未知枚举使 md_type=nullptr 直达
  EVP_get_digestbyname——Linux OpenSSL 防 null 安全返回（本地
  测试绿，掩盖问题），Windows OpenSSL 解引用 null 崩溃（SEH
  0xc0000005，run 34892890668 实证）。补 `default: md_type =
  ""` 走既有获取失败防御路径，两平台一致；损坏的持久化算法字
  段在 Windows 上原会使 calculate 段错误
- 剩余定性：EVP 注入防御 ×17、curl OOM ×2、阈值日志 ×1、环境
  不可达 ×4、downloadRange 零尺寸路径（memcpy(dst,nullptr,0)
  UB，记录潜在缺陷不覆盖）
- **全包覆盖率（批次 C 同款 gcovr 口径）：行 81.2% / 函数
  94.5% / 分支 44.5%**（批次 N 81.0/94.3/44.3）；全量 ctest
  1938 零失败；新增 8 用例 cov + ASan 双绿
- Windows CI 修复（c7feabc）：http_handler_edges_test.cpp 补
  _WIN32 适配块（批次 G 漏 guard，MSVC C1083）
- 测量备忘：**多 target 编译水分**——同一 .cpp 编进多个 target
  时单份 gcov miss 虚高（daemon/config.cpp 54 → OR 合并 13），
  矿点表使用前按双 gcda OR 合并口径复核

### 2026-09-14 - 覆盖率批次 N：segment_downloader.cpp 76 → 11 miss + 死代码清理
- **删除匿名命名空间死函数** `generate_random_suffix`（零调用）
  与 `format_bytes`（唯一"引用"在注释里），22 miss 行出账；全
  包覆盖率（批次 C 同款 gcovr 口径）**行 81.0% / 函数 94.3% /
  分支 44.3%**（批次 M 80.8/94.1/44.2）
- segment_downloader.cpp gcov miss **76 → 11**：11 新用例——修
  复 ZeroFileSize 占位测试（从未调 start()）、start 门禁两态、
  等分策略（adaptive_sizing=false 全链首覆盖）、续传完成态两态
  （零下载直接合并 + 逐字节序校验）、暂停三循环（worker/监控
  线程/段内重试）、返回 false 但段恰好整段的 best-effort 收口、
  merge 闸门确定性篡改（等目标段完整落盘后再改——中途篡改会被
  重试自愈反而放行）、输出为已存在目录的 rename 失败、传输中
  cancel 收割存活监控线程
- 剩余 11 miss 全部定性：死防御 ×4（segments 恒非空/全段必完
  成/段入口完成检查/merge 临时文件）、30s worker 超时兜底 ×5
  （墙钟）、权限依赖 ×1（chmod 类，Windows 不兼容）
- 批次 N 时序教训：`num_connections=1` 经 calculate_optimal_
  segments 只产生 1 段（段数=连接数），多段场景先核段数

### 2026-09-14 - 覆盖率批次 M：cloud_storage_plugin.cpp 失败路由路径收敛 + WebCrawler 泄漏修复
- **修复 WebCrawler 析构泄漏**（ASan 实证 76 字节/4 处）：
  set_headers 保存的 `curl_slist* headers_` 在析构中从不释放，
  每个 GenericSearchProvider 构造（load_config 每引擎一个）泄
  漏整条请求头链；析构补 `curl_slist_free_all`
- 覆盖率批次 M：cloud_storage_plugin.cpp gcov miss **78 → 75**：
  3 新用例覆盖失败路由路径（蓝奏云 detect/extract 正则字符类差
  导致的无效链接短路、Google Drive docs 形态无 file id 的轻量
  基类分支、未知平台三循环落空"未找到对应的网盘插件"）
- 剩余 75 miss 全部定性：init_patterns 的 map initializer_list
  行归属伪影×12（执行序矛盾铁证：empty 检查 14045 次提前返回 ⇒
  map 非空 ⇒ 赋值必执行过）；五类接口四件套存根×55（管理器不暴
  露插件指针 + register_plugin/handle_share_link 生产零调用方，
  grep 全库实证）；6 平台 display_name 死代码（唯一调用点对其结
  构性不可达——detect/extract 正则字符类对齐）；第二循环已识别
  平台防御×2（默认插件 can_handle ≡ true，第一循环必命中）
- 全包覆盖率（批次 C 同款 gcovr 口径）：**行 80.8% / 函数
  94.1% / 分支 44.2%**（批次 L 80.7/94.0/44.0）；ctest 清单
  1919（1859 通过 + 60 既存 Skipped，零失败）；drives 139 用例
  cov + ASan 双绿

### 2026-09-14 - 覆盖率批次 L：task_storage.cpp 收敛 + initialize 死锁缺陷修复
- **修复 TaskStorage::initialize 死锁缺陷**（新测试曝光，strace
  铁证）：initialize() 入口持 `std::mutex`（不可重入），建表失败
  分支调 close()，close() 内部再次 lock 同一把锁 → 死锁。生产影
  响：**task db 损坏（非 SQLite 文件）时 daemon 启动永久挂死**而
  非优雅报错。修复：close() 去掉内部加锁（private 辅助，仅析构与
  已持锁的 initialize 流程两个调用点）
- 覆盖率批次 L：task_storage.cpp gcov miss **81 → 39**：8 新用例
  （storage 24 → 32，cov + ASan 双绿）覆盖 open 失败（不存在父目
  录）、坏库文件建表失败（256 字节垃圾）、completed_at 有值
  create/update 往返、显式 id 重复插入 step 失败（UNIQUE 冲突）、
  list limit+offset 分页（created_at 显式错开保证次序确定——同毫
  秒并列时 DESC 次序未定义）、cleanup_completed_tasks 全语义（过
  期删除/留存/幂等/未初始化 0）、move 构造与 move 赋值、get_last_
  error
- 剩余 39 miss 定性：sqlite prepare/step 失败防御×36、gcc 15 行
  归属伪影×3（664/666/670，同块尾行覆盖 + 字段断言通过双证）
- 全包覆盖率（gcovr 批次 C 同款口径）：**行 80.7% / 函数 94.0% /
  分支 44.0%**（批次 K 80.5/93.7/44.0）；全量 ctest 1916 零失败

### 2026-09-14 - 覆盖率批次 K：config_manager.cpp 认证门/主密码/导入导出边界收敛
- config_manager.cpp gcov miss **82 → 32**（行 92.01%）：8 用例
  （`config_manager_test.cpp` 18→26，cov + ASan 双绿）覆盖未初始
  化 manager 全操作拒绝（认证门 db_=nullptr 提前返回一径覆盖七个
  门行）、verify 全链（正确/错误/恢复）、master 表行删除、
  set_master_password 换密与弱密码拒绝、update 空 provider、导
  出导入边界（空密码/不存在文件/短文件/错 magic）、篡改 payload
  语义（mini-GCM 加密器构造生产对齐布局的导出文件：无 configs
  键/非 array/条目缺 name 跳过的部分导入语义）、库内密文截短
  （get 成功且解密失败字段空串）
- 测试构造手段：`exec_sql` sqlite3 直连篡改库内容、
  `mini_gcm_encrypt`（IV12+ct+tag16，key=SHA256(password)）构造
  任意语义的导出 payload（驱动 import 的深层解析分支）
- 两个真实语义发现（记录不修，未接线 API 的设计缺口，修复属特
  性开发）：① set_master_password 换密不重加密已存配置——旧密
  文解密失败恒空串（生产零调用方）；② verify missing-row 提前
  返回不撤销 authenticated_——已认证 manager 删行后写入仍放行
- ASan 曝出 resource_search.cpp 既有泄漏（WebCrawler::set_
  headers 的 headers_ slist 76 字节，排除本批用例依旧实证），留
  给 resource_search 批次
- 剩余 32 miss 全部定性：EVP crypto 失败防御×12（需注入）、
  sqlite prepare/exec/step 失败防御×15、initialize 空密码路径不
  可达×2、export encrypt 空返回×1、import 中 save 失败不可达×1
- 全包覆盖率（gcovr 批次 C 同款口径）：**行 80.5% / 函数 93.7% /
  分支 44.0%**（批次 J 80.3/93.4/43.8）；全量 ctest 1908 零失败

### 2026-09-14 - 覆盖率批次 J：dht_node.cpp Kademlia 查找边界与路由表纯单元收敛
- dht_node.cpp gcov miss **94 → 10**（行 97.3%）：16 新用例挂
  `falcon_protocols_tests`（dht_node_test.cpp 9 → 25，cov + ASan
  双绿）。纯单元簇直接构造公开类型：DhtUtils nodeIdFromString
  （大写 hex/非 hex 回退/非 40 长度补零截断）、DhtBucket 全 API
  （桶满替换 15 分钟不活跃最旧节点——lastSeen 回拨 16 分钟构造、
  替换缓存拒绝、inactive 记账、距离排序截断）、DhtRoutingTable 跨
  桶聚合 + 全零 id 桶钳位、DhtMessage Error 往返 + 非 dict 防御
- DhtClient 查找边界：未 start 客户端 socket==-1 快速终结不悬挂、
  α=3 单轮并发上限（第 4 近候选放行后才被查）、kMaxCandidates=64
  吸收截断（响应携 70 节点强制命中）、k=8 上报截断（9 响应者恰报
  8）、重复 id 两端点各查一次只报一次
- 测试设计要点：bootstrap 候选 id 全零距离排序退化——需确定距离
  序的用例先热身查找让响应把真实 id 写入路由表，第二阶段候选才
  确定有序（getRoutingTable 返回 const 不可直接注入）；迭代轮次
  冻结用响应门闩（responder 自旋等 atomic 门闩，观察到第 1 轮恰
  α=3 个查询后放行），零 sleep 依赖
- 剩余 10 miss 全部定性：socket() OOM×2、维护线程 5 分钟周期×2、
  recvfrom 错误竞态×2、并发防御×4（finalizeLookup 与
  pendingRequests_ 同锁同步清理，迟到响应/双重终结仅在超时
  finalize 与回调派发间微窗口可达，无确定性注入点）
- 全包覆盖率（gcovr 批次 C 同款口径）：**行 80.3% / 函数 93.4% /
  分支 43.8%**（批次 I 79.9/92.9/43.6）；全量 ctest 1900 零失败。
  口径警示：gcovr 显式位置参数 `.` 只扫单棵构建树（曾得 90.5% 虚
  高），留档命令无位置参数吃进 cov+asan 两树，铁账链为两树合并
  口径

### 2026-09-14 - 覆盖率批次 I：websocket_rpc_client.cpp 客户端协议栈边界收敛 + daemon 进 ASan
- websocket_rpc_client.cpp gcov miss **99 → 9**（行 97.6%）：18 新
  用例挂 `falcon_daemon_rpc_client_tests`（29 → 47）。新增可编程
  原始 WS 服务器 `RawWsServer` 测试基建（握手剧本 + on_connected
  控制帧注入 + on_request 应答脚本 + 客户端帧记录），与
  JsonRpcServer 回环互补，帧级行为完全由测试控制
- 收口六簇：便捷方法全簇（14 转发方法 params 归一形状服务器侧断
  言 + as_gid/expect_ok 解包防御变体）、call 失败路径（非数组
  params 归一、应答超时 -32000、无 result 无 error -32600、请求
  在途中断连 fail_pending 唤醒）、服务器控制帧（ping→pong 回帧、
  close 回应后收尾、binary/pong 忽略）、握手容错（半截头 EOF/
  非 101/错 Sec-WebSocket-Accept/.invalid 域）、set_url 运行期重
  定向（旧服务器下线后 call 仍成功 ⇒ 必连新端点的强断言）与
  parse_url 分支（IPv6 字面量/裸主机默认 path+端口/自定义 path
  请求行断言）
- 剩余 9 miss 定性：238-239/494-499 时序窗口不可测（TCP 刚建立
  首发失败无注入点；fd 失效与读线程收尾之间的竞态无可控时机）、
  520 不可达（slot->response 两条赋值路径均保证 object）
- **ASan 树首次纳入 daemon**：build-asan `FALCON_BUILD_DAEMON`
  OFF→ON，falcon_daemon_rpc_client_tests 47 用例 ASan+UBSan 零告
  警（WS 客户端多线程 socket 代码首次内存检查）
- 全包覆盖率（gcovr 批次 C 同款口径）：**行 79.9% / 函数 92.9% /
  分支 43.6%**（批次 H 79.6/92.1/43.2）；全量 ctest 1884 零失败
- 批次 J 候选（##### 铁账）：dht_node 94 / config_manager 82 /
  task_storage 81 / cloud_storage_plugin 78 / segment_downloader 77

### 2026-09-14 - 覆盖率批次 H：task_manager.cpp 状态持久化容错与事件转发层收敛
- task_manager.cpp gcov miss **75 → 13**（行 97.31%）：21 用例
  （`task_manager_edges_test.cpp`，cov + ASan 双绿）覆盖状态持久
  化容错全链（损坏状态文件逐字段截断解析防御、非法 id/空 URL/
  重复 id 跳过、越界优先级回落 + 活动状态净化为 Paused、options
  全字段含转义字符 save/load 往返）、auto-save 异步保存链三触发
  点（add/remove/cleanup 周期，stop 排空后断言落盘 + 重载校验）、
  调度防御（无 handler 任务启动即 Failed、stop 先取消活动下载、
  worker 出队过期条目静默丢弃）、事件注入转发层
  （on_task_status_changed/on_task_progress 经 EventDispatcher
  派发 + 活动计数进出）
- 剩余 13 miss 全部定性：9 行 gcc 15 行归属伪影（铁证：同一顺序
  执行块内后执行的行覆盖而先执行的不覆盖——如 561 覆盖而 560 不
  覆盖；round-trip 全字段断言通过即证明在执行）、4 行
  Impl::on_completed 不可达（全库零调用方，完成事件实际走
  on_status_changed 的 Completed 分支）
- 全包覆盖率（gcovr 批次 C 同款口径）：**行 79.6% / 函数 92.1% /
  分支 43.2%**（批次 G 79.2/91.5/43.1）；全量 ctest 1864（2 例
  DownloadEngineTest 并行抖动串行复跑即过，本次零生产代码改动）

### 2026-09-14 - 覆盖率批次 G：http_handler.cpp V1 curl 数据面回环测试 + 空指针缺陷修复
- **修复 HttpHandler::pause/resume/cancel 空指针崩溃**（新测试曝
  光）：`pause(nullptr)` 直接解引用，与 FtpHandler 同位置的
  `if (!task) return;` 防御不一致——三外层入口统一补防御（impl
  与 V2 转发共用）
- 覆盖率批次 G：http_handler.cpp gcov miss **80 → 16**（行
  96.2%）；全包（批次 C 同款 gcovr 口径）行 79.2% / 函数 91.5% /
  分支 43.1%（78.9/91.5/42.9 → 涨幅 0.3/0.0/0.2）。剩余缺口全
  部定性：curl init OOM×3、单连接 cancelled 标志防御（接口未暴
  露）、段文件打开失败、段续传截回（worker 超尺寸先删结构性不可
  达）、200-instead-of-206 纵深防御（现代 curl resume 守卫先
  拒）、else 行归属伪影、重试间隙毫秒竞态窗口
- 新文件 `http_handler_edges_test.cpp` 26 用例全量 ctest 全绿
  （1844，cov + ASan 双零告警）：自包含可编程 HTTP 服务器（应答
  剧本 + HEAD 探测末位放行 + Range 自动 206 切片 + 按 Range 差异
  化慢发 + 部分发送硬断连 + Range 撒谎 + 无 Content-Length EOF
  定界）——CD 引号/无引号 filename 解析、URL 推导、curl 选项传
  播（cookie 引擎往返/401 挑战 Basic 重放/必败代理）、REST 续
  传、500 重试与 404 快速失败、rename 失败、限速热应用、未知总
  长下载、分段端到端/段错误收口/传输中 pause-cancel 转发/暂停
  resume 重入/段短传重试续传/Range 撒谎分段失败干净
- 语义记录：download() 顶部 get_file_info（HEAD）先行——错误剧
  本必须在 HEAD 层放行否则直接炸掉；分段路径的段只看 downloader
  cancelled 标志不查 task 状态，中止必须经 handler 转发；
  SegmentDownloader 析构即清段文件（handler 层暂停不保留段断
  点）；resume 前置位 Downloading 是 TaskManager 职责

### 2026-09-14 - 覆盖率批次 F：ftp_plugin.cpp 135 → 3 miss（占位测试重写 + weak stub 链接陷阱）
- **缺口定性**：135/146 miss = 零真实测试——旧 ftp_handler_test.cpp
  55 个用例全是断言字符串字面量的占位测试，唯一真实的 registry 测
  试长期 GTEST_SKIP
- **weak stub 链接陷阱**（registry 0 注册根因，nm 实证）：core 的
  weak 空 stub 与 protocols 的真实实现分属两个对象；GNU ld 归档一
  次扫描下，不引用任何 protocols 符号的二进制（falcon_ftp_tests）
  真实实现对象从未拉入，空 stub 生效 → load_builtin_handlers() 0
  注册。daemon 因 RPC 引用 describe_builtin_protocols 强符号免疫。
  测试侧引用强符号收口，Skip 变真断言
- 测试基建：新 `mock_ftp_server.hpp`（storage 包 mock 复制 + RETR/
  REST/慢发/一次性失败扩展）；55 占位 → 20 真实用例（cov+ASan 双
  绿）：SIZE 探测语义（curl 对 SIZE 550 在 RETR 前即弃——命令序
  列 dump 实证）、端到端下载、REST 续传、瞬态失败重试（含 1s 退
  避下界）、重试耗尽、rename 失败不假报完成、慢发进度记账越过
  200ms 节流窗、暂停中止（CURLE_ABORTED_BY_CALLBACK）、proxy 凭据
  生效性
- 覆盖率批次 F 收口：ftp_plugin.cpp gcov miss **135 → 3**（行
  97.26%），剩余全为 OOM throw/write_callback 防御等不可测项；
  全包（批次 C 同款 gcovr 口径）行 78.9% / 函数 91.5% / 分支
  42.9%（78.3/90.9/42.6 → 涨幅 0.6/0.6/0.3 点）

### 2026-09-14 - 覆盖率批次 E：bittorrent_plugin.cpp 187 → 7 miss + 四缺陷修复
- **magnet infoHash off-by-one**：`"xt=urn:btih:"` 是 12 字符，旧
  代码 `pos + 11` 截取——magnet 任务 infoHash 恒带前导冒号（
  can_handle 与 download() 两处不一致即证据）。提取/归一化收口为
  公开 static `extract_info_hash`/`info_hash_to_hex`
- **Base32 magnet 不解码**：can_handle 接受 32 位 base32，
  download() 却把 base32 文本原样传 findPeers——必然查询错误
  info_hash；base32Decode 存在却从未被调用（死代码激活），查表改
  大小写不敏感（RFC 4648）
- **parseBencode 宽松解析**：截断输入静默返回假值、stoll 宽松接
  受空白/'+'、越界抛裸 out_of_range——get_file_info 纯模式
  .torrent 路径真实使用这套内嵌解析器。三处严格化
- **DHT 僵尸客户端**：DhtClient::start() 失败只记日志不抛异常，
  startDht 照常持有没在运行的客户端——isDhtRunning() 撒谎、查找
  无人驱动。startDht 检查 isRunning() 失败即 reset；新增
  clearDhtBootstrapNodes()
- 死代码删除 5 个零引用函数；测试 +32 用例（Base32 向量经 Python
  独立生成；DHT 随机端口 + 占口测冲突）；ASan BT 套件 118 用例零
  告警
- 覆盖率批次 E 收口：bittorrent_plugin.cpp gcov miss **187 → 7**
  （行 95.72%）；全包（批次 C 同款 gcovr 口径）行 78.3% / 函数
  90.9% / 分支 42.6%（77.3/89.8/42.1 → 涨幅 1.0/1.1/0.5 点）。
  测量教训：改源码后必须全量重建——未重建二进制内嵌旧 checksum
  对象会在全量 ctest 中整体替换 gcda（表现为全绿测试但覆盖数据
  被清掉）

### 2026-09-14 - 覆盖率批次 D：http_commands.cpp 真实缺口收敛 + chunked 分片/SIGPIPE 缺陷修复
- **chunked 分片错帧修复**：TCP 可把块大小行 CRLF 拆开送达，旧代码
  预消费 CR 进大小行——LF 与块数据被并进 size_str，stoul 在 '\r' 静
  默截断得错误块大小 → 数据错位。新增 `chunk_cr_pending_` 状态位，
  CR 在缓冲末尾置位等待下批补判 LF 绝不预消费；trailer 同法修复跨
  缓冲 CR 丢失（终止 CRLF 被分片时曾永不可见挂到 EOF 判截断），
  trailer 侧宽松（CR 后非 LF 不消费继续扫描）与大小行严格判定有意
  不对称
- **SIGPIPE 三层防护**（测试 SIGPIPE 暴露的生产缺陷）：引擎三处
  `send(...,0)` 无 MSG_NOSIGNAL，对端 RST 后写 socket 即杀死整个进
  程。引擎 POSIX send 统一 kSendFlags（MSG_NOSIGNAL；macOS socket
  级 SO_NOSIGPIPE）、CLI main 补 SIGPIPE SIG_IGN（daemon 既有）；
  顺带消除 base64 移位与 send/recv 长度参数的既有符号转换告警
- 覆盖率批次 D：http_commands.cpp gcov miss **256 → 176**（行
  87.95%），净收敛 80 行真实缺口；全包（批次 C 同款 gcovr 口径）行
  77.3% / 函数 89.8% / 分支 42.1%（76.8/89.8/41.8 → 涨幅 0.5 点含
  四云批次贡献）。剩余缺口：TLS 防御分支、send/recv 硬错误、
  resume 理论不可达、Windows 平台分支
- 测试 +30 用例全量 ctest 全绿：proxy 套件 4（连接应答跨分片重入/
  base64 填充向量/IPv6 authority 与斜杠 path 判 Unsupported）+ 新
  文件 `http_commands_edges_test.cpp` 26（编程式剧本服务器：传输中
  断 4/段失败收口 2/大流量让出 1/发布失败 1/重定向变体 5/续传调度
  3/chunked 边界 5/解析容错 1/TLS 与代理失败收口 5/域名失败 2）；
  ASan 引擎相关 194 用例零告警

### 2026-09-14 - 四云存储浏览器 endpoint 改造 + 缺陷修复 + mock 测试（OSS/COS/Kodo/Upyun）
- 四浏览器 endpoint（又拍云为 api_domain）配置此前全链路零消费——
  官方 virtual-host 域名 mock 不可行，配置了自定义 endpoint 也被
  无视（MinIO/私有化网关类部署不可用）。现携带 scheme（http:// 或
  https://）时走 path-style `endpoint[/bucket[-app_id]]/key`
  （Kodo 为 rs/rsf 同 endpoint 按路径区分，Upyun 无 bucket 前缀），
  否则保持官方域名；同步推广 S3 模板全套修复：真 HEAD（NOBODY 与
  CUSTOMREQUEST 互斥清设）、POSTFIELDS 恒设（handle 复用残留）、
  HEADERFUNCTION 响应头回传、状态码成功判定（200≤status<400）+
  ok 出参、encode_key 逐段编码保留 '/'
- **修复各 browser 特有缺陷**：① OSS/COS 列举 query_string 从未拼
  到请求 URL（只进签名）——prefix/max-keys 从未真正发到服务端；
  ② Kodo/Upyun 递归列举边遍历边向同一 vector 插入（扩容即悬垂
  迭代）——先收集/快照再插入；③ Upyun 递归删除双斜杠（子目录
  列举 path 已带前导 '/' 再拼 "/"）；④ get_resource_info 恒真
  条件（对象不存在也报"存在"）改按状态码判定
- **mock 测试首跑曝光三个额外真实缺陷**：① COS/OSS 签名 URI 用
  `find('/')+bucket.length()+N` 偏移算术提取——host/port 长度不同
  即把 authority 片段混进规范资源（签名恒错），COS 端口个位数时
  substr 越界抛 out_of_range，改为 scheme 后定位 path；② COS 签名
  头键小写化后 `at(小写键)` 查原大小写 map——带 Content-Type 的
  请求（建目录）必抛 map::at，改为插入时统一小写；③ Kodo
  base64url 输出带尾部换行拼进 URL——curl 报 "bad/illegal format"
  （stat 探测从未成功过），改 BIO_FLAGS_BASE64_NO_NL
- 测试基建：共享 `mock_http_server.hpp`（自 S3 测试提取，新增防
  RST 排空——POST 带请求体未读即 close 会以 RST 收场吞掉刚写出
  的响应，应答后半关闭写端 + SO_RCVTIMEO 排空读端再 close）；新增
  四个 browser mock 测试共 59 用例（endpoint path-style 连接断言/
  列举解析/递归/HEAD 信息头/exists 状态码语义/建目录/递归删除顺序
  /rename/配额/错误路径）；全量 ctest 1701 用例通过，ASan 下五
  browser 套件 81 用例零告警

### 2026-09-13 - 测试覆盖率专项（行 68.1% → 74.0%）+ S3 浏览器五缺陷修复 + PEX 重复回调
- 覆盖率从基线 68.1%/77.0%/36.2%（行/函数/分支）提升到 **74.0%/
  81.0%/39.4%**（gcovr，packages/ 范围排除 tests/），净增 1365 行
  覆盖；100% 行覆盖对本项目不可达（http_commands 平台分支与真实
  网络交互、cli main 入口、logger.hpp 为 gcc 15 行号漂移测量伪影
  、其余五个 browser 需各自 endpoint 改造），如实评估见 todo.md
- 新增 S3 浏览器 mock 测试 27 用例（`s3_browser_mock_test.cpp`，
  编程式 mock HTTP 服务器一连接一请求）：URL 解析/连接/列表（JSON
  解析+隐藏过滤+递归 CommonPrefixes）/HEAD 信息头解析/exists 状态
  码语义/建目录 marker/递归删除降序/复制改名/配额；测试基建踩坑
  记录——Linux close() 不唤醒阻塞在 accept() 的线程（strace 实证
  join 死等），停机必须先 shutdown(listen_fd, SHUT_RDWR) 再 close
  再 join；测试服务器绑 htonl(INADDR_ANY) + getsockname 取随机
  端口（本沙盒 INADDR_LOOPBACK 有字节序怪癖）
- **S3 浏览器五项产品缺陷修复**（`s3_browser.cpp`，mock 测试曝光）：
  ① endpoint 配置全链路零消费（MinIO 等 S3 兼容服务不可用），现
  endpoint 优先生效 path-style `endpoint/bucket/key`；② 对象 key
  的 '/' 被 url_encode 编成 %2F 导致子目录 key 必然 404，新增
  encode_key 逐段编码保留 '/'；③ HEAD 语义错误——
  CURLOPT_CUSTOMREQUEST 只改请求行方法字符串响应仍按 GET 处理，
  真 HEAD 须 CURLOPT_NOBODY 且与 CUSTOMREQUEST 互斥清设（handle
  复用时残留覆盖方法切换，curl 实报 "Weird server reply"）；④
  get_resource_info 解析从不存在的 body（HEAD 无体，info 恒空）
  ——响应头经 HEADERFUNCTION 回传填充；⑤ 成功判定看 body 非空
  （S3 写操作成功常为 204 无 body），改状态码判定（200≤status<
  400）+ ok 出参
- **PEX 重复发现回调修复**：handlePexMessage Add 分支对已存在
  peer 重复触发 onPeerDiscovered_，改为仅新插入候选集才触发
- 新增 PEX 25 用例（编解码往返/字节布局/握手分发/去重/回调/异常
  输入）、bencode 边界 ~12 用例（深层嵌套/空容器/非最小整数/截断
  ）、BT 解析 ~10 用例；手写 bencode 测试数据的长度前缀须用解析
  器逐字符验证（`3:aa` 类错误高频）
- ASan 构建重配 `FALCON_ENABLE_BITTORRENT=ON`（此前 OFF 导致 BT
  测试整个不编；纯 C++ 模式无需 libtorrent），BT 86 用例 + S3 27
  用例零告警；全量 ctest 1629 用例仅 DaemonModeLifecycle 一例并行
  抖动（串行复跑即过）

### 2026-09-13 - DHT Kademlia 迭代查找（异步回调接线 + 三个既有缺陷修复）
- 完成 todo 未完成事项 #2/#3：`findPeers/findNode` 此前的回调参数从未
  接线（`pendingRequests_` 只读不写、永远为空），`performLookup` 仅对
  最接近的 8 个节点单轮 fire-and-forget——DHT 查找自集成以来从未真正
  返回过结果
- `LookupContext` 替代从未使用的死结构 `DhtLookupRequest`：候选集/
  已查询/待响应/已发现 peers/已响应节点 + 回调，按 lookupId 管理，
  并发查找互不干扰；已查询/待响应以端点（ip:port）为键而非节点 ID
  （引导节点 ID 未知，响应带回真实 ID 不得导致重复查询）
- 迭代驱动闭环：`continueLookup` 每轮向最近未查询候选（α=3）发查询
  并注册事务回调；响应吸收 compact nodes/values 后继续下一轮；候选
  耗尽且全部响应收齐 → `finalizeLookup` 收敛（peers 一次性上报、
  node 回调对最近已响应节点 ≤k 逐个触发）；超时（默认 30s，可配）
  上报部分结果；回调一律锁外执行（回调内再取 mutex_ 会死锁）
- **修复公网引导节点黑洞**（异步查找从未工作的深层原因之一）：预置的
  router.bittorrent.com 等域名从不做 DNS 解析，`inet_pton` 失败后
  `sin_addr=0` 静默发往 0.0.0.0，查询永远挂 outstanding → 查找永不
  收敛。`sendMessage` 返回 bool + `noteUnreachable`（发送失败按已
  终结处理，查找立即收敛而非悬挂到超时）；新增
  `clear_bootstrap_nodes()`（纯私有网络/测试场景）
- **修复 `nodeIdFromString` hex 解码**：原为 memcpy 截断而非 hex 解码
  ——40 位 hex info_hash 与消息 decode 的 id 字段全部解析错误
  （encode/decode 不对称）；合法 40 位 hex 才解码否则回退字节截断，
  与 `nodeIdToString` 构成往返
- **修复 get_peers 协议格式**：`info_hash` 参数从 40 字符 hex 文本改
  为 20 字节原始值（BEP-005）；compact nodes/values 解析提取为共享
  辅助函数（原两处重复实现合一）
- 新增 `dht_node_test.cpp` 9 用例（本地 UDP mock DHT 网络）：两跳迭代
  逼近、find_node 距离序上报且收敛后无多余查询、空网络空结果、超时
  终结不悬挂、并发查找独立、无效端点/不可解析引导快速终结；DHT 9
  用例全绿，全量 ctest 零回归

### 2026-09-13 - V2 引擎接入生产接线（M3：daemon/CLI 配置面 + 停机与恢复缺陷三连修）
- 三生产二进制接线完成，默认 `v1` 全关（灰度开关逐任务可回退 curl）：
  - daemon：daemon.json `download.http_engine: "v1"|"v2"`（非法值告警
    忽略保持现值）+ `--http-engine` flag（非法值报错退出）；启动接线
    在任务恢复**之前**（恢复的 Downloading 任务 start_task 即进下载
    路径）；drain_engine 尾部 `V2EngineHost::shutdown_and_join()`（V1
    pause_all → V2 组 PAUSED 固化断点 → 桥接返回 → 收引擎）；SIGHUP
    重载遇开关变化告警 "restart required"（V2 稀疏临时文件与 V1 前缀
    续传布局不兼容，翻转开关必须在停机窗口）
  - CLI：`--http-engine` flag + 校验；`shutdown_and_join()` 在摘要
    生成前（任务全部终态/暂停后桥接已返回）
  - desktop：InProcessBackend 构造读 `FALCON_HTTP_ENGINE` 环境变量
    兜底、析构收引擎（本机无 Qt6，随 CI Qt6 job 编译验证）
- **修复停机 SIGSEGV**（V2 暂停任务后 SIGTERM，"Daemon stopped" 打印
  后 main 局部对象析构阶段崩；strace -k 栈回溯定位）：EventDispatcher
  以裸指针持有监听者且 worker 线程存活到引擎析构，监听者对象先死而
  引擎后死时，引擎停机派发的尾部事件回调悬垂指针（虚调用读已释放
  vptr）。三处修复：daemon/CLI 注册点加 RAII ListenerDetacher（先于
  监听者析构摘除，覆盖全部退出路径）；EventDispatcher 析构先
  clear_listeners 再 stop（clear 与在途回调互斥——纵深防御，只覆盖
  「监听者比 dispatcher 长寿」的顺序）。V1 时代窗口从未命中，V2 停机
  事件密度（pause→PAUSED→桥接收尾）放大了它
- **修复重启恢复任务 id 错位**（潜伏既有缺陷，V2 E2E 首次踩中）：
  daemon 启动恢复走 `add_task` 重新分配 id，而 RPC 按持久化记录的
  gid 寻址——重启后 pause/unpause/remove 全部 "Task not found"
  （tellStatus 有 storage 回落而显示正常，掩盖了引擎内无任务）。
  DownloadEngine 新增 `add_task_as_id(id, url, options)`（id 冲突返回
  nullptr；CAS 语义推高计数器绝不回退），恢复循环改用原 id 进引擎
- **修复恢复任务 Paused 状态未还原**：add_task 重建的任务是初始
  Pending，`resume_task` 按 Paused 判定 → unpause 恒 "Resume failed"；
  恢复循环对 Paused 记录显式 `set_status(Paused)`
- **修复适配层终态进度同步缺口**（M2 既有缺陷）：桥接 200ms 轮询粒度
  下最后一次进度可能落在终态分支，快任务完成时 V1 task total/
  downloaded 恒 0（tellStatus 报 0/0+complete）——`sync_final_progress`
  在 COMPLETED/FAILED/PAUSED 三终态分支前同步组内进度
- 测试：config 3 新用例（http_engine 解析/非法值告警忽略/默认值）；
  main_integration 新增真实二进制 E2E 两用例（自含 RangeFileServer：
  HEAD 探测 + Range 双边界忠实解析 206——Range 撒谎会被 M1 防护拒绝）：
  4MB 多段下载完成逐字节一致 + 无临时/控制文件残留；2MB 慢发暂停 →
  SIGTERM 排水 → 同 DB 重启 → unpause → 断点续传完成逐字节一致 +
  两次停机 exit 0。全量 ctest 1028 通过（6 预存在跳过），ASan V2/HTTP
  相关用例零告警，CLI `--http-engine v2` 冒烟成品逐字节一致

### 2026-09-13 - V2 适配层与进程开关（V2EngineHost + V2HttpDownloadAdapter，默认关）
- V2 引擎以 V1 契约下的 HTTP 数据面接入生产的适配层落地（M2）：
  V1 引擎/TaskManager/事件/持久化全不动，`HttpHandler::download()`
  开头按进程级开关分叉——`v2_http_enabled()`（默认 false）且
  options 无 curl 专属能力时桥接到共享 V2 引擎，逐任务可回退
  curl；pause/cancel 先置 V1 状态再转发引擎（幂等），resume 无需
  分支（V1 resume 即重新 download()，桥接层续跑 PAUSED 组）
- `V2EngineHost`（进程单例，v2_engine_host.{hpp,cpp}）：engine()
  惰性启动共享引擎 + 专用 run 线程；`configure()` 引擎启动前置配
  置（幂等，启动后告警忽略）；`shutdown_and_join()` 排水停机——
  pause_all → 轮询全部组安顿（新增 `RequestGroupMan::all_settled`
  判据，PAUSED 视为已安顿）→ shutdown → join run 线程；halt 后的
  引擎不可复用，实例销毁待重建；宿主强制 wait_when_idle=true +
  max_concurrent_tasks 抬升至 ≥64（V1 TaskManager 权威排队，V2
  侧不二次排队）
- 修复引擎实例重建即停机的挂死：run() 入口复位 halt_requested_
  （实例复用语义），「创建后立即 shutdown」的停机请求先于 run
  线程入口到达会被吞掉 → run 线程永久轮询、join 挂死；engine()
  现在等待线程真正进入 run()（新增 `is_running()`，running_ 顺势
  原子化——add_download 路径本就跨线程宽松读）才返回
- `V2HttpDownloadAdapter`（plugins/http）：`supports()` 回退表——
  socks/HTTPS 代理（parse_http_proxy 判 Unsupported）、cookie
  引擎（CURLOPT_COOKIEFILE/JAR）、HTTP 401 认证、Referer 头
  （防盗链语义 V2 不发送）一律回退 V1；`run()` 保持 V1 worker
  线程阻塞语义（可重入）：HEAD 探测照旧（on_file_info 先于
  on_progress）→ find_group：PAUSED 组 resume_task 续跑，无组则
  add_download_as 注入（V1 id + 已确定 output_path，两侧写同一
  文件）→ 桥接轮询（200ms）：组状态为主判据，COMPLETED →
  set_status(Completed)，FAILED → throw（V1 worker catch 统一
  set_error+Failed，事件序列与 V1 逐一对齐），组侧 PAUSED → V1
  状态对齐后挂起，V1 侧 pause/cancel 优先转发
- 新增 v2_http_adapter_test.cpp 12 用例：`WithParamInterface<bool>`
  参数化 V1/V2 等价对照（worker 收口路径忠实复刻 task_manager）——
  下载完成+事件顺序（file_info 先于 progress、终态回调）/ 404
  错误传播 / 多分段 / 慢发暂停→恢复（V2 侧含排水停机 → 引擎重建
  → 控制文件断点续传）/ 取消，两侧成品逐字节一致；supports 回退
  表（含 proxy_type 覆盖指向 socks、http 代理带认证放行）；宿主
  生命周期（惰性启动/配置生效/停机销毁/重建新实例）；全量 1535
  ctest 通过，ASan 适配层+引擎相关 131 用例零告警
- M3 待接：daemon.json `download.http_engine` / CLI
  `--http-engine` / desktop 环境变量 → set_v2_http_enabled +
  configure；daemon drain 尾部 shutdown_and_join（停机窗口硬要求
  ——V2 稀疏临时文件与 V1 前缀续传布局不兼容，跨引擎混杂恢复会
  写花数据）

### 2026-09-13 - V2 引擎 HTTP 代理支持（absolute-form 请求行 + CONNECT 隧道）
- V2 数据面此前无法穿透代理（aria2 生产部署的常见网络形态）；补齐
  明文 HTTP 代理全路径，socks/TLS 代理明确判 Unsupported（M2 适配层
  据此回退 V1 curl，libcurl 自带 socks 支持）——不静默直连
- `parse_http_proxy(options)` 纯函数（暴露便于单测）：`http://[
  user:pass@]host[:port]` 与无 scheme 的 authority（按明文代理，缺省
  端口 80，curl 同语义）；userinfo 凭据优先、缺省回落
  proxy_username/proxy_password 字段；socks4/5/https/未知 scheme/
  proxy_type 含 socks/非法端口一律 Unsupported；IPv6 字面量代理随
  V2 的 AF_INET 数据面一并判 Unsupported
- 代理分叉在 connect_socket：代理生效时连接代理服务器（目标主机名
  解析延迟到隧道建立之后——absolute-form/CONNECT 语义下目标解析
  本就归代理）；分段的续传/重试/跨段连接经 options_ 值拷贝自然贯通
- 明文 HTTP 经代理：请求行 absolute-form（RFC 7230 §5.3.2）+ 
  Proxy-Authorization: Basic（本地 15 行 RFC 4648 base64）；
  `HttpRequest::to_string` 仅在 target 无 `"://"` 时补前导 `/`，
  absolute-form 原样透传
- HTTPS 经代理：CONNECT 隧道（RFC 7231 §4.3.6 authority-form target，
  认证同源）——新 `PROXY_TUNNEL_SEND/RECV` 状态非阻塞推进（发完
  CONNECT 注册 READ 等代理最终应答，收满 `\r\n\r\n` 判 2xx），隧道
  建立后同连接切 TLS_HANDSHAKING——既有异步握手/证书校验/SNI 零
  改动照常生效；隧道内请求回 origin-form 且不再发 Proxy-
  Authorization（凭据只交代理，不向目标泄漏）；非 2xx/代理提前断连
  走初始连接失败收口（make_connection_retry → 终态）
- request_group 门禁：Unsupported 代理组 FAILED + 明确错误消息
  （语义性失败不重试）
- 新增 http_commands_proxy_test.cpp 15 用例（parse 表驱动 11 + 端到
  端 4：ProxyTestServer 三模式——明文代理假实现记录请求形态后直接
  应答、CONNECT 接受后原地 SSL_accept 变身 TLS 服务器、CONNECT 拒
  绝 403）：absolute-form 请求行 + 上游 .invalid 保留域不可解析而
  任务成功（客户端从未触碰上游解析）/ Basic 凭据精确到达（预计算
  RFC 向量）/ CONNECT 隧道 + authority 断言 + 隧道内 origin-form +
  正向证书校验 / 403 干净失败；证书生成函数平移至测试共享头
  tls_cert_generator.hpp（TLS 与 proxy 测试共用）；全量 1524 ctest
  通过，ASan 引擎相关 36 用例零告警

### 2026-09-13 - V2 引擎 HTTPS 放行与加固（异步 TLS 握手 + 证书校验硬断连）
- 此前 V2 引擎 https:// 被 init 门禁直接拒绝，TLS 基建以半成品形态
  沉睡：非阻塞 socket 上 SSL_connect 的 WANT_* 直接判失败（生产连
  接几乎必然非阻塞即挂）、证书校验失败仅 WARN 后照常收数据（TLS
  形同虚设）、响应命令以 `void* ssl_conn_` 裸指针跨命令持有会话
  （初始连接命令调度响应后随即出队销毁并 SSL_free——悬垂）
- 异步握手闭环：`setup_tls()` 返回 `TlsHandshakeResult{OK, WANT_READ,
  WANT_WRITE, FAILED}`；WANT_* 置新 `HttpConnectionState::TLS_
  HANDSHAKING` 并按所需方向注册 socket 事件重入续推（SSL_connect
  首调必然 WANT_READ——ClientHello 刚写出，重入路径天然覆盖）；
  SSL 对象经 `tls_started_` 守卫只建一次；execute 以 DISCONNECTED
  →CONNECTING→TLS_HANDSHAKING 三 case fallthrough 统一收口，请求
  发送单一出口（消除同步/异步两份重复块）
- TLS 会话共享所有权贯通命令链（`HttpTlsSessionPtr = shared_ptr
  <SSL>`，Initiate→Response→Download 构造链传递，四个下载命令创建
  点全部接线）：修复下载体阶段原为明文 recv() 读到密文的缺陷（SSL
  对象必须沿命令链到达下载命令），并消除裸指针悬垂；初始连接析构
  不再抢先 SSL_free
- verify_ssl 硬化：`SSL_set1_host` 绑定期望主机名（SSL_get_verify_
  result 结论同时覆盖证书链与主机名）；校验失败握手即中止硬断连，
  错误路径优先给出 X509 verify 结论
- SSL_read EOF 语义映射：ZERO_RETURN（close_notify 干净关闭）/
  SYSCALL（底层断连）→ n=0 交给既有截断判定（chunked 必须见终止
  块、Content-Length 必须收满）；读路径 WANT_WRITE 同样按写方向
  注册事件（注册错方向会挂死）
- request_group https:// 门禁在 OpenSSL 构建下放行（无 OpenSSL 保持
  明确报错）；重定向到 https 目标仍明确拒绝（直接 https 已可达，
  跟随跳转后续放开）
- 新增 http_commands_tls_test.cpp 3 用例（运行时自签证书：EVP_PKEY_
  keygen 便携 API 零弃用告警、CA:TRUE + SAN DNS:localhost/IP:127.0.0.1
  ；TlsTestServer 阻塞 SSL_accept + 发完即关不等待 close_notify，
  客户端按 Content-Length 判完成）：异步握手下载成品逐字节一致 /
  verify_ssl=true 自签必 FAILED（回归 WARN-only）/ SNI 服务器侧
  观测 + SSL_CERT_FILE 信任自签的正向校验成功；全量 1509 ctest
  通过，ASan 引擎相关 26 用例零告警；CI run 34767471583（M1.6
  宿主化前置 + Windows CRT 兜底）8 job 全绿

### 2026-09-13 - V2 引擎重定向跟随（Location 解析 + 命令链接力 + 超链防护）
- `HttpResponseCommand::handle_redirect` 是半成品：构造了跟随命令
  却从不入队（孤儿），execute 遇 3xx 直接 fail——V2 无法下载任何
  经重定向的 URL（CDN/短链/规范化跳转全挂）
- 补全跟随链路：Location 经 RFC 3986 §5 引用解析为绝对 URL——
  绝对 URL / 协议相对 `//host/path` / 绝对路径 `/path` / 相对路径
  （基于当前请求目录 + `.`/`..` 段归一化，query/fragment 先剥除）
  四形态全覆盖，解析失败（空 Location/无 authority）按失败收口；
  仅 RFC 7231 明确可跟随的 301/302/303/307/308 跟随，304 等其他
  3xx 按失败处理
- 深度沿命令链传递：响应命令 → 跟随的连接命令 → 下一响应命令，
  超过 kMaxRedirects=5 按失败收口（重定向环有界，绝不多打一次
  连接）；https 目标在 V2 放行 TLS（M1.1）前明确报错，不静默
  崩进 init 门禁
- 旧实现两处隐患一并清除：相对 Location 拼接用 `redirect_url_[0]`
  无空串防御（越界读）；基准 URL 取自 `http_request_->url()` 而
  响应命令不保证持有请求对象——统一改用值持有的 `source_url_`
- 新增 http_commands_redirect_test.cpp 4 用例（RedirectServer 路由
  表 + 每 path 命中计数）：302→307 多跳链成品逐字节一致（绝对 +
  绝对路径混合）/ 相对 `../up/rel.bin` 归一化命中 `/a/up/rel.bin` /
  自引用环在 6 次连接内失败收口（有界断言）/ https 目标干净失败；
  全量 1505 ctest 通过，ASan 引擎相关 163 用例零告警

### 2026-09-13 - V2 引擎 chunked 响应激活（Transfer-Encoding 端到端生效）
- 完整的分块解码状态机（READ_SIZE → READ_DATA → READ_CR → READ_LF
  → READ_TRAILER）自 2026-05 起就存在，但 `chunked_encoding_` 全库
  无置位点——响应头解析不认 transfer-encoding，chunked 响应按
  Content-Length=0 + EOF=完成处理，净载荷混着块协议杂质落盘且
  截断被当完成（死代码激活缺口）
- 激活链路：parse_header_line 解析 `transfer-encoding` 置
  is_chunked_response_（值可能携带逗号分隔编码链，含 chunked 即
  命中）；headers 循环**结束后**统一置零 content_length_ /
  accepts_range_ / supports_resume_——头序不定逐行置零会被后续
  content-length 覆盖；总长未知同时让分段/续传门禁自然失活
  （无 Range 请求、不建续传追踪）
- HttpDownloadCommand 构造尾参 `bool chunked`，仅单连接全新下载
  调度点传入（多段段 0 / 续传 / 跨段连接路径 chunked 到不了——
  门禁已挡）；捎带的首批 body 字节（initial_data_）在 chunked 时
  同样过状态机解帧，直写会把块大小行写进文件
- 截断语义收紧：分块响应终止块未到先断连即 FAILED——总长未知下
  EOF 不构成完成证据（旧行为 n==0 且 length==0 直接 download_
  complete=true，半截数据假报完成）；正常完成仍由状态机的终止块
  判定驱动，服务器发完即关连接不受影响
- 新增 http_commands_chunked_test.cpp 3 用例（ChunkedServer 自行
  编码块行、5KB 非整块边界压粘包/半包）：96KB 净载荷逐字节一致
  且无控制文件残留 / RFC 7230 冲突场景（伪造 Content-Length=1KB
  实发 48KB 不截断不判败）/ 发 8 块后断连必 FAILED 且半成品不顶
  最终名；全量 1501 ctest 通过，ASan 引擎相关 37 用例零告警

### 2026-09-13 - V2 引擎真暂停（入口守卫 + 暂停清扫 + 状态守卫三位一体）
- 此前 pause_group 只改组状态，命令照常执行/挂起——数据流继续走、
  连接保持到自然结束，"暂停"名不副实；且存在既有死代码缺陷：
  pause_group 先 set_status(PAUSED) 再调 RequestGroup::pause()，
  后者仅在 ACTIVE 态执行副作用，检查恒假——断点固化与内部任务
  暂停从未执行过
- 三位一体收口（暂停语义 = 冻结数据流 + 收走连接 + 固化断点）：
  ① 三命令 execute 入口 PAUSED 守卫（Initiate/Response/Download/
  Retry 四处）：已暂停的命令不再推进——下载命令冲刷残留缓冲 +
  上报断点 + 关 fd 后静默退出（非失败语义），Retry 不再续建重试
  链；② 新 HttpPauseSweepCommand → DownloadEngineV2::
  sweep_task_connections(task)：pause_task 成功后投递，引擎线程内
  收走挂起等事件的命令（不经 execute，入口守卫覆盖不到）——锁内
  收集命令所有权并清四表，锁外摘事件监听、关 fd；③
  fail_group_of_command 顶部状态守卫：PAUSED/REMOVED/COMPLETED
  直接 return，堵"暂停与清扫之间竞态窗口内的超时清理把非失败
  语义改写成 FAILED"
- 命令清扫检查点 prepare_sweep() 虚函数（基类默认无操作，下载
  命令 override）：冲刷写缓冲 + report_segment_flushed + 
  save_resume_now——析构路径只冲刷不上报（命令可能比引擎后销毁，
  悬垂指针风险），清扫在引擎线程内可安全触达任务组，必须补上报
  才能固化断点；pause 顺序修复后 RequestGroup::pause() 的
  save_resume_now 也在暂停瞬间生效（终态组拒绝暂停、已暂停幂等）
- 恢复闭环复用断点续传链路：resume_group → 组回 WAITING → 重新
  激活时 create_initial_command 携带断点 Range + If-Range → 206
  校验 → 从暂停时落盘进度继续（init 有 download_task_ 幂等守卫，
  内存断点不丢）
- 新增 download_engine_v2_pause_test.cpp 3 用例（PauseTestServer：
  0 号连接分块慢发留暂停窗口、send 带 MSG_NOSIGNAL 防客户端断开
  触发 SIGPIPE 杀测试进程；客户端断开作为 sweep 生效的服务器侧
  证据）：传输中途暂停字节冻结 + sweep 收走连接 + 断点 = 暂停时
  落盘进度 + 恢复 206 续传成品一致 / 跨任务超时周期 PAUSED 不被
  误杀（sweep 漏收或守卫缺失任一失守即红）/ 幂等 + 终态拒绝；
  全量 1498 ctest 通过，ASan 引擎相关 145 用例零告警

### 2026-09-13 - V2 引擎宿主化前置（wait_when_idle 常驻 + 显式 ID 注入 + 终态组回收）
- V2 接入生产（作 V1 契约下的 HTTP 数据面）的三块地基，默认行为零变化：
  ① `EngineConfigV2::wait_when_idle`（默认 false 完全保留测试语义）：
  true 时 `run()` 在全部任务终态后不再退出、持续轮询直到显式
  shutdown——共享常驻引擎以专用线程驱动 run()，必须跨任务存活
  ② `add_download_as(id, urls, options, output_path_override = {})`：
  桥接层注入 V1 引擎已分配的任务 ID（两侧任务对齐寻址）；ID 冲突
  返回 INVALID_TASK_ID 不创建组；`output_path_override` 非空时
  init() 覆盖按 URL 自推导的输出路径（覆盖门禁按最终路径检查）——
  V1 任务的 output_path 已确定，两侧必须写同一文件
  ③ `RequestGroupMan::purge_finished_groups()`：回收终态组
  （COMPLETED/FAILED/REMOVED 从 all_groups_/group_map_ erase，调度
  队列防御性清理，对象锁外析构）；run() 循环每 10s 周期调用（周期
  从 run() 起算，防时钟纪元默认值导致首轮立即触发）——常驻引擎的
  终态组不再随 run() 退出销毁，不回收则组表无界增长；PAUSED 组是
  停机恢复挂点，明确不在回收之列
- ID 计数器从函数局部 static 提升为文件级共享：注入侧把计数器推到
  注入 ID 之上（含冲突路径），自动分配不再撞上外部占用的 ID（两处
  函数局部 static 本互不相干，仅改注入侧无法约束自动分配）
- 新增 5 用例：purge 回收终态/保留 WAITING+PAUSED（waiting_count
  复核队列无悬垂）、wait_when_idle 无任务不退 + shutdown 退、显式
  ID 注入/同 ID 冲突拒绝/自动分配让路/空列表拒绝、override 路径
  端到端落盘（组写注入路径且成品字节一致）；全量 1496 ctest 通过，
  ASan 引擎相关 41 用例零告警

### 2026-09-13 - Windows 桌面包 Qt 插件搜索修复（qt.conf 缺失 + CI 产物裸 exe）
- 用户实测解压 nightly Windows zip 运行报 `Could not find the Qt
  platform plugin "windows"`：Qt6Core/Gui DLL 都加载成功才走到平台
  插件搜索，排除缺 DLL——根因是插件被归到 `plugins/` 子目录而
  Qt 运行时只自动搜 `<exe 目录>/platforms/`（windeployqt 布局，
  applicationDirPath 作为兜底库路径只补这一层），`plugins/` 布局
  没有 qt.conf 指引必挂
- nightly 打包补写 `qt.conf`（`[Paths] Plugins = plugins`，相对
  qt.conf 所在目录解析），并把 qt.conf 加入打包后关键文件校验清单
- CI（cmake-multi-platform）Windows 产物同场修复：此前只上传裸
  falcon-desktop.exe（连 Qt DLL 都没有，用户机器直接缺 DLL 报错）；
  新增 Package (Windows) 步骤与 nightly 对齐——exe/daemon +
  vcpkg 依赖 DLL（manifest 树优先、经典树兜底）+ Qt 插件四类 +
  qt.conf + MSVC CRT（免装 VC++ Redistributable），整目录上传
- 顺带发现 desktop CMakeLists 的 vcpkg DLL 复制为 `copy_if_different
  *.dll` 字面量（CMake -E 不做 glob，`|| cd .` 吞错静默无效），
  workflow 侧自拷贝绕开，CMake 文件不动
- nightly 的 workflow_dispatch 触发会连带发布 Release，不在 CI 轮
  里验证；qt.conf 修复随下次 scheduled nightly 生效，CMake Build 的
  Package (Windows) 步骤随本次推送验证
- 手动 nightly 验证发现第二层：CRT 拷贝通配 `2022\*` 在 windows-
  latest 上不匹配（VS 目录布局漂移），失败仅 WARNING 静默产出缺
  vcruntime140.dll 的残包；放宽为 `*\*\VC\Redist` 双 ProgramFiles
  候选 + System32 兜底（装过 VS 的机器系统目录必有同版本 DLL），
  并把 vcruntime140.dll 加进打包后关键文件校验（缺了让 job 失败，
  不再静默出残包）

### 2026-09-13 - V2 引擎断点续传闭环（.falcon.ctrl 控制文件 + If-Range 内容变更防护）
- V2 此前失败/中断即进度归零：多段模式所有段位置写入同一临时文件，
  段文件无从区分哪些区间有效（有洞即零）；补齐持久化断点——
  `<最终名>.falcon.ctrl` 行式控制文件（aria2 `.aria2` 同思路）记录
  url/total/etag/last_modified + `seg=<idx> <offset> <length>
  <downloaded>` 各段断点，原子写（tmp+rename），加载端严格校验
  （魔数/段序连续/计划恰好覆盖 [0,total)/进度不越段界）
- 落盘进度记账：写路径直写成功与缓冲冲刷成功两处上报（单调取 max
  防乱序，1s 节流写盘）；失败收口（段错误/组终态/超时清理/暂停）
  全部强制保存兜底；完成发布成品即删控制文件。析构路径不 report
  （命令可能比引擎后销毁，悬垂指针风险；有界丢失最多一个写缓冲，
  保守重下安全）
- 恢复流：init() 加载控制文件 → 严格校验（URL 一致 + 临时文件尺寸
  与断点吻合（只统计有进度段的 offset+downloaded 做下界——零进度
  段的 offset 不构成约束）+ sum<total）→ 初始连接携带第一个未完成
  段的 Range + If-Range（ETag 优先）→ 段 0 预置断点继续收尾，其余
  段各自续传连接；跨会话恢复与同进程连接级重试共用一套分支
  （HttpRetryCommand 重发时原样携带续传范围）
- If-Range 内容变更防护（RFC 7233）：续传响应必须 206 且长度/起点
  与请求一致；资源已变更回 200 或 Range 被无视时校验失败 →
  abandon（删控制文件）→ 重新发起无 Range 全新下载——断点数据
  绝不接续新内容，成品不可能混合新旧
- 顺带修复三处既有缺陷：① 续传的段 0 此前会以 trunc 打开临时文件
  （截断即销毁全部断点数据）且首段直写从位置 0 覆盖（不 seekp）——
  新增 truncate_output 构造参数区分全新/续传，写位置统一无条件
  seekp(offset+已落盘)；② 文件打开失败分支静默置 FAILED 无任何
  日志（多段段间创建竞态无从排查），统一收口 fail_group_on_segment_
  error 并补错误日志；③ 多段任务不参与连接级重试的既有语义使续传
  分支天然免于重复 begin_multi_segment（幂等守卫兜底）
- 门禁：resume_enabled=false（对照）/ overwrite_existing=true（显式
  覆盖=要求重下）/ 控制文件损坏 / 临时文件缺失，均回退全新下载且
  清理失效挂点；总长未知（chunked）不建立追踪
- 新增 9 用例（download_engine_v2_resume_test.cpp，自含
  ResumeTestServer 按 Range 特征应答 206/200 并记录断言）：单连接/
  多分段续传端到端（成品逐字节一致）、内容变更放弃续传转全新、
  Range 撒谎防护、resume 关闭/控制文件损坏/临时文件缺失/覆盖授权
  四条回退对照、控制文件 save/load 往返+严格校验单测；全量 1491
  ctest 通过，ASan 引擎相关 39 用例零告警

### 2026-09-13 - Nightly Linux AppImage 打包修复（Qt 模块检测在 vcpkg 布局下失效）
- linuxdeploy-plugin-qt 的模块自动检测机制：对 AppDir 内 ELF 跑 ldd，
  按路径前缀匹配 qmake 报告的 QT_INSTALL_LIBS 计模块数。Qt 经 vcpkg
  安装时该机制不可用——qmake 报告 `tools/Qt6/lib`，实际链接的 Qt 库
  却从 `<triplet>/lib` 解析，前缀永不一致 → `Found Qt modules:` 恒
  为空 → `Could not find Qt modules to deploy`（Nightly Linux Package
  连续两轮失败的根因；非调用顺序问题，合并调用也不解决）
- 修复：`EXTRA_QT_MODULES="core;gui;widgets;network;concurrent"` 显式
  声明（分号分隔，与 apps/desktop 链接的 Qt6 组件一致）跳过自动检测；
  顺带把 `--plugin qt` 与 `--output appimage` 合并为单次 linuxdeploy
  调用（官方推荐用法）
- 验证 run 暴露第二层缺陷：vcpkg qtbase 默认不构建 xcb 平台插件
  （`platforms/libqxcb.so`），qt 插件部署 gui 模块的平台插件时报
  `Cannot deploy non-existing library file`——没有它 AppImage 即使
  打包成功在用户桌面上也起不来。修复：vcpkg.json desktop feature 的
  qtbase 增加 `"xcb"`（所需系统 X11 开发包 Ubuntu CI 步骤早已备齐）。
  **Qt configure 的 feature 是强制语义**——首次修复无平台限定导致
  macOS/Windows 的 qtbase configure 直接失败
  （`Feature "xcb": Forcing to "ON" breaks its condition`，不会静默
  降级），改为按平台拆分 qtbase 依赖条目：`platform: "linux"` 带
  xcb / `platform: "!linux"` 不带（depend-info 三平台解析验证）
- 第三层：qtbase 的 xcb 强制拉起 `system_xcb_xinput`，Ubuntu apt
  清单独缺 `libxcb-xinput-dev`（其余 xcb 系列全齐），补装后 configure
  通过（icu.h/sctp.h/tzdb 等 try-compile 报错是可选特性探测，非致命）
- 第四层（预审发现）：Create Nightly Release job 此前从未真正执行
  （总挂在 Package 层），默认只读 GITHUB_TOKEN 对 delete-asset 与
  release 发布必然 403——workflow 顶层补 `permissions: contents: write`
- 第五层（诊断轮证伪一版误判）：qtbase[xcb] 构建成功后
  linuxdeploy-plugin-qt 仍报 libqxcb.so 不存在——曾据插件日志的
  ldd 依赖列表误判"文件存在仅路径不匹配"做镜像修复，随后的诊断行
  证明 **libqxcb.so 全树不存在**（platforms 目录在、目录为空）。
  Qt 源码考古闭环确认静态逻辑应产出插件（`if(QT_FEATURE_xcb)`
  是唯一门禁，强制语义下 build 成功即 feature=ON；Linux 默认
  QT_QPA_PLATFORMS=["xcb"]，DEFAULT_IF 恒真；QT_AUTODETECT_ANDROID
  不置 ANDROID 变量；vcpkg 端口无插件裁剪）——实际产物与全部静态
  推理矛盾，根因藏在无法取到的 runner configure 输出里
- 第六层（终局方案）：Linux 桌面端弃用 vcpkg qtbase，改用发行版
  系统 Qt6（qt6-base-dev）——vcpkg.json desktop feature 删除
  `platform: "linux"` 条目（!linux 两平台不变）；nightly 与 ci 的
  Linux apt 清单以 qt6-base-dev 替换整套 xcb/xkb 开发包。理由：
  ① 系统包的 libqxcb.so 与 qmake 布局天然自洽，linuxdeploy-
  plugin-qt 回到全社区标准路径（插件运行时发现由其自动生成的
  AppDir qt.conf 解决，AppRun 无需 QT_PLUGIN_PATH）；② vcpkg
  toolchain 不设 FIND_ROOT_PATH_MODE=ONLY（仅响应外部设置，且
  ONLY 时也补 `/` 到搜索路径），系统 Qt6 的 find_package 可达，
  已核验；③ Linux job 省掉 22 分钟 qtbase 构建。depend-info 验证：
  x64-linux 零 qtbase，x64-osx/x64-windows 保留。残余风险：系统
  Qt 版本（6.4/6.8+）低于此前 vcpkg 的 6.10.2，desktop 源码若用
  新 API 需随编译报错适配
- 同日早前修复已验证生效：Linux qmake 定位、Windows 150min 步骤
  超时放宽、macOS macdeployqt 绝对路径

### 2026-09-13 - V1 段下载完整性闭环（Range 撒谎服务器静默损坏防护）
- 修复生产引擎（daemon/CLI 的 HTTP 段下载路径）三连环静默损坏缺陷：
  ① `download_segment_curl` 对 Range 请求不校验 206——服务器宣称
  Accept-Ranges 却忽略 Range 回 200 全量（透明代理/CGI 常见）时，
  curl 把整个文件传回并追加进已有段文件；② 成功路径不看段文件尺寸
  （`validate_pieces=false` 硬编码，宽松校验形同虚设），200-全量/
  短传的段无条件按 segment_size 记账标记完成；③ 恢复检测与失败
  更新两处 `min(file_size, segment_size)` clamp 把超尺寸段"祝福"为
  完成 + merge 零校验——损坏数据静默并入成品，错误结果报 COMPLETED
- 修复四层防线：`start > 0` 的段请求要求 206（否则截回本次续传起点
  按失败收尾，损坏数据不留给 merge）；段成功路径无条件精确尺寸校验
  （恰好等于段长，取代 validate_pieces 开关的宽松校验——该死配置
  字段删除）；超尺寸段删除整段重下（对旧版缺陷时代遗留的损坏段
  文件自愈）；merge 前逐段校验作最终闸门
- `download_single` 同域加固：续传请求被以 200 应答时清空临时文件
  降级完整重下（一次有界的浪费尝试优于损坏成品；现代 libcurl 自带
  resume 守卫先拒时行为不变）；段重试记账修正——失败后从段文件
  尺寸 best-effort 更新进度，超尺寸不再 clamp 成完成而是删除重来
- 新增测试：`SegmentDownloaderIntegrity` 3 用例（超尺寸遗留段自愈/
  撒谎 mock 损坏追加检测后恢复/短传拒绝不得静默出成品）+
  `RangeIntegrity` 2 个回环端到端（新增 RangeLiarServer：HEAD 宣称
  Accept-Ranges、GET 一律 200 全量——单路续传终局不变量"绝不
  COMPLETED + 内容损坏"、分段路径必须失败干净且成品不出现）；
  `mock_segment_download` 改 app 追加写入（忠于 206 服务器语义，
  旧截断重写契约在精确校验下会假失败）

### 2026-09-12 - V2 引擎临时文件发布闭环（temp_extension 端到端生效 + 死配置清扫）
- `EngineConfig::temp_extension` 与 `auto_start` 为最后两个零消费
  配置：temp_extension 自初始核心库即无任何实现；auto_start 唯一
  "引用"在被误提交进库的 `falcon-cli/src/main.cpp.bak` 死备份文件里
  （已随本commit 删除，.bak/.orig/.rej 全库清零）
- V2 实现临时文件发布语义（aria2 同思路）：temp_extension 非空
  （默认 ".falcon.tmp"）时数据写 `<最终名><扩展名>`，任务组完成时
  **原子改名**为最终名——下载中途与失败之后，半成品不再顶着最终名
  出现（媒体播放器/用户不会误取半截文件）；改名在 Completed 之前，
  监听者看到完成时成品必然已就位，改名失败按失败收尾不会假报
  COMPLETED；失败/中断的临时文件保留（未来断点续传挂点），最终名
  文件不受影响
- 与既有闭环的正交性：overwrite 门禁仍查最终名（授权覆盖的二次
  下载由"trunc 临时文件 + 完成改名"天然原子化，旧文件要么完整保留
  要么被完整替换，不再有中间态）；磁盘写缓冲冲刷先于改名（发布时
  数据必然全部落盘）；temp_extension 置空即直写最终名
- V1 两个字段删除（temp_extension 特性归属 EngineConfigV2，
  auto_start 描述的是引擎固定行为；V1 生产引擎数据路径不动）
- 新增 2 个用例（download_engine_v2_run_test）：完成改名（最终名
  完整成品 + 临时文件消失）/ 置空直写对照；异常销毁用例改为断言
  滞留数据落在临时路径且最终名不产生；既有多段定位写测试种子改
  预置临时路径（模拟引擎真实状态，完成断言经 rename 后不变）；
  全量 1477 ctest 通过，ASan 136 用例零告警

### 2026-09-12 - V2 引擎磁盘写缓冲闭环（enable_disk_cache/disk_cache_size 端到端生效）
- `enable_disk_cache`/`disk_cache_size` 自初始核心库（9b73dca 时代）
  即为愿望式配置，两引擎全链路零消费且默认 `true`（配置在撒谎）；
  V2 裸 socket 路径每 recv 块直写 ofstream（多段模式还逐块 seekp
  强制冲刷流缓冲），小块写 syscall 放大实打实存在
- V2 实现（`HttpDownloadCommand` 应用层写缓冲）：`enable_disk_cache=
  true` 时数据攒在内存、攒满 `disk_cache_size` 一次性落盘（顺带消掉
  多段逐块 seekp）；`=false` 保持直写；容量在文件打开时从引擎配置
  取定（引擎级参数，任务选项无法承载，`DownloadEngineV2::config()`
  新只读访问器），接收即记账（进度/限速按收到的字节计，与落盘解耦）
- 异常路径兜底：超时清理与停机排水直接销毁命令、不经 execute 收尾
  分支——析构补冲刷（`finish_output`），滞留缓冲的数据不再随命令
  静默丢失；完成路径冲刷失败按段错误收尾（磁盘满不会假报
  COMPLETED）；完成/失败/析构三条路径共用同一收口
- V1 `EngineConfig` 同名字段删除（curl+FILE* 已双层缓冲，实现无
  收益；按 resume_if_exists 先例清除死配置），特性归属 V2 配置
- 新增 3 个用例（`download_engine_v2_run_test.cpp`，新增
  PartialThenHangServer 部分响应后挂起服务器）：小缓冲多次落盘
  （16KB 缓冲下载 64KB 逐字节一致）/ 禁用直写对照 / 异常销毁兜底
  冲刷（4KB 滞留缓冲 + 超时清理，析构后文件必须完整含这 4KB）；
  既有全部下载用例默认走缓冲路径即回归；全量 1475 ctest 通过
  （WsRpcClient 一例并行负载抖动，串行复跑即过），ASan 134 用例
  零告警

### 2026-09-12 - 进度回调节流闭环（progress_interval_ms 端到端生效）
- 修复事件风暴缺陷：`DownloadTask::update_progress` 每次调用都
  无条件下发 `on_progress`，而 V1 curl 写回调与 V2 每 recv 块都直打
  该咽喉——监听链全量挨打（TaskStorageListener 自带 1s 节流只保
  DB 写入，其余监听者无防护）；
  `DownloadOptions::progress_interval_ms`（默认 500ms）恰为缺的
  节流值，此前全链路零消费（仅序列化往返）
- 节流收在 `DownloadTask::update_progress` 单点：非终态更新距上次
  下发不足 `progress_interval_ms` 即吞没；终态进度（downloaded ≥
  total）不节流，监听者必须能看到 100%；存储值（downloaded/total/
  speed）始终即时更新，节流只作用于监听回调
- 新增用例（download_task_test）：间隔内突发只下发一次 / 短睡后
  仍吞没（1s 间隔 + 50ms 短睡，调度延迟近 1s 才会误判，防 CI
  抖动）/ 终态穿透 / 间隔恢复下发 / 存储值不受节流影响；全量
  1472 ctest 通过，ASan 44 用例零告警

### 2026-09-12 - V2 引擎停机排水（run() 退出关闭命令持有的 fd）
- 补齐 33efedc 超时清理修复的姊妹项：运行期挂起 fd 由超时清理
  收口，但 shutdown/force_shutdown/异常停机退出 run() 时仍在队列
  或挂起中的命令随容器析构——HTTP 命令析构 `= default` 不关 fd，
  对端黑洞任务在默认 120s 兜底超时到达前停机即静默泄漏 fd
- `Command` 根基类新增 `virtual int socket_fd()`（默认 -1），
  HttpInitiate/Response/Download 三个命令 override 返回各自持有
  的 fd（fd 生命周期语义注释化：由引擎统一管理，命令析构不关）
- run() 退出统一调 `drain_command_fds()`：锁内收集两个容器中命令
  持有的 fd 并清空容器与 socket 映射 → 锁外 `remove_event` +
  `close_socket_fd`；EventPoll::poll 在 run() 线程内同步派发回调，
  循环退出后排水无并发面
- 新增用例（SilentServer 黑洞 + shutdown）：EOF 断言归因干净——
  任务/引擎兜底超时在测试时长内不触发，服务器侧观察到 EOF 只能
  来自停机排水（修复前 fd 泄漏即观察不到）；同时断言排水不改变
  任务状态（非失败语义）；全量 1471 ctest 通过，ASan 25 用例零
  告警

### 2026-09-12 - 死代码清理（resume_if_exists 死字段 + http_plugin_v2 死文件）
- 删除 `DownloadOptions::resume_if_exists`：注释自述"resume_enabled 的
  别名"，唯一消费点在一个从未接入构建的死文件里，活代码全库零读；
  断点续传语义由 `resume_enabled` 独立承载（V1 四处消费）
- 删除 `plugins/http/http_plugin_v2.cpp`（466 行）：2025-12 时代的
  `HttpPlugin` 重复实现，与在建的 `http_plugin.cpp` 同名同类（接入
  构建即重定义冲突），全库零引用零构建；沿用 websocket_server/
  xml_rpc_server 死原型删除先例，git 历史可查
- 全量 1470 ctest 通过，零新增编译警告

### 2026-09-12 - V2 引擎覆盖保护闭环（overwrite_existing 端到端生效）
- 修复静默数据破坏缺陷：`DownloadOptions::overwrite_existing` 默认
  false（"不覆盖已存在文件"），但 V2 首段下载命令无条件以 trunc 打开
  输出文件——默认配置下二次下载同名文件即静默销毁旧文件；该字段
  此前全链路零消费（task_manager 序列化往返、CLI/daemon 配置读写，
  引擎侧从不读取）
- 门禁挂 `RequestGroup::init()`（输出路径在此确定、激活时恰好执行
  一次）：文件已存在且未显式 `overwrite_existing=true` → 组直接
  FAILED + 明确错误消息（aria2 `allow-overwrite=false` 同语义），
  不发任何网络请求；门禁先于协议检查，错误消息区分"文件已存在"
  与连接失败
- 任务终态同步补齐：组在 init 失败（覆盖拒绝、URL 协议不支持）时
  同步 task Failed + error（此前组 FAILED 但任务永久停留初始
  Pending 态，与组状态脱节）
- V1 不在本次范围（daemon/CLI 生产引擎，行为改动需评估停机恢复流）；
  V2 暂无断点续传，已存在文件没有可续传语义，覆盖必须显式授权
- 新增 2 个用例（`download_engine_v2_run_test.cpp`，新增
  MinimalHttpServer 测试服务器）：默认配置保护旧文件（FAILED +
  错误含"已存在" + 文件内容逐字节保留，URL 不可达但错误必须是
  文件已存在——证明门禁先于网络生效）/ 显式授权覆盖（COMPLETED +
  内容完整替换）；全量 1470 ctest 通过，ASan 19 用例零告警

### 2026-09-12 - V2 引擎连接级重试链（max_retries + retry_delay_seconds 端到端生效）
- 修复三重缺陷：`HttpRetryCommand` 是孤儿命令（生产路径零创建，
  仅测试直接构造）——V2 HTTP 下载失败根本没有重试，
  `DownloadOptions::max_retries`/`retry_delay_seconds` 零消费；且该
  命令在引擎事件循环线程里 `sleep_for(5s)`——一个任务重试时整个
  引擎所有任务与 socket 事件停摆；初始连接失败后无人给任务组标
  终态——任务悬空 Downloading、all_completed 永不成立、run() 永不
  退出（既有 run 测试的 keepalive 任务恰好依赖此悬空行为）
- 重试链接线（aria2 max-tries 同语义）：连接失败（connect 失败/
  响应头阶段断连，均无已下载数据）→ `make_connection_retry` 构造
  `HttpRetryCommand` → 以 `NEED_RETRY` 回队轮询到
  `retry_delay_seconds` 到点（不注册 socket 事件、不阻塞引擎线程）
  → 重新 initiate；重试计数经 `set_retry_count` 沿命令链传递
  （首连 + max_retries 次重试），重试期间任务组保持 ACTIVE
- 边界语义：多连接意图的任务不参与连接级重试（分段失败语义不同，
  暂不覆盖）；传输中断（已下载数据）重试属断点续传域，亦不在本次
  范围；HTTP 状态错误/重定向不支持等语义性失败重试无价值，直接
  终态收口
- 新增 `fail_group_terminal` 收口：初始连接失败重试耗尽后组标
  FAILED + task Failed（复用段失败收尾语义）
- 新增 5 个端到端用例（`download_engine_v2_retry_test.cpp`，
  FlakyServer 前 N 次连接立即关闭 + accept 计数）：瞬时故障恢复
  （COMPLETED，恰 2 次连接）/ 重试耗尽精确次数（3 次）/ 延迟消费
  （2 次重试 ≥2s）/ 多连接任务跳过重试（1 次连接）/ 连接拒绝获得
  终态；既有 run 测试 keepalive 任务改用长挂起重试链保持组 ACTIVE
  （组悬空依赖随缺陷一并移除）；全量 1468 ctest 通过，ASan 17
  用例零告警

### 2026-09-12 - V2 引擎超时清理闭环（任务终态 + fd 关闭 + 任务级超时）
- 修复 `cleanup_completed_commands` 双重缺陷（对端黑洞时暴露）：
  命令挂起超时后只销毁命令对象——`HttpResponseCommand`/
  `HttpDownloadCommand` 析构 `= default` 不关 fd（泄漏），且不把
  所属任务组标 FAILED（任务永久悬空 Downloading、all_completed
  永不成立、run() 永不退出）
- 重构为三遍扫描：锁内收集等待中命令 {cmd_id, fd, task_id, 挂起
  时间} → 锁外按任务阈值筛选 → 锁内移除映射取所有权 → 锁外
  `remove_event` + `close_socket_fd`（新增引擎侧辅助，_WIN32 走
  closesocket）+ 复用 `fail_group_of_command` 标终态（与命令异常
  路径同语义；组未完成时必为 ACTIVE，task=0 命令 find_group 安全
  无操作）
- `DownloadOptions::timeout_seconds` 从零消费端变为生效：任务
  显式设置（>0）优先，未设置回落引擎 `command_wait_timeout_seconds`
  （默认 120s 兜底）
- 新增 2 个黑洞服务器用例（`download_engine_v2_run_test.cpp`，
  SilentServer 接受连接后不读不写）：任务级 2s / 引擎兜底 2s 两条
  路径均断言组 FAILED + run() 在时限内退出 + 服务器侧观察到 EOF
  （进程存活期间 fd 未关闭即观察不到，真泄漏检测）；全量 1463
  ctest 通过，ASan 构建限速/超时/异常边界 17 用例零告警

### 2026-09-12 - V2 引擎单任务限速（任务窗口 + 与全局取严）
- `DownloadOptions::speed_limit` 在 V2 引擎同样零消费端（V1 有
  curl 通道，V2 裸 socket 路径全速跑），复用全局限速的滑动窗口
  机制扩展为 per-task 记账：`report_downloaded_bytes(task_id, n)`
  同时记全局窗口与任务窗口（多连接分段共享任务窗口，预算先到
  先得自然分摊——任务总限速语义）
- `recv_budget(task_id, task_limit)` 取 min(全局预算, 任务预算)；
  任务限速值从 RequestGroup 读取（命令不持有 options）
- 任务级节流不跳过 execute_commands（单任务受限不能拖累其他任务），
  只把本轮 poll 拉长到最早的任务预算恢复点；预算挂起不注册 socket
  事件（数据已在内核缓冲会立即唤醒形成忙旋，改为回队轮询）
- 两个实测缺陷修复：任务窗口淘汰只挂在 report 上，预算耗尽挂起
  期间没有新 report → 预算永不恢复死锁（30s 超时），改为 recv_budget
  查询时同步淘汰；终态任务窗口条目每轮清理防 map 无限增长
- 新增 2 个端到端用例：任务自身 64KB/s（≥2.5s，实测 3.5s）+
  全局/任务取严双向验证；全量回归 protocols 551 + core 402 +
  daemon 229，ASan 15 用例零告警

### 2026-09-12 - V2 引擎全局限速（读层预算 + 事件循环节流）
- `EngineConfigV2::global_speed_limit` 从零消费端变为端到端生效：
  `HttpDownloadCommand::receive_data` 每次 recv 后
  `report_downloaded_bytes` → 引擎 1s 滑动窗口统计全局接收速率
- 节流双层设计（aria2 SpeedCalc 同思路）：
  - 读层预算 `recv_budget()`（限值 − 窗口占用）：recv 前查询，
    单次读取量截断到预算内、预算归零即挂起等 socket 事件——
    单次 execute 最多循环读 64 轮，若不在读层限流，一次就能把
    整个文件拉完，循环级节流永远插不进来；截断同样必要，预算
    残量时整缓冲读会让均速到限值 2 倍（测试实测复现）
  - 循环级节流：窗口达限后 `evaluate_throttle` 以"旧样本满 1s 龄
    淘汰、预算恢复"为截止点拉长 poll 等待并跳过数据面命令，
    socket 事件照常处理不丢唤醒
- `set_global_speed_limit` 运行时可调（清空节流截止立即生效），
  多连接分段下载共享同一窗口（全局语义）
- 新增 3 个端到端用例（`download_engine_v2_speed_limit_test.cpp`）：
  全速对照上界 + 配置构造/运行时 setter 两条限速路径的时间下界
  断言（256KB @ 64KB/s ≥ 2.5s，实测 ≈3.0s 精确节流）；测试服务器
  带 RAII 析构（joinable 线程析构即 terminate）与 Winsock 类型
  别名，三平台可编译
- 全量回归：protocols 549 + core 402 + daemon 229 通过，ASan 构建
  V2 限速/异常边界 13 用例零告警

### 2026-09-12 - 引擎全局限速真正落地（零消费端 → 端到端生效）
- 修复全局限速"只存值不生效"的缺陷：`set_global_speed_limit` 此前
  仅写原子并广播事件，下载路径零消费端（daemon.json 的
  `max_overall_speed_limit` 与 RPC `max-overall-download-limit`
  均为无效配置）；`EngineConfig::global_speed_limit` 构造参数同样
  从未消费
- 通道：`IEventListener::query_speed_limit(task_id)`（默认返回 0，
  非破坏扩展）→ `TaskManager` 实现（全局限速按并发槽位均摊、与
  任务自身 `options.speed_limit` 取严）→ HTTP handler 消费
- HTTP 单连接路径：初值 + 进度回调每 200ms 窗口查询一次，变化时
  热应用 `CURLOPT_MAX_RECV_SPEED_LARGE`（libcurl 支持传输中修改，
  RPC/SIGHUP 改限速即时生效）；段路径：任务限速按连接数均摊后经
  `options.speed_limit` 传入（既有消费点）
- 新增 3 个回环 HTTP 端到端用例（`global_speed_limit_test.cpp`，
  `falcon_http_tests`）：256KB 全速 520ms / 全局 64KB/s 4021ms /
  任务自身 64KB/s 4021ms——时间下界断言只验证"限速生效必变慢"，
  不设上界防 CI 抖动误报；测试 server 用 poll+超时防 Linux close
  阻塞 accept 不唤醒的挂死（复用 RangeTestServer 模板）
- 全量回归：core 392 + protocols 424 + daemon 229 全绿

### 2026-09-12 - V2 引擎异常边界（命令异常不再 terminate 整个进程）
- `run()` 主循环体兜底 try/catch：循环内异常安全停机（引擎常以 `run()`
  作线程函数，异常逃逸即 `std::terminate`——此前引擎零 try/catch，
  任何命令异常直接杀死整个进程，UAF 调查中确认的最大扩散面
- `execute_commands()` 单命令异常边界：捕获后所属任务组标 FAILED
  （新增 `fail_group_of_command`，多段组走 `finish_segment(false)`
  收尾并记录错误消息），同轮其余命令与引擎继续运行
- `execute_routine_commands()` 例程命令异常只跳过本轮（例程是全局性
  后台任务，无对应任务组可标失败）
- Socket 事件回调逻辑提取为 `handle_socket_ready()` 并整体 try/catch：
  回调在 EventPoll 线程上下文执行，异常逃逸即 terminate；回调失败仅
  丢失一次唤醒（挂起命令由超时清理回收）
- 新增 3 个异常注入测试（`download_engine_v2_run_test.cpp`）：注入
  抛 `std::runtime_error` / 非 std 异常的命令验证引擎存活 + 组标
  FAILED + 同轮命令不受影响；例程异常验证引擎继续运行；protocols
  全量 424 用例通过，ASan 构建 3 轮无告警

### 2026-09-12 - Daemon 下载参数配置化（daemon.json "download" 节）
- `daemon.json` 新增 `download` 节（`max_concurrent_tasks`/
  `max_overall_speed_limit`），启动时应用、SIGHUP 重载热更，与
  `aria2.changeGlobalOption` 键位对齐
- 顺手加固 WS 测试基建的注册窗口竞争（`wait_registered` 辅助），
  消除负载下 BroadcastFanout/count 断言的偶发失败
- daemon 全量 229 用例通过

### 2026-09-12 - Daemon SIGHUP 配置重载（daemon.json 热更新）
- SIGHUP 触发重读启动时生效的 daemon.json：`rpc.secret`/
  `allow_origin_all` 经 `JsonRpcServer::update_auth` 立即生效，
  监听/存储/守护化项变化告警"restart required"；重载失败保持现有
  配置继续运行
- 修复信号处理器直接执行重载回调的缺陷：新增 async-signal-safe 的
  `DaemonManager::request_reload()`（仅置原子标志），`run()` 主循环
  在普通线程上下文消费执行
- 新增 2 个 main 集成用例（SIGHUP 换 secret 生效、坏配置重载不死机）；
  daemon 全量 225 用例通过

### 2026-09-12 - 修复 V2 引擎多段下载悬垂引用（Windows CI 崩溃根因）
- `HttpResponseCommand::options_` 由引用成员改为值拷贝：其构造方
  `HttpInitiateConnectionCommand` 在 `send_http_request` 末尾把自己
  的 `options_` 值成员按引用传入，随后命令对象即被引擎队列销毁，
  响应命令再解引用即 heap-use-after-free（ASan 于
  `determine_download_strategy` 首行命中；Windows MSVC 堆 free 后
  改写导致读脏数据崩溃，且引擎线程无异常边界 → 静默 terminate）
- Linux glibc free 后暂不改写，读回旧值侥幸通过（压测 300 次不复现），
  属三平台共同潜伏缺陷；`HttpDownloadCommand`/`RequestGroup` 核查为
  值持有，全库仅此一处悬垂引用
- ASan 构建 20 轮压测 + protocols 全量 446 用例通过

### 2026-09-12 - Daemon 配置文件加载（daemon.json）
- 新增 `daemon.json` 配置文件：`rpc`（enabled/host/port/secret/
  allow_origin_all）、`daemon`（run_as_daemon/pid_file/working_dir/
  log_file）、`storage`（task_db_path）三节，路径支持 `~` 展开
- 优先级：命令行显式参数 > 配置文件 > 默认值；`--conf-path <file>`
  显式指定（必须存在），默认尝试 `~/.config/falcon/daemon.json`（存在
  才加载，aria2 语义），`--no-conf` 短路一切加载
- 容错：JSON 非法/类型错误报错退出（daemonize 之前），未知键告警不失败
- 新增 12 个解析单测 + 8 个真实二进制集成用例（全量 1444 用例通过）

### 2026-09-12 - 桌面端接入 Daemon 事件流
- 新增 `WebSocketRpcClient`（daemon 包，随 `falcon_daemon_rpc_client` 库）：
  WS 单连接承载请求/响应与服务器通知，断线自动重连，`call()` 语义与
  HTTP 客户端一致
- desktop `DaemonRpcBackend` 切换到 WS 客户端：daemon 通知（任务状态
  变更/进度推送）即时触发快照刷新，500ms 轮询降为兜底；刷新路径
  `IDownloadBackend::set_wake_callback` + `DownloadService::request_refresh`
- 新增 10 个 WS 客户端回环测试（daemon 159 用例全过）+ desktop 事件
  唤醒全链路用例（等价目标本地编译验证 6/6）；修复 desktop 测试目标
  缺失 server 符号的潜在链接缺陷

### 2026-09-12 - Daemon WebSocket 事件流订阅
- daemon 同端口支持 WebSocket 升级（`ws://host:6800/jsonrpc`，aria2 真实
  行为，AriaNg 实时模式可直接对接）；WS 上的 JSON-RPC 与 HTTP 共用分发与
  `token:` 认证，28 个方法全会话内可用
- 新增 `websocket_frame.{hpp,cpp}`：RFC 6455 协议层（增量帧解析、掩码、
  分片聚合、控制帧；SHA1/base64 自实现，daemon 不新增 OpenSSL 依赖）
- 引擎事件经 `RpcEventBridge` 广播为 JSON-RPC 通知：aria2 兼容
  `onDownloadStart/Pause/Complete/Error/Stop` + Falcon 扩展
  `falcon.onProgress`（每任务 1 秒节流）；params[0] 携带 gid + 进度快照
- 删除从未接入构建的死代码原型 `websocket_server.{hpp,cpp}`
- 停机安全：有活动订阅者时 `stop()` shutdown 唤醒会话线程，不挂死
- 新增 15 个 WebSocket 测试用例（RFC 向量/帧协议/真实引擎事件链回环），
  daemon 全量 149 用例本地通过

### 2026-09-12 - V2 引擎 Windows 运行时适配
- 修复 `http_commands.cpp` 三类 Winsock 运行时缺陷（此前仅"能编译"，跳过测试
  掩盖了无法实际运行）：
  - Winsock 调用失败后不设置 errno——新增 `sock_errno()/sock_err_str()/
    sock_would_block()` 辅助（Windows 走 `WSAGetLastError()`），替换全部
    `errno` 直接判定
  - 非阻塞 connect 的"进行中"判定：Winsock 一律报 `WSAEWOULDBLOCK`（旧代码
    按 `errno == EINPROGRESS` 判定，Windows 上必然误判为连接失败）
  - send/recv 的 EAGAIN 判定（MSVC 的 EAGAIN=11 与 WSAEWOULDBLOCK=10035
    永不匹配，缓冲区满会被误判为致命错误）
- 移除 `http_commands_coverage_test.cpp` 全部 7 处 `TODO(Win)` GTEST_SKIP，
  Windows 与 POSIX 统一走真实事件循环（PollEventPoll/WSAPoll）；测试基建补
  `RangeTestServer::start()` 显式 winsock 初始化
- `download_engine_v2_test.cpp` 解除 Windows 排除：ScopedPipe 的 pipe 以回环
  TCP 连接等价实现（仅作合法 fd 喂给引擎映射表，无 I/O 依赖），三平台统一编译
- docs 站点依赖漏洞修复（Dependabot 8 条）：pnpm overrides 强制 vite 6.4.3+/
  esbuild 0.25+/postcss 8.5.23+/nanoid 3.3.18+，文档站构建验证通过

### 2026-09-11 - 桌面应用接入 Daemon RPC（下载服务层）
- desktop 新增下载服务层：`IDownloadBackend` 抽象 + `InProcessBackend`
  （进程内引擎）/ `DaemonRpcBackend`（aria2 兼容 JSON-RPC）双实现，
  UI 经 `DownloadService` 快照轮询驱动，不再直连 `DownloadEngine`
- daemon 拆出 `falcon_daemon_rpc_client` 静态库（JSON-RPC 客户端 +
  aria2 快照转换层），desktop 硬依赖；RPC 方法扩至 28 个（新增
  `aria2.changePriority`），tellStatus 携带 `priority` 扩展字段
- 设置页新增 Daemon 模式开关（RPC URL + secret，重启后生效）；
  下载页重写为快照驱动（行内暂停/继续、右键优先级子菜单）
- 新增纯 C++ 后端回环测试 `falcon_desktop_backend_tests`（5 用例），
  desktop Qt 层随 CI Qt6 job 编译验证

### 2026-09-11 - Daemon RPC API 全面完善
- aria2 兼容 JSON-RPC 方法扩至 26 个，查询/删除同时覆盖引擎内存态与 SQLite 历史（引擎优先、storage 回落、按 id 去重）
- 新增 `getFiles`/`getUris`/`getOption`/`getGlobalOption`/`changeGlobalOption`/`getSessionInfo`/`saveSession`/`purgeDownloadResult`/`removeDownloadResult`/`pauseAll`/`unpauseAll`/`forceShutdown`/`system.multicall` 等
- `forceShutdown` 接线 `DaemonManager::request_stop()`，RPC 可触发正常停机（排水 + 落库）
- 修复 `TaskStorage::create_task` 不写显式 id 的缺陷（此前 RPC addUri 落库后 storage id 与引擎 id 在删除/重启后错位）
- JSON-RPC 错误码分层修正：-32700 仅限 JSON 解析失败，dispatch 内部异常报 -32603
- 删除死代码 `xml_rpc_server.{hpp,cpp}`；daemon CLAUDE.md 从 gRPC 蓝图重写为 JSON-RPC 实际实现
- Core 新增只读查询 `get_global_speed_limit()` / `get_max_concurrent_tasks()`
- 新增测试目标 `falcon_daemon_rpc_storage_tests`（15 用例）与扩展 `falcon_daemon_rpc_coverage_tests`

### 2026-09-11 - Daemon 任务持久化闭环
- 新增 `TaskStorageListener`：引擎状态/进度事件实时落库（SQLite，进度 1 秒节流）
- 停机改为暂停语义（`pause_all` + 排水），未完成任务以可恢复状态入库
- 重启恢复：跳过终态、还原 `output_path`、Paused 任务恢复但不自动启动
- 修复重启后任务 id 计数器与持久化记录冲突（`DownloadEngine::set_next_task_id` + `TaskStorage::get_max_task_id`）
- vcpkg.json / CI 增加 SQLite3，daemon 持久化与存储测试纳入 CI

### 2026-04-14 - 四库拆分重构（P0+P1）
- 将单体 `libfalcon` 拆分为四个独立包：`libfalcon-core`、`libfalcon-protocols`、`libfalcon-storage`、`libfalcon-drives`
- 建立独立 CMake target 和 alias（`Falcon::core/protocols/storage/drives`）
- Core 头文件收口：不再暴露 S3/OSS/COS/CloudStorage/ResourceBrowser 等概念
- 测试文件按库归属迁移到对应包
- 删除旧 `packages/libfalcon/` 单体目录
- 跨包 option 提升到顶层 CMakeLists.txt

### 2026-04-15 - 公共头文件路径重组（Phase 3）
- 24 个公共头文件迁移到 namespace 对齐路径
  - `libfalcon-drives`: 3 headers → `falcon/drives/`
  - `libfalcon-storage`: 11 headers → `falcon/storage/`
  - `libfalcon-protocols`: 10 headers → `falcon/protocols/`（含子目录）
- 保留向后兼容 shim 头文件（纯转发，无警告）
- 更新 45+ 源文件/测试文件的 include 路径
- CI 全平台验证通过

### 2026-04-15 - 接口收尾与文档清理（Phase 4）
- 文档更新：修正 `plugin_interface.hpp` 旧引用为 `protocol_handler.hpp`
- shim 警告清理：移除 24 个 shim 头文件的 `#warning` 编译警告
- `falcon::plugins` 命名空间清理完成（代码中无残留）
- 验证安装导出配置完善（Config.cmake.in、FalconTargets 导出）

### 2025-12-21 - 添加私有协议支持
- 实现迅雷 thunder:// 协议支持
- 实现 QQ 旋风 qqlink:// 协议支持
- 实现快车 flashget:// 协议支持
- 实现 ED2K 电驴协议支持
- 实现 HLS/DASH 流媒体协议支持
- 更新插件管理器支持所有新协议
- 为 CLI 工具添加所有新协议的命令行支持
- 添加私有协议的单元测试

### 2025-12-21 - 重构为 Monorepo 架构
- 调整项目结构为 Monorepo 模式
- 建立 packages/ 和 apps/ 目录分离
- 更新模块索引与构建系统说明
- 完善跨平台构建配置

### 2025-12-21 - 初始化项目架构
- 创建项目基础架构文档
- 定义模块化设计方案
- 建立编码规范与开发指引

### 2026-05-07 - 协议处理器扩展接口与委托机制
- 新增 `IProtocolHandlerExtension` 扩展接口
- 支持协议处理器访问 `ProtocolRegistry` 实现协议委托
- Thunder/FlashGet/QQDL 插件实现委托下载：解析链接后委托给实际协议处理器
- ED2K 插件实现多源下载委托：遍历源地址列表尝试下载

### 2026-05-07 - BitTorrent DHT/PEX 集成与 HTTP 分块传输
- BitTorrent 插件集成 DHT 客户端和 PEX 扩展协议
  - DHT 路由表（K-bucket）、XOR 距离计算、UDP 通信
  - PEX 管理：peer 生命周期、候选 peer 管理、消息协议处理
  - 与 libtorrent 模式集成：通过 `connect_peer()` 添加 DHT/PEX 发现的 peers
- HTTP 分块传输编码（Chunked Transfer Encoding）解析
  - 完整状态机：READ_SIZE → READ_DATA → READ_CR → READ_LF → READ_TRAILER
  - Windows memmem 兼容实现
- 资源搜索功能完善：JSON 响应解析、路径模式替换、多格式支持

### 2026-05-12 - 协议插件重构完成
- 所有私有协议插件（ED2K/Thunder/QQDL/FlashGet/HLS）迁移到 `IProtocolHandler` 接口
- 统一任务管理：TaskContext、活动任务跟踪、异步下载线程模型
- 完整支持任务暂停、恢复、取消操作
- HLS 插件实现完整 M3U8 下载逻辑：播放列表解析 → 段并行下载 → 二进制合并

---

## 项目愿景

**Falcon（猎鹰下载器）** 是一个现代化、跨平台的 C++ 下载解决方案，采用 Monorepo 架构，致力于提供高性能、可扩展的多协议下载能力。

### 核心目标
- 打造一个轻量级、高性能的下载引擎核心库（libfalcon-core）
- 标准下载协议作为独立库（libfalcon-protocols）
- 对象存储与资源浏览作为独立库（libfalcon-storage）
- 网盘与云存储作为独立库（libfalcon-drives）
- 提供直观易用的 CLI 工具（falcon-cli）
- 支持后台守护进程模式（falcon-daemon）
- 预留 GUI 桌面应用与 Web 管理界面扩展能力
- 保持跨平台兼容性（Windows/Linux/macOS）
- 异步架构，支持大规模并发下载

### 发展阶段
- **第一阶段（MVP）**：HTTP/HTTPS 基础下载、CLI 工具
- **第二阶段**：FTP、BitTorrent/Magnet、断点续传
- **第三阶段**：ED2K（电驴）、私有链接解析（thunder://）、HLS/DASH
- **第四阶段**：Daemon 后台服务、RPC 接口、GUI/Web 应用

---

## 架构总览

### 设计原则
- **SOLID 原则**：保证代码可维护性和可扩展性
- **插件化架构**：协议处理器作为独立插件，方便新增/移除协议
- **异步优先**：基于现代 C++ 异步模型（std::async、协程或第三方异步库）
- **接口隔离**：核心库与应用层完全解耦，可独立作为库被其他项目引用
- **Monorepo 管理**：所有模块统一版本控制，便于依赖管理与协同开发

### 架构分层

```
┌─────────────────────────────────────────────────────────┐
│           应用层 (apps/)                                 │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │   desktop    │  │     web      │  │   (future)   │  │
│  │  GUI 桌面应用 │  │  Web 管理界面 │  │   移动端等    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└──────────────────────────┬──────────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────────┐
│          工具层 (packages/)                              │
│  ┌──────────────┐  ┌──────────────┐                     │
│  │ falcon-cli   │  │falcon-daemon │                     │
│  │  命令行工具   │  │  后台守护进程 │                     │
│  └──────┬───────┘  └──────┬───────┘                     │
│         │                 │                             │
│         └─────────┬───────┘                             │
│                   │                                     │
│  ┌────────────────▼────────────────┐                   │
│  │       核心库 (libfalcon-core)    │                   │
│  │   下载引擎 / 任务管理 / 事件系统  │                   │
│  │   插件注册接口 / 通用基础设施     │                   │
│  └───┬──────────┬──────────┬───────┘                   │
│      │          │          │                            │
│  ┌───▼───┐ ┌───▼───┐ ┌───▼───┐                        │
│  │proto- │ │storage│ │drives │                         │
│  │cols   │ │       │ │       │                         │
│  │HTTP   │ │S3/OSS │ │网盘   │                         │
│  │FTP/BT │ │COS等  │ │云存储 │                         │
│  │ED2K等 │ │资源浏览│ │搜索等 │                         │
│  └───────┘ └───────┘ └───────┘                         │
└────────────────────────────────────────────────────────┘
                     │
┌────────────────────▼────────────────────────────────────┐
│       基础设施层 (third_party/ & 系统库)                 │
│  libcurl, libtorrent, spdlog, CLI11, nlohmann/json...   │
└─────────────────────────────────────────────────────────┘
```

### 依赖方向

```
falcon_protocols  →  falcon_core
falcon_storage    →  falcon_core
falcon_drives     →  falcon_core
```

禁止反向依赖：`core` 不依赖 `protocols/storage/drives`。

---

## 模块结构图

```mermaid
graph TD
    A["(根) falcon"] --> B["packages/"];
    B --> C["libfalcon-core"];
    B --> D["libfalcon-protocols"];
    B --> E["libfalcon-storage"];
    B --> F["libfalcon-drives"];
    B --> G["falcon-cli"];
    B --> H["falcon-daemon"];

    A --> I["apps/"];
    I --> J["desktop"];
    I --> K["web"];

    A --> L["cmake/"];
    A --> M["third_party/"];
    A --> N["docs/"];
    A --> O["examples/"];

    D -->|depends on| C
    E -->|depends on| C
    F -->|depends on| C

    click C "./packages/libfalcon-core/CLAUDE.md" "查看核心库文档"
    click D "./packages/libfalcon-protocols/CLAUDE.md" "查看协议库文档"
    click E "./packages/libfalcon-storage/CLAUDE.md" "查看存储库文档"
    click F "./packages/libfalcon-drives/CLAUDE.md" "查看网盘库文档"
    click G "./packages/falcon-cli/CLAUDE.md" "查看 CLI 文档"
    click H "./packages/falcon-daemon/CLAUDE.md" "查看 Daemon 文档"
```

---

## 模块索引

| 模块路径 | 职责 | CMake Target | 状态 |
|---------|------|-------------|------|
| `packages/libfalcon-core` | 下载引擎/任务管理/事件系统 | `Falcon::core` | 开发中 |
| `packages/libfalcon-protocols` | HTTP/FTP/BT/ED2K/HLS 等协议 | `Falcon::protocols` | 开发中 |
| `packages/libfalcon-storage` | S3/OSS/COS/Kodo/Upyun 对象存储 | `Falcon::storage` | 开发中 |
| `packages/libfalcon-drives` | 网盘/云存储/搜索/配置管理 | `Falcon::drives` | 开发中 |
| `packages/falcon-cli` | 命令行下载工具 | `falcon-cli` | 开发中 |
| `packages/falcon-daemon` | 后台守护进程 + RPC 服务 | `falcon-daemon` | 开发中 |
| `apps/desktop` | GUI 桌面应用（Qt6） | `falcon-desktop` | 规划中 |
| `apps/web` | Web 管理界面（预留） | — | 规划中 |

---

## 运行与开发

### 构建系统
- **CMake 3.15+**：主构建工具
- 支持三平台：Windows (MSVC/MinGW)、Linux (GCC/Clang)、macOS (Clang)
- Monorepo 模式：顶层 CMakeLists.txt 通过 `add_subdirectory()` 组织子项目

### 依赖管理
- 推荐使用 **vcpkg** 或 **Conan** 管理第三方依赖
- 或通过 Git Submodules 引入（放在 `third_party/`）

### 主要依赖项
| 依赖 | 用途 | 版本要求 |
|------|------|---------|
| libcurl | HTTP/FTP 协议支持 | 7.68+ |
| OpenSSL | HTTPS/TLS 支持 | 1.1+ |
| libtorrent-rasterbar | BitTorrent 协议（可选） | 2.0+ |
| spdlog | 日志库 | 1.9+ |
| CLI11 | 命令行解析 | 2.3+ |
| nlohmann/json | JSON 配置解析 | 3.10+ |
| gRPC | RPC 框架（daemon 用） | 1.40+ |
| GoogleTest | 单元测试 | 1.12+ |

### 编译步骤

```bash
# 1. 克隆仓库
git clone https://github.com/yourusername/falcon.git
cd falcon

# 2. 安装依赖（以 vcpkg 为例）
vcpkg install curl libtorrent spdlog cli11 nlohmann-json grpc gtest

# 3. 配置 CMake
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake

# 4. 编译所有模块
cmake --build build --config Release

# 5. 运行 CLI 工具
./build/bin/falcon-cli --help

# 6. 启动 Daemon（可选）
./build/bin/falcon-daemon --config daemon.json
```

### 开发模式编译

```bash
# Debug 模式 + 测试 + 示例
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON \
  -DFALCON_BUILD_EXAMPLES=ON \
  -DFALCON_BUILD_DAEMON=OFF  # 可选关闭 daemon

cmake --build build
ctest --test-dir build --output-on-failure
```

### 独立模块编译

```bash
# 仅编译核心库
cmake -B build -S . -DFALCON_BUILD_CLI=OFF -DFALCON_BUILD_DAEMON=OFF -DFALCON_BUILD_DESKTOP=OFF -DFALCON_BUILD_EXAMPLES=OFF
cmake --build build --target falcon_core

# 仅编译协议库（自动包含 core）
cmake --build build --target falcon_protocols

# 仅编译 CLI
cmake -B build -S . -DFALCON_BUILD_DAEMON=OFF -DFALCON_BUILD_DESKTOP=OFF
cmake --build build --target falcon-cli
```

---

## 测试策略

### 测试框架
- **Google Test (GTest)**：单元测试与集成测试
- **CMake CTest**：测试运行器
- **Google Benchmark**：性能测试（可选）

### 测试覆盖率目标
- 核心库（libfalcon）：≥ 80%
- 关键路径（下载逻辑、插件加载）：≥ 90%
- 协议插件：≥ 70%（依赖外部服务的部分可 mock）
- CLI/Daemon：≥ 60%（UI 逻辑除外）

### 测试分类
1. **单元测试** (`packages/*/tests/unit/`)
   - 每个类/模块独立测试
   - 使用 mock 隔离外部依赖（如网络、文件系统）

2. **集成测试** (`packages/*/tests/integration/`)
   - 端到端流程测试（下载小文件、断点续传、多任务等）
   - 需本地测试服务器或公开测试资源

3. **性能测试** (`packages/libfalcon/tests/benchmark/`)
   - 使用 Google Benchmark
   - 测试下载速度、并发性能、内存占用

### 持续集成
- **GitHub Actions**：多平台构建 + 测试（Ubuntu/Windows/macOS）
- **代码覆盖率**：Codecov 或 Coveralls
- **静态分析**：clang-tidy、cppcheck

---

## 编码规范

### C++ 标准与风格
- **标准**：C++17（最低要求），推荐 C++20（便于协程）
- **命名规范**：Google C++ Style Guide 或 LLVM Coding Standards
  - 类名：PascalCase（`DownloadEngine`）
  - 函数/变量：snake_case（`start_download()`）
  - 成员变量：`m_` 前缀（`m_task_queue`）或下划线后缀（`task_queue_`）
  - 常量：全大写 + 下划线（`MAX_RETRY_COUNT`）

### 代码组织
- **头文件**：
  - Core 公共 API 放在 `packages/libfalcon-core/include/falcon/`
  - Protocols 公共 API 放在 `packages/libfalcon-protocols/include/falcon/`
  - Storage 公共 API 放在 `packages/libfalcon-storage/include/falcon/`
  - Drives 公共 API 放在 `packages/libfalcon-drives/include/falcon/`
  - 使用 `#pragma once` 或传统的 include guard

- **源文件**：
  - 一个类一个文件（除非是紧密相关的小类）
  - `.cpp` 文件按模块组织在对应目录

### 注释与文档
- 使用 **Doxygen** 风格注释
- 所有公共 API 必须有详细注释（参数、返回值、异常、示例）
- 复杂逻辑添加行内注释说明意图

示例：
```cpp
/**
 * @brief 启动下载任务
 *
 * @param url 下载链接（支持 http/https/ftp/magnet 等）
 * @param output_path 保存路径（绝对路径或相对路径）
 * @param options 下载选项（可选，包含并发数、超时等）
 * @return DownloadTask* 任务对象指针，失败返回 nullptr
 * @throws InvalidURLException 当 URL 格式非法时
 */
DownloadTask* start_download(const std::string& url,
                              const std::string& output_path,
                              const DownloadOptions& options = {});
```

### 异常处理
- 使用异常处理错误（非性能关键路径）
- 自定义异常类继承自 `std::exception`
- 关键异常定义在 `packages/libfalcon-core/include/falcon/exceptions.hpp`

### 资源管理
- 使用 **RAII** 原则
- 智能指针优先：`std::unique_ptr`（独占所有权）、`std::shared_ptr`（共享所有权）
- 避免裸指针传递所有权

### 线程安全
- 所有公共 API 必须是线程安全的或明确标注非线程安全
- 使用 `std::mutex`、`std::lock_guard`、`std::shared_mutex` 等
- 避免死锁：固定加锁顺序、使用 `std::scoped_lock` 多锁

### 格式化工具
- 使用 **clang-format** 自动格式化
- 配置文件：`.clang-format`（基于 Google 或 LLVM 风格）
- Git pre-commit hook 自动运行

---

## AI 使用指引

### 推荐交互方式
1. **架构设计**：先讨论模块划分、接口设计，确认后再生成代码
2. **接口优先**：先定义头文件与接口，再实现具体逻辑
3. **增量开发**：逐模块开发，每次明确当前模块的依赖关系
4. **测试驱动**：为每个新功能同时提供单元测试代码
5. **文档同步**：每次架构变更时更新对应模块的 `CLAUDE.md`

### 关键决策记录（ADR）
在做重要技术决策时，在 `docs/decisions/` 目录下创建 ADR 文档，格式：
```
# ADR-001: 选择异步模型为 std::async

## 背景
需要支持大规模并发下载任务...

## 决策
选择 std::async + std::future 作为异步基础...

## 后果
优点：...
缺点：...
```

### 插件开发指引
新增协议插件时，请遵循以下步骤：
1. 在 `packages/libfalcon-protocols/plugins/<protocol_name>/` 创建目录
2. 实现 `IProtocolHandler` 接口（定义在 `packages/libfalcon-core/include/falcon/protocol_handler.hpp`）
3. 如需访问其他协议处理器（如包装协议委托下载），实现 `IProtocolHandlerExtension` 接口
4. 在插件目录下创建 `CLAUDE.md` 记录协议特性、依赖库、测试方法
5. 在 `packages/libfalcon-protocols/CMakeLists.txt` 中添加编译选项（可选编译该插件）

**协议委托机制**：
- 包装协议（thunder://、flashget://、qqlink://）应实现 `IProtocolHandlerExtension`
- 通过 `set_protocol_registry()` 获取 `ProtocolRegistry` 引用
- 使用 `delegateDownload()` 将解析后的真实 URL 委托给对应协议处理器

---

## 目录结构说明

```
falcon/                              # 项目根目录
├── CMakeLists.txt                   # 根 CMake 配置（管理所有子项目）
├── LICENSE                          # Apache 2.0 许可证
├── README.md                        # 项目介绍（面向用户）
├── CLAUDE.md                        # 本文件（面向 AI 与开发者）
├── todo.md                          # 重构任务跟踪
├── .gitignore                       # Git 忽略规则
├── .clang-format                    # 代码格式化配置
├── .github/
│   └── workflows/                   # CI/CD 配置
│
├── packages/                        # C++ 包（4个独立库 + 2个工具）
│   ├── libfalcon-core/              # 核心下载引擎库
│   │   ├── CMakeLists.txt           # Target: falcon_core / Falcon::core
│   │   ├── CLAUDE.md
│   │   ├── include/falcon/          # 公共 API 头文件
│   │   │   ├── download_engine.hpp
│   │   │   ├── task_manager.hpp
│   │   │   ├── protocol_handler.hpp
│   │   │   ├── protocol_handler_extension.hpp  # 协议扩展接口
│   │   │   ├── event_dispatcher.hpp
│   │   │   ├── exceptions.hpp
│   │   │   └── version.hpp
│   │   ├── src/                     # 核心库实现
│   │   └── tests/                   # 核心库测试
│   │
│   ├── libfalcon-protocols/         # 协议实现库
│   │   ├── CMakeLists.txt           # Target: falcon_protocols / Falcon::protocols
│   │   ├── include/falcon/
│   │   ├── src/                     # DownloadEngineV2, SegmentDownloader 等
│   │   ├── plugins/                 # 协议插件
│   │   │   ├── http/                # HTTP/HTTPS (libcurl)
│   │   │   ├── ftp/                 # FTP
│   │   │   ├── bittorrent/          # BitTorrent/Magnet
│   │   │   │   ├── bencode.cpp/hpp  # B 编码实现
│   │   │   │   ├── dht_node.cpp/hpp # DHT 路由节点
│   │   │   │   └── pex_protocol.cpp/hpp  # PEX 扩展协议
│   │   │   ├── thunder/             # 迅雷协议
│   │   │   ├── ed2k/                # ED2K 电驴
│   │   │   ├── hls/                 # HLS/DASH 流媒体
│   │   │   └── ...                  # 其他协议
│   │   └── tests/
│   │
│   ├── libfalcon-storage/           # 对象存储库
│   │   ├── CMakeLists.txt           # Target: falcon_storage / Falcon::storage
│   │   ├── include/falcon/
│   │   ├── src/                     # ResourceBrowser 等
│   │   ├── plugins/                 # 存储插件
│   │   │   ├── s3/                  # Amazon S3
│   │   │   ├── oss/                 # 阿里云 OSS
│   │   │   ├── cos/                 # 腾讯云 COS
│   │   │   ├── kodo/                # 七牛云
│   │   │   └── upyun/               # 又拍云
│   │   └── tests/
│   │
│   ├── libfalcon-drives/            # 网盘与云存储库
│   │   ├── CMakeLists.txt           # Target: falcon_drives / Falcon::drives
│   │   ├── include/falcon/
│   │   ├── src/                     # CloudStoragePlugin, ConfigManager 等
│   │   └── tests/
│   │
│   ├── falcon-cli/                  # CLI 命令行工具
│   │   ├── CMakeLists.txt
│   │   ├── CLAUDE.md
│   │   ├── src/
│   │   └── tests/
│   │
│   └── falcon-daemon/               # 后台守护进程
│       ├── CMakeLists.txt
│       ├── CLAUDE.md
│       ├── src/
│       └── tests/
│
├── apps/                            # 应用层（GUI/Web）
│   ├── desktop/                     # 桌面应用（预留）
│   │   ├── CLAUDE.md
│   │   └── README.md                # 技术选型说明（Qt/Tauri/Electron）
│   └── web/                         # Web 管理界面（预留）
│       ├── CLAUDE.md
│       └── README.md                # 技术选型说明（React/Vue）
│
├── cmake/                           # CMake 辅助模块
│   ├── FindLibcurl.cmake
│   ├── FindLibtorrent.cmake
│   ├── CodeCoverage.cmake
│   └── CompilerWarnings.cmake
│
├── third_party/                     # 第三方依赖（如使用 submodule）
│   └── README.md                    # 依赖说明（或使用 vcpkg）
│
├── docs/                            # 文档
│   ├── api/                         # API 文档（Doxygen 生成）
│   ├── user_guide.md                # 用户指南
│   ├── developer_guide.md           # 开发者指南
│   └── decisions/                   # 架构决策记录 (ADR)
│       └── ADR-001-async-model.md
│
├── examples/                        # 使用示例
│   ├── simple_download.cpp          # 基础下载示例
│   ├── batch_download.cpp           # 批量下载示例
│   └── CMakeLists.txt
│
└── .claude/
    └── index.json                   # AI 扫描索引（自动生成）
```

---

## 配置文件说明

### CMake 配置选项
```cmake
# 用户可配置选项
option(FALCON_BUILD_TESTS "构建测试" ON)
option(FALCON_BUILD_EXAMPLES "构建示例" ON)
option(FALCON_BUILD_CLI "构建 CLI 工具" ON)
option(FALCON_BUILD_DAEMON "构建 Daemon 服务" ON)

# 插件开关
option(FALCON_ENABLE_HTTP "启用 HTTP/HTTPS 插件" ON)
option(FALCON_ENABLE_FTP "启用 FTP 插件" ON)
option(FALCON_ENABLE_BITTORRENT "启用 BitTorrent 插件" OFF)

# 依赖管理
option(FALCON_USE_SYSTEM_LIBS "使用系统库而非 vcpkg" OFF)
option(FALCON_USE_STATIC_LIBS "静态链接依赖库" OFF)
```

### 运行时配置
CLI 程序支持配置文件（`~/.config/falcon/config.json`）：
```json
{
  "max_concurrent_tasks": 5,
  "default_download_dir": "~/Downloads",
  "log_level": "info",
  "plugins": {
    "http": {
      "timeout_seconds": 30,
      "max_retries": 3,
      "user_agent": "Falcon/1.0"
    }
  }
}
```

Daemon 配置文件（默认 `~/.config/falcon/daemon.json`，或 `--conf-path` 显式指定）：
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
    "task_db_path": "/var/lib/falcon/tasks.db"
  },
  "download": {
    "max_concurrent_tasks": 5,
    "max_overall_speed_limit": 0
  }
}
```

优先级：命令行显式参数 > 配置文件 > 内置默认值；`--no-conf` 跳过加载。
SIGHUP 重载：`rpc.secret`/`allow_origin_all` 与 `download` 节立即生效，
监听/存储/守护化项需重启。

---

## 下一步开发计划

### ✅ 第一阶段（已完成）
1. **核心库架构**
   - ✅ 任务管理器（TaskManager）
   - ✅ 下载引擎核心（DownloadEngine）
   - ✅ 插件系统框架（ProtocolRegistry）
   - ✅ 公共接口（IProtocolHandler）
   - ✅ 协议扩展接口（IProtocolHandlerExtension）

2. **HTTP/HTTPS 插件**
   - ✅ 基于 libcurl 实现
   - ✅ 支持断点续传、分块下载
   - ✅ HTTPS/TLS 支持（OpenSSL）
   - ✅ HTTP 分块传输编码解析

3. **协议支持**
   - ✅ HTTP/HTTPS
   - ✅ FTP/FTPS
   - ✅ BitTorrent/Magnet（含 DHT/PEX）
   - ✅ ED2K（电驴）
   - ✅ Thunder（迅雷）
   - ✅ FlashGet（快车）
   - ✅ QQDL（QQ 旋风）
   - ✅ HLS/DASH 流媒体

4. **高级下载特性**
   - ✅ 多线程分块下载（SegmentDownloader）
   - ✅ 协议委托机制
   - ✅ 任务暂停/恢复/取消

### 🔄 第二阶段（进行中）
1. **Daemon 服务**
   - ✅ HTTP RPC 服务器
   - ✅ aria2 兼容 API（26 个方法，含查询回落、批量控制、会话管理）
   - ✅ 任务持久化（SQLite：状态/进度落库、停机保存、重启恢复）
   - ✅ 事件流订阅（同端口 WebSocket 推送 aria2 兼容通知 + 进度通知）
   - ✅ 配置文件加载（daemon.json，CLI > 文件 > 默认值）

2. **桌面应用（Qt6）**
   - ✅ 迅雷风格 UI
   - ✅ 任务列表（表格/网格视图）
   - ✅ 系统托盘集成
   - ✅ 主题切换（亮色/暗色）
   - ✅ 云存储浏览（S3/OSS/COS）
   - ✅ 资源搜索集成
   - ✅ Daemon 通信（aria2 兼容 JSON-RPC 后端 + 进程内引擎双后端；
     WebSocket 事件流驱动刷新，轮询兜底）

3. **测试与文档**
   - 🔄 单元测试覆盖率提升
   - 🔄 集成测试补充
   - ✅ API 文档更新

### 📋 第三阶段（规划中）
1. **更多协议支持**
   - 🔄 网盘直链解析（百度/阿里云盘/夸克等）
   - 📋 WebDAV 协议
   - 📋 SFTP 协议

2. **完善桌面应用**
   - 📋 设置页面完善
   - 📋 下载规则管理
   - 📋 任务调度功能

3. **发布准备**
   - 📋 多平台安装包
   - 📋 用户手册
   - 📋 性能优化

---

**文档维护**：每次架构调整、模块新增、重要功能开发时，请更新本文件对应章节，并在顶部"变更记录"中添加条目。
