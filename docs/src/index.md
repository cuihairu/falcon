---
layout: home

hero:
  name: Falcon
  text: A Modern, Cross-Platform C++ Resource Transfer Engine
  tagline: Unified downloading and access for HTTP, FTP, SFTP, WebDAV, object storage, and P2P resources — delivered as a CLI, a daemon, and a desktop app
  image:
    src: /logo.svg
    alt: Falcon logo
  actions:
    - theme: brand
      text: Get Started
      link: /guide/getting-started
    - theme: alt
      text: Project Overview
      link: /guide/

features:
  - title: Multi-Protocol Support
    details: The transfer core ships with HTTP/HTTPS, FTP, SFTP, WebDAV, Metalink, and BitTorrent enabled by default, plus optional compatibility plugins for Thunder, QQ Whirlwind, FlashGet, ED2K, and HLS/DASH.
  - title: Event-Driven Engine
    details: Built on a C++17 event-driven architecture with multi-threaded chunked downloads, resumable transfers, and configurable concurrency control.
  - title: Multiple Entry Points
    details: The repository contains the core libraries, a CLI, a daemon, a desktop app, and a browser extension, so you can combine them to fit your scenario.
  - title: Plugin-Based
    details: Protocol handlers are organized as independent plugins, making it easy to add new protocols and keep implementation complexity isolated.
  - title: Easy to Integrate
    details: The libfalcon core libraries let you reuse the download capability in other C++ projects.
  - title: Open Source
    details: Released under the Apache 2.0 license.
---

[中文](index.zh.md) | **English**

The main-view design prototypes (warm console / cold utility) and live desktop
screenshots (download management, completed list, add-download dialog, cloud
storage, settings) rotate automatically — hovering pauses the carousel, and
you can switch with the left/right arrows, the dots, or the keyboard arrow
keys.

<ShowcaseCarousel />

> More UI screenshots (52 in total: 13 views × light/dark × two window
> widths) are available in the repository at
> [docs/design/ui-sandbox/](https://github.com/cuihairu/falcon/tree/main/docs/design/ui-sandbox).

## Quick Preview

### Install

```bash
# Build from source
git clone https://github.com/cuihairu/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The current documentation focuses on building from source; it no longer assumes that Homebrew, APT, or prebuilt installers have been published.

### Basic Usage

```bash
# Download a single file
falcon-cli https://example.com/file.zip

# Multi-threaded download
falcon-cli https://example.com/large.zip -c 5

# Read URLs from a file for batch downloading
falcon-cli -i urls.txt -j 3

# Download through a proxy
falcon-cli https://example.com/file.zip --proxy http://127.0.0.1:7890
```

### C++ API

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.max_connections = 5;

    auto task = engine.add_task(
        "https://example.com/file.zip",
        options
    );

    if (task) {
        engine.start_task(task->id());
        task->wait();
    }
    return 0;
}
```

## Project Structure

```
falcon/
├── packages/
│   ├── libfalcon-core/      # Core download engine, task model, event system
│   ├── libfalcon-protocols/ # Standard download protocols
│   ├── libfalcon-storage/   # Object storage and resource browsing
│   ├── libfalcon-drives/    # Cloud drive and share-link capabilities
│   ├── falcon-cli/          # Command-line tool
│   └── falcon-daemon/       # Background daemon
├── apps/
│   ├── desktop/             # Desktop application
│   └── browser_extension/   # Browser extension
└── docs/                    # Project documentation
```

## Protocol Support

| Protocol | Status | Notes |
|----------|--------|-------|
| HTTP/HTTPS | Enabled | Resumable and chunked downloads |
| FTP/FTPS | Enabled | Active/passive modes |
| BitTorrent | Enabled | libtorrent-based, with seeding, DHT, and NAT port mapping |
| ED2K | Optional | Implemented in the codebase, usually off in default builds |
| Thunder | Optional | Implemented in the codebase, usually off in default builds |
| QQ Whirlwind | Optional | Implemented in the codebase, usually off in default builds |
| FlashGet | Optional | Implemented in the codebase, usually off in default builds |
| HLS/DASH | Optional | Implemented in the codebase, usually off in default builds |

## Contributing

Contributions, bug reports, and suggestions are welcome!

- [GitHub Issues](https://github.com/cuihairu/falcon/issues)
- [Contributing Guide](/developer/)

## License

Apache License 2.0
