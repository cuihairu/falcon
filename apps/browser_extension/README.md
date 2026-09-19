# Falcon Browser Extension (Chrome / Edge)

MV3 扩展：接管浏览器下载并转发给 Falcon（桌面应用或 daemon），提供媒体嗅探、
右键菜单下载、页面链接批量收集与任务面板。兼容 Chrome 及其衍生内核浏览器
（Edge、Brave、Opera 等，Chromium ≥ 114）。

## 功能

- **下载接管**：浏览器开始下载时转发给 Falcon 并取消浏览器下载；可按文件类型
  过滤（Options 中配置扩展名清单，留空 = 全部接管）。
- **右键菜单**：
  - 链接/视频/音频右键 → 「用 Falcon 下载」。
  - 页面右键 → 「用 Falcon 收集本页链接」打开批量选择页。
- **批量链接收集**（batch 页）：`scripting.executeScript` 提取页面全部
  http(s) 链接，支持关键字/扩展名过滤、全选/全不选，批量发送到 Falcon。
- **媒体嗅探**：检测视频/音频/HLS(.m3u8)/DASH(.mpd) 请求，popup 中选择发送；
  可选包含分片（ts/m4s），每 tab 上限 80 条。
- **任务面板**（popup）：展示 Falcon 任务列表（文件名、进度条、速度、状态）；
  daemon 模式下可直接暂停/继续。
- **双发送目标**（Options 中选择）：
  - `Falcon 桌面应用`（默认）：本地 IPC `http://127.0.0.1:51337/v1/add`，
    桌面端弹出添加对话框。
  - `Falcon daemon`：aria2 兼容 JSON-RPC（`http://127.0.0.1:6800/jsonrpc`，
    `aria2.addUri`，`token:` 认证），任务直接入队不打扰。
- 「跳过下一次下载（此 tab）」与按站点禁用（主机名后缀匹配）。
- 发送时可附带 Cookies（用于已登录资源，≤ 8KB）。
- Falcon 不可达时可回落 `falcon://add?url=...` 深链（需系统注册协议处理器）。

## 安装（开发者模式）

1. 打开 `chrome://extensions`（或 `edge://extensions`）。
2. 开启 **开发者模式**。
3. **加载已解压的扩展程序**，选择 `apps/browser_extension/` 目录。

## 桌面端 / Daemon 端

- **桌面应用**：监听 `http://127.0.0.1:51337`，`POST /v1/add` 收到 URL 后弹出
  添加对话框（202 立即应答，不阻塞）；`GET /v1/health` 可做无副作用连通性探测；
  `GET /v1/tasks`、`GET /v1/stats` 供任务面板使用。改端口需同时改扩展 Options
  与桌面端配置。
- **Daemon 模式**：即 falcon-daemon 的 aria2 兼容 RPC（HTTP/WebSocket 同端口，
  默认 6800），在 Options 中填 RPC 地址与 secret（`rpc.secret`）即可。
- Falcon 未运行时，可回落 `falcon://add?url=...`（需在 OS 层注册 `falcon://`
  协议指向 Falcon 可执行文件）。

## 嗅探说明

- 嗅探使用 `webRequest`（observational）+ `<all_urls>` host 权限，仅在
  Options 开启时工作。
- 加密/DRM 流与 blob: URL 无法嗅探；认证资源建议开启「发送时包含 Cookies」。
