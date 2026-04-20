# BitTorrent 协议

Falcon 代码库中包含 BitTorrent 插件实现，可处理 Magnet 链接和 `.torrent` 文件；该插件在顶层 CMake 中默认关闭。

## 启用方式

```bash
cmake -B build -S . -DFALCON_ENABLE_BITTORRENT=ON
```

## 特性

- ✅ Magnet 链接
- ✅ `.torrent` 文件
- ✅ DHT / LSD / PEX / UPnP 等能力的插件实现
- ✅ 大文件分发场景

## 命令行示例

```bash
# Magnet
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example"

# 带 tracker
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# 远程 torrent 文件
falcon-cli "https://example.com/file.torrent"

# 本地 torrent 文件
falcon-cli "file:///path/to/file.torrent"
```

## C++ API

当前公共头文件没有把 `BittorrentOptions` 作为稳定 API 暴露出来，因此推荐先使用统一下载入口：

```cpp
#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.output_directory = "./downloads";

    auto task = engine.add_task(
        "magnet:?xt=urn:btih:HASH&dn=Example",
        options
    );

    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## 边界说明

- 选择性下载、做种策略、上传限速、监听端口等更细粒度能力属于插件内部能力范围。
- 当这些 BitTorrent 专属配置真正进入公共头文件后，再适合把它们作为稳定 API 文档公开。

## Magnet 格式

```text
magnet:?xt=urn:btih:<hash>&dn=<name>&tr=<tracker>
```

常见参数：

| 参数 | 说明 |
|------|------|
| `xt` | 资源哈希 |
| `dn` | 显示名称 |
| `tr` | tracker 地址 |
| `ws` | Web seed |
| `xl` | 文件大小 |

## 常见问题

### Q: `magnet:` 链接不被支持？

A: 先确认构建时已开启 `FALCON_ENABLE_BITTORRENT=ON`。

### Q: 下载速度慢？

A: 常见原因包括 peers 不足、tracker 数量少、防火墙限制或端口映射不可用。

### Q: 为什么这里没有公开 `BittorrentOptions`？

A: 因为当前公共头文件并未把该结构体作为稳定 API 暴露出来，文档需要以已公开接口为准。
