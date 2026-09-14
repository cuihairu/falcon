[根目录](../../../../CLAUDE.md) > [packages](../../../) > [libfalcon](../../CLAUDE.md) > [plugins](../) > **http**

---

# HTTP/HTTPS Plugin

## 变更记录 (Changelog)

### 2026-09-14 - 覆盖率批次 G：V1 curl 数据面回环测试 + 空指针缺陷修复
- **修复 pause/resume/cancel 空指针崩溃**：`pause(nullptr)` 直接
  解引用（FtpHandler 同位置有防御，跨插件不一致）——三外层入口
  统一 `if (!task) return;`（impl 与 V2 转发共用）
- `http_handler_edges_test.cpp` 26 用例（自包含可编程 HTTP 服务
  器，cov + ASan 双绿）：响应头解析（Content-Disposition 引号/
  无引号、URL filename 推导含 query 剥除与 "download" 默认）、
  curl 选项传播服务器侧观测（UA/Referer/自定义头/cookie 引擎往
  返/401 挑战后 Basic 重放/必败代理零直连）、单连接 REST 续传、
  HTTP 错误重试语义（5xx 退避剧本/4xx 立即抛）、rename 与输出打
  开失败收口、慢发进度记账与限速热应用（listener 逐窗查询 want
  != applied 分支）、未知总长（无 Content-Length EOF 定界）下载、
  分段端到端/段错误收口/传输中 pause-cancel 转发/暂停 resume 重
  入/段短传重试续传/Range 撒谎服务器分段失败干净
- gcov miss 80 → 16（行 96.2%），剩余全部定性（OOM×3、防御分支、
  结构不可达、行归属伪影、毫秒竞态窗口）

### 关键语义（测试与排障须知）
- `download()` 顶部先 `get_file_info`（HEAD 探测）再分叉 V1/V2
  （`v2_http_enabled()` 默认 false → V1 curl 路径）——HEAD 失败
  即整体失败，单连接 GET 层的 4xx/5xx 语义在 HEAD 通过后才生效
- **分段路径的段不查 task 状态**：`segment_progress_callback` 只
  看 downloader 的 cancelled 原子标志——暂停/取消必须经
  `pause()/cancel()` 命中 `active_segmented_downloads_` 转发
  `downloader->cancel()`，仅 `task->set_status()` 无法中止在传段
- **SegmentDownloader 析构即清理段文件**：handler 层暂停后
  downloader 随 download() 返回析构，段断点不保留；resume 重新
  全量分段下载
- 现代 libcurl 自带 resume 守卫：续传请求被 200 应答即
  CURLE_RANGE_ERROR 先拒——handler 内的 200-check 清空重下分支
  是老 curl 纵深防御，新 curl 下不可达
- `resume()` 不复位任务状态：恢复前置位 Downloading 是调用方
  （TaskManager::resume_task → start_task）职责，Paused 状态下
  直接 resume 会被 download_single 循环顶守卫静默返回

### 2025-12-21 - 初始化插件架构
- 创建 HTTP 插件目录结构
- 定义插件接口实现计划

---

## 插件职责

HTTP/HTTPS 协议插件，基于 **libcurl** 实现，支持：

- HTTP/HTTPS 基础下载
- 断点续传（HTTP Range 请求）
- 多线程分块下载
- 自定义请求头
- 代理支持
- SSL/TLS 验证
- 重定向处理

---

## 依赖

- **libcurl** 7.68+（需启用 HTTPS 支持）

---

## 接口实现

实现 `IProtocolHandler` 接口：

```cpp
class HTTPPlugin : public IProtocolHandler {
public:
    std::string protocol() const override { return "http"; }
    bool can_handle(const std::string& url) const override;
    std::shared_ptr<DownloadTask> download(...) override;
    FileInfo get_file_info(const std::string& url) const override;
};
```

---

## 测试

### 单元测试
- `http_plugin_test.cpp`：插件基础功能测试

### 集成测试
- `http_download_test.cpp`：完整下载流程（需测试服务器）
- `http_resume_test.cpp`：断点续传测试

---

## 下一步

1. 实现 `HTTPPlugin` 类
2. 封装 libcurl API（RAII wrapper）
3. 支持 Range 请求（分块下载）
4. 编写单元测试

---

**文档维护**：实现完成后更新本文档，记录关键实现细节与注意事项。
