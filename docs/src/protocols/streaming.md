# HLS/DASH 流媒体协议

Falcon 代码库中包含 HLS 相关插件实现，流媒体能力目前更适合视作“可选协议插件能力”，而不是已经稳定公开的高层 API。

## 当前状态

| 协议 | 代码库状态 | 默认构建 |
|------|------|------|
| HLS | 有插件实现 | 关闭 |
| DASH | 文档有规划说明 | 非稳定公共 API |

启用 HLS 插件示例：

```bash
cmake -B build -S . -DFALCON_ENABLE_HLS=ON
```

## 命令行示例

```bash
# HLS 播放列表
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# 主播放列表
falcon-cli "https://example.com/master.m3u8" -o best.mp4

# 通过代理并附加头部
falcon-cli "https://example.com/playlist.m3u8" \
  --proxy http://proxy:8080 \
  --header "Authorization: Bearer TOKEN"
```

## C++ API

当前公共头文件没有把 `HlsOptions` / `DashOptions` 作为稳定 API 暴露出来，因此更保守的写法是使用统一下载入口：

```cpp
#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.output_filename = "video.mp4";
    options.headers["Authorization"] = "Bearer TOKEN";

    auto task = engine.add_task(
        "https://example.com/playlist.m3u8",
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

## M3U8 示例

```txt
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-TARGETDURATION:10
#EXTINF:9.9,
segment0.ts
#EXTINF:9.9,
segment1.ts
#EXTINF:9.9,
segment2.ts
#EXT-X-ENDLIST
```

## FFmpeg 说明

如果具体实现需要合并媒体段，通常会依赖 FFmpeg。是否需要、如何集成，以实际插件实现为准。

```bash
ffmpeg -version
```

## 边界说明

- HLS/DASH 页面目前更偏“能力说明”而不是最终公共 API 文档。
- 若后续这些协议的专属选项进入公共头文件，再适合补充 `quality`、`stream_index`、`audio_language` 等字段说明。
