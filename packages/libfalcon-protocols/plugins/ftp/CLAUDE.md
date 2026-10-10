[根目录](../../../../CLAUDE.md) > [packages](../../../) > [libfalcon](../../CLAUDE.md) > [plugins](../) > **ftp**

---

# FTP Plugin

## 变更记录 (Changelog)

### 2026-10-10 - timeout_seconds 停滞看门狗语义收口（2026-09-30 P0 批次遗留 ② 的 FTP 侧闭环）
- **同型缺陷修复**：`apply_common_curl_options` 把 `timeout_seconds` 映射
  `CURLOPT_TIMEOUT`（总时长硬帽，默认 30s 杀死一切总时长超限的慢而健
  康下载）——HTTP 路径 2026-09-30 已收口，FTP 路径当时显式遗留（"后续
  批次收口"）。修复沿同款范式：transfer 路径改 `CURLOPT_LOW_SPEED_
  LIMIT=1 + CURLOPT_LOW_SPEED_TIME=timeout_seconds`（低于 1 B/s 持续
  该秒数才中止），`timeout_seconds==0` 不设看门狗对；探测路径
  （get_file_info 的 SIZE 查询）保持 `CURLOPT_TIMEOUT` 总帽语义（探测
  应秒级完成），helper 增 `bool transfer` 参数区分两路
- **附带**：transfer/probe 统一补 `CURLOPT_CONNECTTIMEOUT 10L`（此前
  FTP 路径无连接超时上限，curl 默认 300s——移除总帽后为唯一有界性保
  障，与 HTTP 路径同值）
- **回归钉子**：`SlowHealthyTransferSurvivesTimeoutSeconds`（4096B @
  16B/10ms ≈ 2.56s 总时长 > timeout 2s 照常 Completed——旧映射 2031ms
  整必红，双向往返验证；对位 HTTP 路径同名钉子）；ftp_handler_test
  22 → 23 用例

### 2026-09-14 - 覆盖率批次 F：ftp_plugin.cpp 135 → 9 miss（真实测试重写）
- 旧 `ftp_handler_test.cpp` 为 55 个自说自话的占位测试（断言字符串
  字面量，不触达产品代码），唯一真实的 registry 测试还因 weak stub
  符号拉取缺陷长期 GTEST_SKIP——135 miss 即"零真实测试"
- **weak stub 链接陷阱**（registry 0 注册的根因）：core 的
  `builtin_protocol_handlers_stub.cpp` 提供 weak 空实现，真实实现
  编译进 `falcon_builtin_protocol_handlers` 对象；GNU ld 归档一次
  扫描下，`falcon_ftp_tests` 不引用任何 protocols 符号 → 真实实现
  对象从未拉入 → 空 stub 生效。daemon 因 RPC 引用
  `describe_builtin_protocols` 强符号而免疫。测试侧以引用强符号
  强制拉入收口（ProtocolRegistryLoadsFtpHandler 由 Skip 变真断言）
- 新 `mock_ftp_server.hpp`（自 storage 测试基建复制 + 下载语义扩
  展）：RETR 按内容应答、REST 续传偏移、一次性/永久命令失败、分
  块慢发（暂停窗口）；storage 侧不动
- 测试重写为 20 真实用例：SIZE 探测（成功/未知文件 throw——curl
  对 SIZE 550 判 Remote file not found，**RETR 之前即弃**，该顺
  序经命令序列 dump 实证）、下载端到端（RETR 落盘 + rename 发布 +
  tmp 消失）、REST 断点续传（偏移断言 + app 拼接）、瞬态 RETR 失
 败重试（含 1s 指数退避时长下界）、重试耗尽（恰 2 次 RETR）、
  SIZE 探测失败零 RETR、输出目录打开失败、rename 失败（成品路径
  被目录占用 → FileIOException，状态保持 Pending 绝不假报完成）、
  慢发进度记账（越过 200ms 节流窗，downloaded/total 更新断言）、
  暂停中止（progress_callback 返回 1 → CURLE_ABORTED_BY_CALLBACK，
  tmp 保留为断点）、cancel 先行跳过、resume 重启、proxy + 凭据生
  效性（无人监听代理必败、流量从未直连）、限速选项黑盒
- 覆盖率：ftp_plugin.cpp gcov miss **135 → 9**（行 84.25%→见当批
  收口数字）；剩余为 curl_easy_init OOM throw×2、write_callback
  的 `!is_open()` 防御（回调时 file 必开着）等不可测项

### 2025-12-21 - 初始化插件架构
- 创建 FTP 插件目录结构

---

## 插件职责

FTP 协议插件，基于 **libcurl** 实现，支持：

- FTP/FTPS 基础下载
- 断点续传（FTP REST 命令）
- 被动模式（PASV）
- 用户认证

---

## 依赖

- **libcurl** 7.68+（需启用 FTP 支持）

---

## 测试要点

- `tests/unit/mock_ftp_server.hpp`：编程式 mock FTP 服务器（控制
  协议 + EPSV 数据通道 + RETR/REST/慢发/一次性失败），与 storage
  包的同名基建分叉演化（浏览器语义 vs 下载语义）
- FTPS（AUTH TLS）mock 未实现，测试仅覆盖明文 ftp:// 路径
- curl 在 RETR 前先做 SIZE 探测，550 即弃（不发 RETR）——断言
  "RETR 未发生"时须确认 SIZE 也未注册

---

**文档维护**：实现完成后更新本文档。
