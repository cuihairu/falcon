# 协议支持

Falcon 的传输能力围绕统一引擎按层级组织：核心传输协议默认启用，P2P 与多源能力由同一引擎承载，私有协议族作为可选兼容插件，对象存储与资源浏览由 storage 包提供。下面的状态以当前代码库和顶层构建配置为准（`CMakeLists.txt` 的 `FALCON_ENABLE_*`）。

## 支持的协议

| 层级 | 协议 | 默认 | 说明 |
|------|------|------|------|
| **1 · 核心传输** | HTTP/HTTPS | ✅ 开 | V1/libcurl 与 V2 事件驱动双引擎、断点续传、多段下载、代理 |
| **1 · 核心传输** | FTP/FTPS | ✅ 开 | 主动/被动模式、REST 续传 |
| **1 · 核心传输** | SFTP | ✅ 开（需 libssh2） | known_hosts 验证、断点续传 |
| **1 · 核心传输** | WebDAV | ✅ 开 | `dav`/`davs`；storage 层另有 WebDAV 浏览器 |
| **2 · P2P 与多源** | BitTorrent/Magnet | ✅ 开（需 libtorrent） | 分片级 P2P、做种、DHT、NAT 端口映射 |
| **2 · P2P 与多源** | Metalink | ✅ 开 | RFC 5854 `.meta4` / Metalink3 `.metalink`，多镜像 P2SP 分段 + 发布前哈希校验 |
| **3 · 兼容插件** | 迅雷 / QQ 旋风 / 快车 / ED2K / HLS-DASH | 可选 | 默认关闭，按需 `FALCON_ENABLE_*` 打开 |
| **资源访问** | S3 / OSS / COS / Kodo / 又拍云 | ✅ 开 | 对象存储（SigV4）、MinIO / RustFS 私有网关、远程浏览与资源搜索 |

## 基础协议

### HTTP/HTTPS

最常用的下载协议，Falcon 提供全面支持：

- 断点续传
- 多线程分块下载
- 速度控制
- 自定义 HTTP 头部
- 代理支持
- 认证支持

[查看详细文档 →](./http.md)

### FTP/FTPS

传统的文件传输协议：

- 主动和被动模式
- 匿名和认证登录
- FTPS（显式 TLS）支持

[查看详细文档 →](./ftp.md)

## P2P 协议

### BitTorrent

BitTorrent 是默认启用的 P2P 能力，数据面由 libtorrent 承担：

- Magnet 链接与 .torrent 文件
- DHT（libtorrent 原生）、LSD、UPnP、NAT-PMP
- 可选文件下载、做种（seed-ratio / seed-time）
- 无 libtorrent 环境优雅降级为纯 C++ 实验模式

[查看详细文档 →](./bittorrent.md)

### ED2K

代码库中有 ED2K 相关实现，但默认构建通常关闭：

- 文件链接支持
- 服务器连接
- 来源交换
- AICH 校验

## 私有协议

### 迅雷 (Thunder)

代码库中保留私有协议实现：

- `thunder://` 经典链接
- `thunderxl://` 离线链接

[查看详细文档 →](./private.md)

### QQ旋风 (QQDL)

腾讯的下载协议：

- `qqlink://` 标准链接
- 带 GID 的链接

### 快车 (FlashGet)

老牌下载器的协议：

- `flashget://` 完整格式
- `fg://` 短格式

## 流媒体协议

### HLS/DASH

代码库中保留 HLS/DASH 相关实现：

- .m3u8 播放列表
- .mpd 清单文件
- 自动质量选择
- 加密流支持

[查看详细文档 →](./streaming.md)

## 编译时启用协议

使用 CMake 选项控制哪些协议被编译（前六个默认 ON，后五个默认 OFF）：

```bash
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_SFTP=ON \
  -DFALCON_ENABLE_WEBDAV=ON \
  -DFALCON_ENABLE_METALINK=ON \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON
```

> 依赖缺失时优雅降级：BitTorrent 缺 libtorrent、SFTP 缺 libssh2 会以 WARNING 自动关闭对应选项；HTTP/FTP/WebDAV 经 libcurl 提供。

## 检查支持的协议

当前版本更可靠的方式是查看构建时传入的 `FALCON_ENABLE_*` 选项，以及生成日志中对应插件是否被启用。

## 协议对比

| 协议 | 速度 | 稳定性 | 资源占用 | 适用场景 |
|------|------|--------|----------|----------|
| HTTP/HTTPS | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ | 低 | 网页、文件下载 |
| FTP | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | 低 | 企业文件共享 |
| BitTorrent | ⭐⭐⭐⭐⭐ | ⭐⭐⭐ | 高 | 大文件分发 |
| ED2K | ⭐⭐⭐ | ⭐⭐ | 中 | 老资源共享 |
| 迅雷/快车 | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | 中 | 中文资源 |
| HLS/DASH | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ | 中 | 视频点播 |

::: tip 选择合适的协议
- **日常下载**：使用 HTTP/HTTPS
- **大文件**：使用 BitTorrent
- **中文资源**：使用迅雷链接
- **视频**：使用 HLS/DASH
:::
