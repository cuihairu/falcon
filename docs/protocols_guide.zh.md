[English](protocols_guide.md) | **中文**

# Falcon 下载器 - 协议支持指南

> [!WARNING]
> 本文档属于旧版协议说明归档。
> 当前维护中的协议文档请以 `docs/src/protocols/` 下页面为准。

本文档详细介绍了 Falcon 下载器支持的所有下载协议及其使用方法。

## 目录

- [基础协议](#基础协议)
- [私有协议](#私有协议)
- [流媒体协议](#流媒体协议)
- [P2P协议](#p2p协议)
- [使用示例](#使用示例)
- [配置说明](#配置说明)
- [故障排除](#故障排除)

## 基础协议

### HTTP/HTTPS

Falcon 支持完整的 HTTP/HTTPS 协议，包括：

- **断点续传**：自动检测服务器支持，中断后可继续下载
- **分块下载**：多线程并发下载，提高大文件下载速度
- **速度控制**：支持全局和单任务速度限制
- **自定义头部**：可设置 User-Agent 和其他 HTTP 头
- **代理支持**：支持 HTTP/HTTPS/SOCKS 代理
- **认证支持**：基本认证和摘要认证

```bash
# 基础下载
falcon-cli https://example.com/file.zip

# 多线程下载（5个连接）
falcon-cli https://example.com/large.zip -c 5

# 断点续传
falcon-cli https://example.com/file.zip --continue

# 速度限制
falcon-cli https://example.com/file.zip --limit 1M

# 使用代理
falcon-cli https://example.com/file.zip --proxy http://proxy:8080
```

### FTP

支持 FTP 和 FTPS 协议（FTP 默认使用被动模式）。

```bash
# 下载FTP文件
falcon-cli ftp://ftp.example.com/file.zip

# 使用用户名和密码
falcon-cli ftp://user:pass@ftp.example.com/file.zip

# FTPS（libcurl 的 ftps://，含显式与隐式协商）
falcon-cli ftps://ftp.example.com/file.zip
```

## 私有协议

Falcon 支持多种国内常用下载工具的私有协议：

### 迅雷 (Thunder)

支持 `thunder://` 和 `thunderxl://` 链接格式。

```bash
# 经典迅雷链接
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg=="

# 迅雷离线链接
falcon-cli "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
```

### QQ旋风 (QQDL)

支持 `qqlink://` 和 `qqdl://` 格式。

```bash
# QQ旋风链接
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# 带GID的链接
falcon-cli "qqlink://1234567890ABCDEF|http://example.com/video.mp4"
```

### 快车 (FlashGet)

支持 `flashget://` 和 `fg://` 格式。

```bash
# Base64 完整格式（&ref= 尾缀会被剥除；载荷先 Base64 解码再剥 [FLASHGET] 前缀）
falcon-cli "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA==&ref=http://example.com/page"

# 短格式（fg:// 载荷是 URL 编码，不是 Base64）
falcon-cli "fg://http%3A%2F%2Fexample.com%2Ffile.zip"

# Base64 简写（无 &ref= 尾缀同样支持）
falcon-cli "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA=="
```

### ED2K (电驴)

支持 `ed2k://` 文件链接（服务器链接 `ed2k://server|...` 当前不支持，解析会报错）。

```bash
# 文件链接
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# 带源的文件链接
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|s,192.168.1.1:4662|s,192.168.1.2:4662"

# 带AICH校验
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|h=ABCDEF123456789"
```

## 流媒体协议

### HLS (HTTP Live Streaming)

支持 `.m3u8` 播放列表，自动下载所有媒体段并合并。

```bash
# 下载HLS流
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4
```

HLS 特性：
- 解析 `.m3u8` 播放列表（EXTINF 时长信息）
- 按序下载全部媒体段并按二进制拼接合并
- 暂不支持主播放列表（master playlist）择流与加密流（AES 密钥）

### DASH (Dynamic Adaptive Streaming)

DASH（`.mpd` 清单解析）尚未实现；直接提交 `.mpd` URL 会按普通文件下载清单本身。
需要下载流媒体时请使用 HLS 播放列表。

## P2P协议

### BitTorrent

支持 `.torrent` 文件和 `magnet:` 链接。

```bash
# 使用Magnet链接
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# 通过 HTTP 直链下载 .torrent 文件本身
falcon-cli "https://example.com/file.torrent"

# 下载本地torrent文件
falcon-cli "file:///path/to/file.torrent"

# 设置保存目录
falcon-cli "magnet:?xt=urn:btih:HASH" -d /downloads/torrents
```

BitTorrent 特性：
- 支持 DHT、PEX（libtorrent 数据面，含 LSD/UPnP/NAT-PMP）
- 自动获取更多节点
- 支持做种（ratio/时长策略，默认做种到 1.0 倍）

## 使用示例

### 批量下载

创建一个文本文件 `urls.txt`：

```
# HTTP文件
https://example.com/file1.zip
https://example.com/file2.zip

# 迅雷链接
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTMuemlwWlo=

# ED2K链接
ed2k://file|video.mp4|1073741824|ABCDEF123456789|/

# HLS流
https://example.com/playlist.m3u8
```

然后批量下载：

```bash
falcon-cli -i urls.txt
```

### C++ API 使用

```cpp
#include <falcon/falcon.hpp>

int main() {
    // 创建下载引擎（构造时自动注册全部内置协议处理器，无需手动注册）
    falcon::DownloadEngine engine;

    // 配置下载选项
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.speed_limit = 1024 * 1024;  // 1MB/s

    // 下载不同协议的文件
    std::vector<std::string> urls = {
        "https://example.com/file.zip",
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg==",
        "ed2k://file|test.zip|1048576|HASH|/",
        "magnet:?xt=urn:btih:HASH&dn=Test"
    };

    std::vector<falcon::DownloadTask::Ptr> tasks;

    for (const auto& url : urls) {
        if (engine.is_url_supported(url)) {
            auto task = engine.add_task(url, options);
            if (task) {
                engine.start_task(task->id());
                tasks.push_back(task);
                std::cout << "Started download: " << url << std::endl;
            }
        }
    }

    // 等待所有下载完成
    for (auto& task : tasks) {
        task->wait();

        if (task->status() == falcon::TaskStatus::Completed) {
            std::cout << "Download completed!" << std::endl;
            std::cout << "File size: " << task->total_bytes() << " bytes" << std::endl;
        } else {
            std::cout << "Download failed: " << task->error_message() << std::endl;
        }
    }

    return 0;
}
```

## 配置说明

### 编译配置

使用 CMake 控制哪些协议被编译：

```bash
# 启用全部协议
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

# 仅启用基础协议（HTTP/FTP/BT/SFTP/WebDAV/Metalink 默认 ON；五个私有协议默认 OFF）
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_SFTP=ON \
  -DFALCON_ENABLE_WEBDAV=ON \
  -DFALCON_ENABLE_METALINK=ON \
  -DFALCON_ENABLE_THUNDER=OFF \
  -DFALCON_ENABLE_QQDL=OFF \
  -DFALCON_ENABLE_FLASHGET=OFF \
  -DFALCON_ENABLE_ED2K=OFF \
  -DFALCON_ENABLE_HLS=OFF
```

### 运行时配置

当前更稳定的运行时入口是 CLI 参数和 CMake 构建选项。配置文件格式仍在演进中，不建议把某个固定 JSON 路径当成稳定接口。

## 故障排除

### 常见问题

1. **协议不被支持**
   - 确保编译时启用了相应的协议
   - 检查 `CMakeLists.txt` 中对应的 `FALCON_ENABLE_*` 选项

2. **下载失败**
   - 检查网络连接
   - 验证 URL 格式是否正确
   - 查看日志获取详细错误信息

3. **断点续传不工作**
   - 确保服务器支持 Range 请求
   - 检查文件是否被修改

4. **BitTorrent 下载慢**
   - 确保 BT 监听端口（默认 6881）可访问（实际端口以运行日志为准）
   - 检查防火墙设置
   - 添加更多 tracker

### 调试模式

启用详细日志：

```bash
falcon-cli <URL> --verbose

# 或设置日志级别
export FALCON_LOG_LEVEL=debug
falcon-cli <URL>
```

### 性能优化

1. **HTTP 下载优化**
   - 使用分块下载（`-c 5`）
   - 调整缓冲区大小
   - 使用 CDN 或镜像

2. **BitTorrent 优化**
   - 启用 DHT 和 PEX
   - 使用更多 tracker
   - 设置合适的上传限制

3. **系统优化**
   - 增加 TCP 连接限制
   - 使用 SSD 存储
   - 关闭不必要的程序

## 协议对比

| 协议 | 特点 | 适用场景 | 限制 |
|------|------|----------|------|
| HTTP/HTTPS | 简单、广泛支持 | 下载网页、文件 | 依赖服务器速度 |
| FTP | 传统文件传输 | 企业文件共享 | 不够安全 |
| 迅雷/快车 | 国内常用 | 中文资源 | 需要解码 |
| ED2K | P2P网络 | 老资源共享 | 速度不稳定 |
| BitTorrent | 去中心化 | 大文件分发 | 需要节点 |
| HLS/DASH | 自适应流 | 视频点播 | 需要合并 |

## 更多资源

- [API 文档](api_guide.md)
- [开发者指南](developer_guide.md)
- [常见问题](src/faq.md)
- [贡献指南](../CONTRIBUTING.md)
