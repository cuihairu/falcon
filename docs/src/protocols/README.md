# 协议支持

Falcon 下载器支持广泛的下载协议，从基础的 HTTP/HTTPS 到各种私有协议和流媒体协议。

## 支持的协议

| 协议 | 状态 | 说明 |
|------|------|------|
| **HTTP/HTTPS** | ✅ 完全支持 | 断点续传、分块下载 |
| **FTP/FTPS** | ✅ 完全支持 | 主动/被动模式 |
| **BitTorrent** | ✅ 完全支持 | Magnet、.torrent 文件 |
| **迅雷** | ✅ 完全支持 | thunder:// 协议 |
| **QQ旋风** | ✅ 完全支持 | qqlink:// 协议 |
| **快车** | ✅ 完全支持 | flashget:// 协议 |
| **ED2K** | ✅ 完全支持 | 电驴协议 |
| **HLS/DASH** | ✅ 完全支持 | 流媒体协议 |

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

去中心化的文件共享协议：

- Magnet 链接支持
- .torrent 文件支持
- DHT、LSD、UPnP、NAT-PMP
- 可选文件下载
- 做种支持

[查看详细文档 →](./bittorrent.md)

### ED2K

经典的 P2P 文件共享协议：

- 文件链接支持
- 服务器连接
- 来源交换
- AICH 校验

## 私有协议

### 迅雷 (Thunder)

国内最常用的下载协议之一：

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

自适应流媒体协议：

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

运行以下命令查看当前编译支持哪些协议：

```bash
falcon-cli --list-protocols
```

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
