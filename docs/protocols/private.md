# Private Protocols

The Falcon codebase contains implementations of five optional protocol plugins: Thunder, QQ Xuanfeng, FlashGet, ED2K, and HLS/DASH. They are disabled by default in the top-level CMake build.

## Support matrix

| Protocol | Common schemes | Default build |
|------|------|------|
| Thunder | `thunder://`, `thunderxl://` | Off |
| QQ Xuanfeng | `qqlink://`, `qqdl://` | Off |
| FlashGet | `flashget://`, `fg://` | Off |
| ED2K | `ed2k://` | Off |
| HLS/DASH | `.m3u8` / `.mpd` playlist links | Off |

To enable them in the current build, turn on the corresponding switches explicitly at configure time:

```bash
cmake -B build -S . \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON
```

## Command-line examples

```bash
# Thunder
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# QQ Xuanfeng
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# FlashGet
falcon-cli "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw"

# ED2K
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"
```

## C++ API

For callers, the entry point for private protocols is still the unified `add_task()`:

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    auto task = engine.add_task(
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
    );

    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## Encoding notes

### Thunder

- The common `thunder://` format Base64-encodes the original URL wrapped in `AA` / `ZZ`.
- The common `thunderxl://` format Base64-encodes the original URL directly.

### QQ Xuanfeng

- Usually `qqlink://BASE64`.
- `qqlink://GID|filename` forms may also appear.

### FlashGet

- Usually `fg://BASE64`.
- `flashget://...` forms may also appear.

### HLS/DASH

- `.m3u8` (HLS) and `.mpd` (DASH) playlist links: the plugin downloads segments and merges them.
- Segment addresses inside a playlist are usually relative and are resolved against the playlist URL.

## Caveats

- Private protocols are generally wrappers around the original URL; they do not magically add origin bandwidth.
- If the plugin is not compiled in, `DownloadEngine::is_url_supported()` returns `false`.
- Private-protocol sites often also require cookies, signatures, or time-limited tokens; merely decoding the URL may not be enough.
