<div align="center">

  <img src="./assets/falcon.png" alt="Falcon Logo" width="200"/>

  # Falcon Downloader

  [![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
  [![Build Status](https://github.com/cuihairu/falcon/workflows/CMake%20Build/badge.svg)](https://github.com/cuihairu/falcon/actions)
  [![codecov](https://codecov.io/gh/cuihairu/falcon/branch/main/graph/badge.svg)](https://codecov.io/gh/cuihairu/falcon)
  [![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/cuihairu/falcon)
  [![Version](https://img.shields.io/badge/version-0.1.0-green.svg)](https://github.com/cuihairu/falcon/releases)

  **A modern, high-performance, cross-platform download accelerator**

</div>

## Features 🚀

- **aria2-Compatible Architecture**:
  - Event-driven command pattern
  - I/O multiplexing (epoll/kqueue/poll)
  - Socket connection pooling
  - Request group management
- **Multi-Protocol Support**: HTTP/HTTPS, FTP, Metalink, BitTorrent, Magnet links, Private protocols
  - Thunder (迅雷)
  - QQDL (腾讯旋风)
  - FlashGet
  - ED2K (电驴)
  - HLS/DASH streaming
- **Daemon & RPC Service** (`falcon-daemon`): aria2-compatible JSON-RPC over HTTP +
  WebSocket event stream, task persistence (SQLite), `daemon.json` config with SIGHUP
  reload
- **Desktop Application** (Qt6): Fluent-design UI, light/dark themes, table & grid task
  views, cloud storage browsing, daemon RPC backend
- **High Performance**:
  - Multi-threaded segmented downloading
  - Non-blocking I/O with event-driven architecture
  - Connection pooling for reduced latency
  - Speed control and bandwidth throttling
- **Advanced Features**:
  - File hash verification (MD5/SHA1/SHA256/SHA512)
  - Resume support for interrupted downloads
  - Multi-mirror failover
  - 30+ aria2-compatible CLI parameters
- **Cloud Storage Integration**:
  - Amazon S3
  - Alibaba Cloud OSS (阿里云OSS)
  - Tencent COS (腾讯云)
  - Qiniu Kodo (七牛云)
  - Upyun USS (又拍云)
- **Remote Resource Browsing**: Browse FTP/SFTP/S3 directories with rich information
- **Resource Search**: Built-in search engine for torrent and file resources
- **Secure Configuration**: AES-256 encrypted credential storage with master password protection
- **Proxy Support**: HTTP/HTTPS/SOCKS5 proxy support

## Quick Start ⚡

### Installation

```bash
# Clone repository
git clone https://github.com/cuihairu/falcon.git
cd falcon

# Build from source
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

The current documented path is source build. Package manager distribution and prebuilt installers should be treated as future release work unless a tagged release says otherwise.

### Basic Usage

```bash
# Simple download
falcon-cli https://example.com/file.zip

# Multi-threaded download (5 connections)
falcon-cli -x 5 https://example.com/large_file.iso

# Download with speed limit (1MB/s)
falcon-cli --max-download-limit=1M https://example.com/video.mp4

# Resume behavior is enabled by default; explicit aria2-style switch is also supported
falcon-cli --continue true https://example.com/partial.zip

# Batch input with concurrent tasks
falcon-cli -i urls.txt -j 3

# Custom headers and proxy
falcon-cli --proxy http://127.0.0.1:7890 \
  -H "Authorization: Bearer TOKEN" \
  https://example.com/file.bin
```

### aria2-Compatible Parameters

Falcon CLI supports aria2-compatible parameters:

```bash
# Connection settings
falcon-cli -x 16 -s 16 --min-split-size=1M https://example.com/file.zip
# -x: Max connections per task
# -s: Max connections per server

# Retry settings
falcon-cli -r 5 --retry-wait 10 https://example.com/file.zip

# Timeout settings
falcon-cli --timeout 30 https://example.com/file.zip

# HTTP authentication
falcon-cli --http-user=user --http-passwd=pass https://example.com/file.zip

# Custom headers
falcon-cli --header="Authorization: Bearer TOKEN" https://example.com/api/file

# Proxy support
falcon-cli --proxy http://proxy.example.com:8080 \
  --proxy-user user --proxy-passwd pass https://example.com/file.zip

# User agent
falcon-cli --user-agent="Falcon/1.0" https://example.com/file.zip

# Output directory
falcon-cli -d /tmp/downloads -o custom_name.zip https://example.com/file.zip

# Speed limits
falcon-cli --max-download-limit 5M https://example.com/file.zip
```

## Supported Protocols 📡

| Protocol | Status | Description |
|----------|--------|-------------|
| HTTP/HTTPS | Enabled | Standard web protocols with resume support |
| FTP/FTPS | Enabled | File Transfer Protocol with passive mode |
| Metalink | Enabled | RFC 5854 `.meta4` / Metalink3 `.metalink` mirror lists with whole-file hash verification |
| SFTP | Optional plugin | Implemented in repo but disabled by default in top-level CMake |
| BitTorrent | Optional plugin | Implemented in repo but disabled by default in top-level CMake |
| Thunder | Optional plugin | Implemented in repo but disabled by default in top-level CMake |
| QQDL | Optional plugin | Implemented in repo but disabled by default in top-level CMake |
| FlashGet | Optional plugin | Implemented in repo but disabled by default in top-level CMake |
| ED2K | Optional plugin | Implemented in repo but disabled by default in top-level CMake |
| HLS/DASH | Optional plugin | Implemented in repo but disabled by default in top-level CMake |

## Cloud Storage Support ☁️

Library-level browsing is implemented for Amazon S3, Alibaba Cloud OSS, Tencent COS,
Qiniu Kodo, and Upyun (`packages/libfalcon-storage`): listing, tree views, object info,
mkdir / rename / recursive delete, quota queries, and custom-endpoint (MinIO /
private-gateway) support. The desktop application ships a cloud storage page backed by
these modules.

> **Note**: the CLI currently does not expose storage-browsing commands. Flags like
> `--list`, `--tree`, `--search`, `--add-config`, `--set-master-password` shown in some
> older examples are **not implemented** — use the desktop GUI or call the libraries
> directly.

## Secure Configuration 🔐

`libfalcon-drives` provides an encrypted configuration manager (SQLite-backed,
AES-256-GCM credential encryption protected by a master password). The desktop
application uses it in its settings page. CLI management commands are not exposed yet.

## Advanced Features ⚙️

### Resource Search
`libfalcon-drives` includes a search provider framework (`resource_search`): a provider
interface with URL validation, magnet-link parsing, filtering/sorting/truncation, and a
generic crawler-backed provider. CLI search commands are not implemented yet; see the
library API for integration.

### Remote Directory Browsing
`libfalcon-storage` implements the `ResourceBrowser` interface for FTP, SFTP, S3, OSS,
COS, Kodo, and Upyun — format-tree/table listings, path validation, and recursive
operations. The desktop cloud storage page is built on it.

### Download Management
```bash
# Batch download from file
falcon-cli --input urls.txt

# Resume interrupted download (enabled by default; explicit aria2-style switch)
falcon-cli --continue true https://example.com/partial.zip

# Custom headers and user agent
falcon-cli https://example.com/file.bin \
  --header "Authorization: Bearer TOKEN" \
  --user-agent "Falcon/1.0"

# Proxy support
falcon-cli https://example.com/file.zip \
  --proxy http://proxy.example.com:8080 \
  --proxy-user user \
  --proxy-passwd pass
```

## Architecture 🏗️

Falcon follows a modular architecture with aria2-inspired event-driven design:

```
┌─────────────────────────────────────────────┐
│              Applications                   │
│  falcon-cli · falcon-daemon · desktop (Qt6) │
└──────────────────────┬──────────────────────┘
                      │
┌──────────────────────▼──────────────────────┐
│         Falcon Core (aria2-style)           │
│  ┌───────────────────────────────────┐      │
│  │  DownloadEngineV2 (Event Loop)    │      │
│  │  ├─ Command Queue                 │      │
│  │  ├─ Routine Commands              │      │
│  │  └─ Event Poll (epoll/kqueue)     │      │
│  ├───────────────────────────────────┤      │
│  │  RequestGroupMan                  │      │
│  │  ├─ Active Tasks                  │      │
│  │  └─ Waiting Queue                 │      │
│  ├───────────────────────────────────┤      │
│  │  Socket Pool                      │      │
│  │  └─ Connection Reuse              │      │
│  └───────────────────────────────────┘      │
└──────────────────────┬──────────────────────┘
                      │
┌──────────────────────▼──────────────────────┐
│              Protocol Plugins               │
│  ┌─────┐ ┌────┐ ┌────┐ ┌────────┐ ┌─────┐   │
│  │ HTTP│ │ FTP│ │ BT │ │Metalink│ │ ... │   │
│  └─────┘ └────┘ └────┘ └────────┘ └─────┘   │
└─────────────────────────────────────────────┘
```

### Event-Driven Architecture

Falcon uses an event-driven command pattern inspired by aria2:

1. **Command Execution**: All download operations are encapsulated as command objects
2. **I/O Multiplexing**: Uses epoll (Linux), kqueue (macOS/BSD), or poll (fallback)
3. **Connection Pooling**: Reuses HTTP/HTTPS connections for better performance
4. **Non-Blocking I/O**: All sockets are non-blocking, driven by events

### Documentation

- [Architecture Overview](docs/aria2_architecture.md) - Detailed architecture documentation
- [API Guide](docs/api_guide.md) - Complete API usage guide
- [Developer Guide](docs/developer_guide.md) - Development setup and guidelines
- [Migration Plan](docs/aria2c-migration-plan.md) - aria2 compatibility roadmap

## Development 👷

### Prerequisites
- CMake 3.15+
- C++17 compatible compiler (GCC 11+ / Clang 14+ / MSVC 2019+ recommended)
- libcurl 7.68+
- OpenSSL 1.1.1+
- nlohmann/json 3.10+
- spdlog 1.9+
- SQLite 3.35+
- libtorrent-rasterbar 2.0+ (optional, BitTorrent plugin)
- Qt 6 (optional, desktop application)

### Build Options
```bash
# Enable/disable features
cmake -B build -S . \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_CLOUD_STORAGE=ON \
  -DFALCON_ENABLE_RESOURCE_BROWSER=ON \
  -DFALCON_ENABLE_RESOURCE_SEARCH=ON
```

## Contributing 🤝

We welcome contributions! Please see our [Contributing Guide](CONTRIBUTING.md) for details.

### Code Style
- Follow Google C++ Style Guide
- Use `clang-format` for code formatting
- Write unit tests for new features

## License 📄

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

## Acknowledgments 🙏

- [libcurl](https://curl.se/) for HTTP/FTP/SFTP support
- [libtorrent](https://www.libtorrent.org/) for BitTorrent support
- [nlohmann/json](https://github.com/nlohmann/json) for JSON handling
- [spdlog](https://github.com/gabime/spdlog) for logging
- [OpenSSL](https://www.openssl.org/) for cryptographic operations
- [SQLite](https://sqlite.org/) for task persistence and configuration storage
- [Qt 6](https://www.qt.io/) for the desktop application

---

<div align="center">
  Made with ❤️ by the Falcon Team

  [中文文档](README_CN.md)
</div>
