[中文](index.zh.md) | **English**

# Falcon Downloader

**Falcon** is a cross-platform C++ downloader built as a monorepo. Its transfer foundation relies on open-source components such as libcurl, OpenSSL, libtorrent, and libssh2, all orchestrated around a unified engine that carries multi-protocol downloading.

## Why Falcon?

- **Concurrent downloads**: a C++17 event-driven engine with multi-threaded chunking and resumable transfers
- **Multi-protocol**: HTTP/HTTPS, FTP, SFTP, WebDAV, BitTorrent, and Metalink enabled by default, plus a variety of optional protocol plugins
- **Ease of use**: provides both a CLI tool and a C++ API
- **Extensible**: a plugin-based architecture that makes adding protocols or capabilities easy
- **Cross-platform**: targets Windows, Linux, and macOS

## Core Features

### 1. Multi-Protocol Downloads

Falcon currently focuses on composable protocol capabilities:

- **Core protocols**: HTTP/HTTPS, FTP/FTPS
- **P2P / extended protocols**: BitTorrent, ED2K
- **Proprietary-protocol plugins**: Thunder, QQ Whirlwind, FlashGet
- **Streaming-protocol plugins**: HLS, DASH

### 2. Concurrent Downloads and Resume

- **Multi-threaded chunked downloads**: splits files into chunks that are downloaded in parallel
- **Resume**: recovers interrupted tasks wherever range requests are supported
- **Speed control**: per-task speed limiting
- **Connection reuse**: reduces the overhead of repeated connection setup

### 3. Multiple Deployment Entry Points

- **CLI tool**: great for scripts and automation
- **Daemon service**: great for background jobs and remote management
- **Core libraries**: embeddable in other C++ projects
- **Desktop app / browser extension**: the code is already in the repository

## Project Architecture

```
falcon/
├── packages/
│   ├── libfalcon-core/      # Core download engine, task model, event system
│   ├── libfalcon-protocols/ # Standard download protocols
│   ├── libfalcon-storage/   # Object storage and remote resource browsing
│   ├── libfalcon-drives/    # Cloud drive and share-link capabilities
│   ├── falcon-cli/          # Command-line tool
│   └── falcon-daemon/       # Background daemon
├── apps/
│   ├── desktop/             # Desktop application
│   └── browser_extension/   # Browser extension
└── docs/                    # Project documentation
```

## Quick Start

### Install

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Basic Usage

```bash
# Download a single file
falcon-cli https://example.com/file.zip

# Multi-threaded download
falcon-cli https://example.com/large.zip -c 5

# Download a Thunder link
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# Read URLs from a file for batch downloading
falcon-cli -i urls.txt -j 3
```

### Using the C++ API

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

## Documentation Navigation

- [Getting Started](getting-started.md) - Get to know Falcon quickly
- [Installation Guide](installation.md) - Detailed build steps
- [Usage Guide](usage.md) - CLI and API usage
- [Protocol Support](/protocols/) - An overview of all supported protocols
- [Developer Guide](/developer/) - Contributing and plugin development
- [FAQ](../faq.md) - Answers to common questions

## Community

- **GitHub**: [https://github.com/cuihairu/falcon](https://github.com/cuihairu/falcon)
- **Issues**: [https://github.com/cuihairu/falcon/issues](https://github.com/cuihairu/falcon/issues)
- **Discussions**: [https://github.com/cuihairu/falcon/discussions](https://github.com/cuihairu/falcon/discussions)

## License

Apache License 2.0

---

::: tip Getting Help
If you run into problems, check the [FAQ](../faq.md) first, or ask in [GitHub Issues](https://github.com/cuihairu/falcon/issues).
:::
