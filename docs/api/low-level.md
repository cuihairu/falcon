[简体中文](/zh/api/low-level) | **English**

# Low-Level Utilities

The public C++ API is centred on `DownloadEngine` and `add_task()` (see [Core API](/api/core)). A few lower-level helpers remain for integrity checks and custom protocol handlers; ordinary users do not need them. The former socket-pool and event-poll utilities were removed with the single-engine cleanup and are no longer part of the public API.

> The headers live in `libs/protocols/include/falcon/`. Prefer the engine API unless you are writing a protocol handler or an integration test.

## File hashing

`FileHasher` computes and verifies whole-file hashes. It is used internally by the engine for integrity checks and Metalink publication.

```cpp
#include <falcon/file_hash.hpp>

using namespace falcon;

auto hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA256);
auto result = FileHasher::verify(
    "downloaded_file.zip", expected_hex, HashAlgorithm::SHA256);
if (result.valid) {
    // integrity confirmed
} else {
    // result.expected / result.calculated describe the mismatch
}
```

- Available algorithms: MD5, SHA1, SHA256, SHA512.
- `FileHasher::detect_algorithm(hash_string)` infers the algorithm from digest length.
- `FileHasher::verify_multiple(path, vector<pair<string, HashAlgorithm>>)` verifies a file against several candidate digests.

## Mirror fallback

Metalink documents may list several mirrors for the same content. The single V1 (libcurl) engine tries mirrors serially and verifies the file before publishing it. This is mirror fallback, not multi-source segmented downloading: segment-level source switching, If-Range and timeout sweeping are not enabled today. P2SP discovery is likewise discovery-only; see [P2SP Sharing Network](/developer/p2sp-network).

## Performance notes

- Set `options.max_connections` to match bandwidth: 4 on slow links, 8–16 on fast ones.
- libcurl reuses HTTP connections where the handler supports it; no separate pool configuration is required.
- Raise `options.min_segment_size` so tiny files are not split wastefully.
- Use `options.speed_limit` (per task) or `daemon.json` / RPC `max-overall-download-limit` (global, daemon only) to avoid saturating a link.

## See also

- [Core API](/api/core) — engine, task, options and status types
- [Events API](/api/events) — listeners and progress reporting
- [Plugin API](/api/plugins) — writing a protocol handler
- [P2SP Sharing Network](/developer/p2sp-network) — node discovery and source sharing
