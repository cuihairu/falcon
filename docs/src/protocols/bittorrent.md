# BitTorrent Protocol

The Falcon codebase includes a BitTorrent plugin that handles magnet links and `.torrent` files. libtorrent owns the data plane, and the top-level CMake build enables the plugin by default (it degrades gracefully with a WARNING when libtorrent is missing).

## Enabling the plugin

```bash
cmake -B build -S . -DFALCON_ENABLE_BITTORRENT=ON
```

## Features

- Magnet links
- `.torrent` files
- Plugin implementations of DHT / LSD / PEX / UPnP and related capabilities
- Large-file distribution scenarios

## Command-line examples

```bash
# Magnet
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example"

# With a tracker
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# Remote torrent file
falcon-cli "https://example.com/file.torrent"

# Local torrent file
falcon-cli "file:///path/to/file.torrent"
```

## C++ API

The public headers do not currently expose `BittorrentOptions` as a stable API, so use the unified download entry point:

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

## Scope

- Selective downloading, seeding policy, upload limits, listening ports, and other fine-grained controls are internal plugin capabilities.
- Once these BitTorrent-specific options enter the public headers, they can be documented as stable public API.

## Magnet format

```text
magnet:?xt=urn:btih:<hash>&dn=<name>&tr=<tracker>
```

Common parameters:

| Parameter | Description |
|------|------|
| `xt` | Resource hash |
| `dn` | Display name |
| `tr` | Tracker address |
| `ws` | Web seed |
| `xl` | File size |

## FAQ

### Q: `magnet:` links are not supported?

A: Confirm that the build enabled `FALCON_ENABLE_BITTORRENT=ON`.

### Q: Why is the download slow?

A: Common causes include too few peers, few trackers, firewall restrictions, or unavailable port mapping.

### Q: Why is `BittorrentOptions` not public here?

A: The current public headers do not expose that struct as a stable API, so the documentation follows the interfaces that are actually public.
