# 协议支持

Falcon 下载器当前以基础下载协议为默认能力，并保留多种扩展协议实现。下面的状态以当前代码库和顶层构建配置为准。

## 支持的协议

| 协议 | 状态 | 说明 |
|------|------|------|
| **HTTP/HTTPS** | 默认启用 | 断点续传、分块下载 |
| **FTP/FTPS** | 默认启用 | 主动/被动模式 |
| **BitTorrent** | 代码库有实现 | 目前未统一接入默认自动注册 |
| **迅雷** | 代码库有实现 | 默认构建通常关闭 |
| **QQ旋风** | 代码库有实现 | 默认构建通常关闭 |
| **快车** | 代码库有实现 | 默认构建通常关闭 |
| **ED2K** | 代码库有实现 | 默认构建通常关闭 |
| **HLS/DASH** | 代码库有实现 | 默认构建通常关闭 |

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

代码库中有 BitTorrent 相关实现，但当前文档站不再把它描述为默认稳定公开能力：

- Magnet 链接支持
- .torrent 文件支持
- DHT、LSD、UPnP、NAT-PMP
- 可选文件下载
- 做种支持

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

使用 CMake 选项控制哪些协议被编译：

```bash
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON
```

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
