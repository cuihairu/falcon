# HLS/DASH 流媒体协议

Falcon 支持 HLS 和 DASH 自适应流媒体协议，可用于下载视频流。

## HLS (HTTP Live Streaming)

### 特性

- ✅ **M3U8 播放列表**：支持主播放列表和变体播放列表
- ✅ **自动选择质量**：根据带宽选择最佳质量
- ✅ **加密流**：支持 AES-128 加密
- ✅ **自动合并**：使用 FFmpeg 合并媒体段
- ✅ **断点续传**：支持中断后继续下载

### 命令行使用

```bash
# 下载 HLS 流
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# 自动选择最佳质量
falcon-cli "https://example.com/master.m3u8" -o best.mp4

# 指定质量
falcon-cli "https://example.com/master.m3u8" -o 1080p.mp4 --quality 1080
```

### HLS 选项

```bash
# 指定质量
falcon-cli "https://example.com/master.m3u8" --quality 720

# 仅下载特定流
falcon-cli "https://example.com/master.m3u8" --stream-index 0

# 使用代理
falcon-cli "https://example.com/playlist.m3u8" --proxy http://proxy:8080

# 添加 HTTP 头部
falcon-cli "https://example.com/playlist.m3u8" --header "Authorization: Bearer TOKEN"
```

### C++ API

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("hls");

    falcon::HlsOptions hls_options;
    hls_options.output_file = "./video.mp4";
    hls_options.quality = "1080";  // 指定质量
    hls_options.auto_select = true;  // 自动选择最佳质量

    falcon::DownloadOptions options;
    options.hls_options = hls_options;

    auto task = engine.startDownload(
        "https://example.com/playlist.m3u8",
        options
    );

    task->wait();
    return 0;
}
```

### M3U8 格式

#### 简单播放列表

```m3u8
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

#### 主播放列表（多质量）

```m3u8
#EXTM3U
#EXT-X-VERSION:3
#EXT-X-STREAM-INF:BANDWIDTH=1280000,RESOLUTION=1280x720
720p.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=2560000,RESOLUTION=1920x1080
1080p.m3u8
#EXT-X-STREAM-INF:BANDWIDTH=512000,RESOLUTION=640x360
360p.m3u8
```

---

## DASH (Dynamic Adaptive Streaming)

### 特性

- ✅ **MPD 清单**：支持 on-demand 和 live 模式
- ✅ **多质量**：自动选择最佳质量
- ✅ **多音轨**：支持选择音轨
- ✅ **自适应**：根据网络条件调整

### 命令行使用

```bash
# 下载 DASH 流
falcon-cli "https://example.com/manifest.mpd" -o video.mp4

# 指定视频质量
falcon-cli "https://example.com/manifest.mpd" -o video.mp4 --video-quality 1080

# 选择音轨
falcon-cli "https://example.com/manifest.mpd" -o video.mp4 --audio-lang zh
```

### C++ API

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("dash");

    falcon::DashOptions dash_options;
    dash_options.output_file = "./video.mp4";
    dash_options.video_quality = "1080";
    dash_options.audio_language = "zh";

    falcon::DownloadOptions options;
    options.dash_options = dash_options;

    auto task = engine.startDownload(
        "https://example.com/manifest.mpd",
        options
    );

    task->wait();
    return 0;
}
```

---

## 流媒体选项

### HLS 选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `quality` | string | "auto" | 视频质量（720、1080 等） |
| `auto_select` | bool | true | 自动选择最佳质量 |
| `stream_index` | int | -1 | 指定流索引 |
| `encryption_key` | string | "" | AES-128 密钥 |
| `merge_segments` | bool | true | 合并媒体段 |

### DASH 选项

| 选项 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `video_quality` | string | "auto" | 视频质量 |
| `audio_language` | string | "und" | 音频语言 |
| `subtitle_language` | string | "" | 字幕语言 |
| `all_subtitles` | bool | false | 下载所有字幕 |

---

## FFmpeg 集成

Falcon 使用 FFmpeg 合并下载的媒体段。

### 安装 FFmpeg

```bash
# macOS
brew install ffmpeg

# Ubuntu/Debian
sudo apt install ffmpeg

# Windows
# 从 https://ffmpeg.org/download.html 下载
```

### 配置 FFmpeg 路径

```cpp
falcon::HlsOptions hls_options;
hls_options.ffmpeg_path = "/usr/local/bin/ffmpeg";
hls_options.merge_cmd = "{ffmpeg} -i {input} -c copy {output}";
```

---

## 加密流处理

### AES-128 加密

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("hls");

    falcon::HlsOptions hls_options;
    hls_options.output_file = "./video.mp4";
    hls_options.encryption_key = "YOUR_16_BYTE_KEY";
    hls_options.encryption_iv = "YOUR_IV";

    falcon::DownloadOptions options;
    options.hls_options = hls_options;

    auto task = engine.startDownload(
        "https://example.com/encrypted.m3u8",
        options
    );

    task->wait();
    return 0;
}
```

### 从密钥文件获取

```cpp
// Falcon 会自动从 #EXT-X-KEY 标签中获取密钥
// 示例 M3U8:
// #EXT-X-KEY:METHOD=AES-128,URI="https://example.com/key.key",IV=0x12345678
```

---

## 常见问题

### Q: FFmpeg 未找到

A: 确保 FFmpeg 已安装并在 PATH 中：

```bash
# 检查 FFmpeg
ffmpeg -version
```

### Q: 下载速度慢

A: 可能的原因：
- 服务器限速
- 网络带宽不足
- 尝试降低视频质量

### Q: 合并失败

A: 检查：
- FFmpeg 是否正确安装
- 媒体段是否完整下载
- 磁盘空间是否足够

### Q: 加密流无法播放

A: 确保密钥正确。有些网站需要额外的认证才能获取密钥。

::: tip 最佳实践
- 使用 `--quality auto` 自动选择最佳质量
- 对于长时间视频，使用断点续传
- 检查磁盘空间，媒体段可能占用大量空间
:::
