# BitTorrent 协议

Falcon 支持 BitTorrent 协议，包括 Magnet 链接和 .torrent 文件。

## 特性

- ✅ **Magnet 链接**：支持 `magnet:` 协议
- ✅ **Torrent 文件**：解析 .torrent 文件
- ✅ **DHT**：分布式哈希表
- ✅ **LSD**：本地服务发现
- ✅ **UPnP/NAT-PMP**：端口映射
- ✅ **PEX**：peer 交换
- ✅ **加密**：协议加密
- ✅ **选择性下载**：选择特定文件
- ✅ **做种**：下载完成后继续上传

## 命令行使用

### Magnet 链接

```bash
# 基本使用
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example"

# 带 tracker
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# 带 display name
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&dn=MyFile"
```

### Torrent 文件

```bash
# 下载 .torrent 文件
falcon-cli https://example.com/file.torrent

# 本地 .torrent 文件
falcon-cli file:///path/to/file.torrent
```

### 指定保存目录

```bash
falcon-cli "magnet:?xt=urn:btih:HASH" -d ~/Downloads/torrents
```

### 选择性下载

```bash
# 仅下载特定文件
falcon-cli "magnet:?xt=urn:btih:HASH" --select-files "file1.txt,file2.txt"
```

### 做种设置

```bash
# 下载后继续做种
falcon-cli "magnet:?xt=urn:btih:HASH" --seed

# 做种比率
falcon-cli "magnet:?xt=urn:btih:HASH" --seed-ratio 2.0

# 做种时间（分钟）
falcon-cli "magnet:?xt=urn:btih:HASH" --seed-time 60
```

### 速度限制

```bash
# 下载速度限制
falcon-cli "magnet:?xt=urn:btih:HASH" --download-limit 1M

# 上传速度限制
falcon-cli "magnet:?xt=urn:btih:HASH" --upload-limit 500K
```

### 端口设置

```bash
# 设置监听端口
falcon-cli "magnet:?xt=urn:btih:HASH" --port 6881
```

## C++ API 使用

### 基本下载

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("bittorrent");

    falcon::BittorrentOptions bt_options;
    bt_options.save_path = "./downloads";
    bt_options.sequential_download = false;

    falcon::DownloadOptions options;
    options.bt_options = bt_options;

    auto task = engine.startDownload(
        "magnet:?xt=urn:btih:HASH&dn=Example",
        options
    );

    task->wait();
    return 0;
}
```

### Torrent 文件下载

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("bittorrent");

    falcon::BittorrentOptions bt_options;
    bt_options.save_path = "./downloads";

    falcon::DownloadOptions options;
    options.bt_options = bt_options;

    auto task = engine.startDownload(
        "https://example.com/file.torrent",
        options
    );

    task->wait();
    return 0;
}
```

### 选择性下载

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("bittorrent");

    falcon::BittorrentOptions bt_options;
    bt_options.save_path = "./downloads";
    bt_options.selected_files = {"file1.txt", "video.mp4"};

    falcon::DownloadOptions options;
    options.bt_options = bt_options;

    auto task = engine.startDownload(
        "magnet:?xt=urn:btih:HASH",
        options
    );

    task->wait();
    return 0;
}
```

### 做种配置

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("bittorrent");

    falcon::BittorrentOptions bt_options;
    bt_options.save_path = "./downloads";
    bt_options.seed_ratio = 2.0;  // 上传/下载比率达到 2.0 时停止
    bt_options.seed_time = std::chrono::minutes(60);  // 最多做种 60 分钟

    falcon::DownloadOptions options;
    options.bt_options = bt_options;

    auto task = engine.startDownload(
        "magnet:?xt=urn:btih:HASH",
        options
    );

    task->wait();
    return 0;
}
```

## BitTorrent 选项

### 连接选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `port` | int | 6881 | 监听端口 |
| `enable_dht` | bool | true | 启用 DHT |
| `enable_lsd` | bool | true | 启用本地服务发现 |
| `enable_upnp` | bool | true | 启用 UPnP 端口映射 |
| `enable_pex` | bool | true | 启用 PEX |
| `max_connections` | int | 500 | 最大连接数 |

### 传输选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `save_path` | string | "./downloads" | 保存目录 |
| `sequential_download` | bool | false | 顺序下载 |
| `selected_files` | vector<string> | all | 选择下载的文件 |
| `download_limit` | int64_t | 0 | 下载速度限制（0=无限制） |
| `upload_limit` | int64_t | 0 | 上传速度限制（0=无限制） |

### 做种选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `seed_mode` | bool | false | 下载后继续做种 |
| `seed_ratio` | float | 0 | 做种比率（0=无限制） |
| `seed_time` | chrono::minutes | 0 | 做种时间（0=无限制） |

## Magnet 链接格式

### 基本格式

```
magnet:?xt=urn:btih:<hash>&dn=<name>&tr=<tracker>
```

### 参数说明

| 参数 | 说明 | 必需 |
|------|------|------|
| `xt` | 准确主题（包含 hash） | ✅ |
| `dn` | 显示名称 | ❌ |
| `tr` | tracker URL | ❌ |
| `ws` | 网络种子（HTTP） | ❌ |
| `xl` | 文件大小（字节） | ❌ |

### 示例

```bash
# 最简单的 magnet 链接
magnet:?xt=urn:btih:ABCDEF123456789

# 带 tracker
magnet:?xt=urn:btih:ABCDEF123456789&tr=http://tracker.example.com:8080/announce

# 多个 tracker
magnet:?xt=urn:btih:ABCDEF123456789&tr=tracker1.com&tr=tracker2.com

# 带名称
magnet:?xt=urn:btih:ABCDEF123456789&dn=MyFile
```

## 常见问题

### Q: 下载速度很慢

A: 可能的原因：
- 没有 peers：检查 tracker 是否可用
- 防火墙阻止：确保端口 6881 可访问
- 启用 UPnP 或手动端口映射

### Q: 无法连接到 tracker

A: Tracker 可能已关闭或需要认证。尝试：
- 添加更多 tracker
- 启用 DHT（不需要 tracker）
- 检查防火墙设置

### Q: 下载卡住不动

A: 检查：
- 是否有可用的 peers
- tracker 是否响应
- 磁盘空间是否足够

### Q: "Stalled" 状态

A: 这是正常的，表示当前没有连接到任何 peer。可以：
- 等待 DHT 发现更多 peers
- 手动添加 tracker
- 确认 torrent 是否还有人在做种

## 性能优化

### 提高下载速度

```cpp
falcon::BittorrentOptions bt_options;
bt_options.max_connections = 1000;       // 增加最大连接数
bt_options.enable_dht = true;           // 启用 DHT
bt_options.enable_lsd = true;           // 启用本地发现
bt_options.enable_pex = true;           // 启用 PEX
```

### 限制上传速度

```cpp
falcon::BittorrentOptions bt_options;
bt_options.upload_limit = 100 * 1024;   // 限制为 100KB/s
```

::: tip 最佳实践
- 家庭网络：最多 500 个连接
- 服务器网络：最多 1000 个连接
- 做种比率：建议 1.5 - 2.0
:::
