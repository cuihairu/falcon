# HLS/DASH Streaming Protocol

The Falcon codebase includes an HLS-related plugin. Streaming support is currently better understood as an optional protocol-plugin capability than as a stable high-level public API.

## Current status

| Protocol | Codebase status | Default build |
|------|------|------|
| HLS | Plugin implementation available | Off |
| DASH | Documented design only | Not a stable public API |

Example of enabling the HLS plugin:

```bash
cmake -B build -S . -DFALCON_ENABLE_HLS=ON
```

## Command-line examples

```bash
# HLS playlist
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# Through a proxy with an added header
falcon-cli "https://example.com/playlist.m3u8" \
  --proxy http://proxy:8080 \
  --header "Authorization: Bearer TOKEN"
```

> The current plugin does not implement master playlists (`master.m3u8`, multi-bitrate variant selection). Use the media playlist directly.

## C++ API

The current public headers do not expose `HlsOptions` / `DashOptions` as stable APIs, so the conservative approach is the unified download entry point:

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

## M3U8 example

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

## FFmpeg notes

If a specific implementation needs to merge media segments, it usually depends on FFmpeg. Whether it is required and how it is integrated depends on the actual plugin implementation.

```bash
ffmpeg -version
```

## Scope

- The HLS/DASH pages currently describe capabilities rather than a final public API.
- If protocol-specific options enter the public headers later, fields such as `quality`, `stream_index`, and `audio_language` can be documented then.
