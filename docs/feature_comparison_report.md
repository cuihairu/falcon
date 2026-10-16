[中文](feature_comparison_report.zh.md) | **English**

# Falcon Downloader vs aria2 Feature Comparison Report (Updated)

> [!NOTE]
> This document is a point-in-time analysis snapshot from 2025-12-27. It is not the current entry point for user documentation, and the statuses described here no longer track the codebase.
> Things have changed since the snapshot: the XML-RPC server has been removed and RPC is now an aria2-compatible JSON-RPC (with WebSocket event streaming on the same port); Metalink parsing has been replaced by vendored libmetalink; the SFTP data path depends on libssh2; and all file paths have moved as part of the four-library split. For current features and coverage, refer to [README](../README.md) and the CI badges.

**Generated**: 2025-12-27
**Falcon version**: 0.1.0
**aria2 reference version**: 1.37.0

---

## Overall Feature Completeness

A single completeness percentage tells you very little; see the comparison table below for the per-item status. The main changes since the snapshot are listed in the NOTE at the top.

---

## Summary of Added Features

### 1. BitTorrent Protocol Support

**Status**: Complete
**File locations**:
- `packages/libfalcon-protocols/plugins/bittorrent/bittorrent_plugin.hpp`
- `packages/libfalcon-protocols/plugins/bittorrent/bittorrent_plugin.cpp`

**Implemented features**:
- .torrent file parsing (Bencode)
- Magnet link support
- DHT (Distributed Hash Table)
- PEX (Peer Exchange)
- LSD (Local Service Discovery)
- File selection (multi-file torrents)
- Priority settings
- Speed limits
- Torrent creation (planned)
- Full unit tests

**Dependency**: libtorrent-rasterbar 2.0+

---

### 2. SFTP Protocol Support

**Status**: Complete
**File locations**:
- `packages/libfalcon-protocols/plugins/sftp/sftp_plugin.hpp`
- `packages/libfalcon-protocols/plugins/sftp/sftp_plugin.cpp`

**Implemented features**:
- SSH key authentication
- Password authentication
- Custom ports
- Resumable transfers
- Speed limits
- Full URL parsing
- Full unit tests

**Supported formats**:
```
sftp://user@host/path
sftp://user:pass@host:port/path
```

**Dependency**: libssh2

---

### 3. Metalink Protocol Support

**Status**: Complete (implementation rewritten after the snapshot: the parsing layer is now vendored libmetalink + expat, see metalink_handler in the package)
**File locations**:
- `packages/libfalcon-protocols/plugins/metalink/metalink_handler.hpp`
- `packages/libfalcon-protocols/plugins/metalink/metalink_handler.cpp`

**Implemented features**:
- Metalink v4 / Metalink3 file parsing
- Multi-source downloads (multi-mirror segmentation via the V2 engine)
- Priority ordering
- Checksum verification (SHA-256, SHA-1, MD5)
- Automatic failover
- XML parsing (vendored libmetalink)
- Full unit tests

**Characteristics**:
- Mirrors are tried in order of priority
- Automatic retry: switches to a backup source when one fails
- Hash verification: validates file integrity automatically once the download completes

---

### 4. XML-RPC Support (removed after the snapshot)

**Current state**: The XML-RPC server implementation described in the snapshot was later removed entirely. RPC is now an aria2-compatible
JSON-RPC (HTTP + WebSocket on the same port), covering 28 methods including aria2.addUri / aria2.remove /
aria2.pause / aria2.unpause / aria2.tellStatus / aria2.getGlobalStat /
aria2.getVersion / system.listMethods.

---

### 5. WebSocket Real-Time Communication

**Current state**: The standalone websocket_server prototype has been removed; WebSocket event streaming is now folded into the daemon
JSON-RPC on the same port (`ws://host:6800/jsonrpc`), where the server broadcasts aria2-compatible notifications
(onDownloadStart/Pause/Complete/Error/Stop) plus Falcon extension progress notifications.

---

### 6. Incremental Download

**Status**: Complete
**File locations**:
- `packages/libfalcon-protocols/include/falcon/incremental_download.hpp`
- `packages/libfalcon-protocols/src/incremental_download.cpp`

**Implemented features**:
- Hash-based diff comparison
- Chunk-level incremental updates
- Rsync algorithm support (framework)
- Multiple hash algorithms (SHA-256, SHA-1, MD5)
- File verification
- Patch application

**Use cases**:
- Large file updates (download only the changed parts)
- Version updates
- Data synchronization

The bandwidth savings depend on how much the file repeats itself, so there is no single percentage.

---

### 7. Shell Completion

**Status**: Complete
**File locations**:
- `packages/falcon-cli/completion/bash-completion.sh`
- `packages/falcon-cli/completion/zsh-completion.zsh`
- `packages/falcon-cli/completion/fish-completion.fish`
- `packages/falcon-cli/completion/README.md`

**Supported shells**:
- Bash
- Zsh
- Fish

**Completion features**:
- Command completion
- Option completion
- GID completion
- Directory completion
- URL completion
- Filename completion

---

## Full Feature Comparison Table

| Feature category | aria2 | Falcon | Notes |
|---------|-------|--------|------|
| **Multi-protocol support** | | | |
| HTTP/HTTPS | Yes | Yes | Common HTTP features supported |
| FTP | Yes | Yes | FTP support |
| SFTP | Yes | Yes | Based on libssh2 |
| BitTorrent | Yes | Yes | Based on libtorrent-rasterbar |
| Magnet links | Yes | Yes | |
| Metalink | Yes | Yes | v4 and Metalink3 |
| **Core download features** | | | |
| Multi-threaded segmentation | Yes | Yes | Adaptive segmentation supported |
| Resumable transfers | Yes | Yes | |
| Speed limiting | Yes | Yes | Global and per-task |
| Connection reuse | Yes | Yes | |
| Incremental download | No | Yes | Not available in aria2 |
| **Task management** | | | |
| Task queue | Yes | Yes | Priority queue |
| Task persistence | Yes | Yes | SQLite database |
| Automatic retry | Yes | Yes | Configurable retry count |
| File selection | Yes | Yes | BT file selection |
| **RPC interface** | | | |
| JSON-RPC | Yes | Yes | |
| XML-RPC | Yes | No | The snapshot implementation was removed and not redone |
| WebSocket | Yes | Yes | Real-time event push |
| **Command-line tool** | | | |
| Basic downloads | Yes | Yes | |
| Batch downloads | Yes | Yes | URL list support |
| Progress display | Yes | Yes | Real-time progress bar |
| Shell completion | Yes | Yes | Bash/Zsh/Fish |
| **Advanced features** | | | |
| URI parsing | Yes | Yes | Includes private protocols |
| Checksum verification | Yes | Yes | Multiple hash algorithms |
| DHT | Yes | Yes | |
| PEX | Yes | Yes | |
| LSD | Yes | Yes | |
| **Extended features** | | | |
| Cloud storage | No | Yes | S3/OSS/COS/Kodo |
| Private protocols | No | Yes | Thunder/QQDL/FlashGet |
| HLS/DASH | No | Yes | Streaming protocols |

---

## Features aria2 Does Not Have

The following are capabilities Falcon adds on top of aria2:

### Incremental Download
- Downloads only the parts of a file that changed
- Bandwidth savings depend on how much the file repeats itself
- Suited to files that are updated frequently

### Cloud Storage Support
- Amazon S3
- Alibaba Cloud OSS
- Tencent Cloud COS
- Qiniu Kodo
- Upyun

### Private Protocol Support
- Thunder thunder://
- QQDL qqlink://
- FlashGet flashget://
- ED2K

### Streaming Protocols
- HLS (HTTP Live Streaming)
- DASH (Dynamic Adaptive Streaming over HTTP)

### C++17 and the Four-Library Split
- Implemented in C++17, with the modules split into four libraries—core / protocols / storage / drives—with one-way dependencies
- Protocols register as plugins, so adding a new protocol does not require touching the engine code

---

## Dependency List

### Core Dependencies
| Library | Version | Purpose | Required/Optional |
|--------|------|------|----------|
| spdlog | 1.9+ | Logging | Required |
| nlohmann/json | 3.10+ | JSON parsing | Required |
| OpenSSL | 1.1+ | Encryption/hashing | Required |

### Protocol Plugin Dependencies
| Library | Version | Purpose | Required/Optional |
|--------|------|------|----------|
| libcurl | 7.68+ | HTTP/FTP | Recommended |
| libtorrent-rasterbar | 2.0+ | BitTorrent | Optional |
| libssh2 | 1.9+ | SFTP | Optional |

### RPC Dependencies
| Library | Version | Purpose | Required/Optional |
|--------|------|------|----------|
| nlohmann/json | 3.10+ | JSON-RPC | Required |
| Micro HTTP server | - | RPC server | Optional |

### Test Dependencies
| Library | Version | Purpose | Required/Optional |
|--------|------|------|----------|
| Google Test | 1.12+ | Unit tests | Recommended |
| Google Mock | 1.12+ | Mock tests | Recommended |

---

## Build Configuration

### Minimal Build (HTTP/HTTPS only)
```bash
cmake -B build -DFALCON_ENABLE_BITTORRENT=OFF \
                 -DFALCON_ENABLE_FTP=OFF \
                 -DFALCON_ENABLE_SFTP=OFF \
                 -DFALCON_ENABLE_METALINK=OFF
cmake --build build
```

### Full Build (all protocols)
```bash
cmake -B build -DFALCON_ENABLE_BITTORRENT=ON \
                 -DFALCON_ENABLE_FTP=ON \
                 -DFALCON_ENABLE_SFTP=ON \
                 -DFALCON_ENABLE_METALINK=ON \
                 -DFALCON_BUILD_DAEMON=ON \
                 -DFALCON_BUILD_TESTS=ON
cmake --build build
```

### With vcpkg Dependencies
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

---

## Test Coverage

There were no trustworthy coverage figures at the time of the snapshot, so the hand-written percentages have been removed. Coverage is now produced continuously by the CI Coverage job
(measured with gcovr); for the numbers, refer to the
[codecov badge](https://codecov.io/gh/cuihairu/falcon) and the CI logs.

---

## Performance Comparison

The repository has never run a controlled performance comparison (same hardware, same network, same task set); the percentage tables in the snapshot had no measurement conditions behind them, so they have been removed. The performance test cases in CI (such as PerformanceLargeFile)
serve only as regression thresholds and do not constitute a head-to-head comparison with aria2. If you need an evaluation, measure it yourself under controlled conditions.

---

## Usage Examples

### HTTP Download
```bash
# Simple download
falcon-cli https://example.com/file.zip

# Multi-connection segmentation
falcon-cli -x 8 -s 8 https://example.com/large-file.bin

# Experimental V2 engine
falcon-cli --http-engine v2 https://example.com/file.zip
```

### Metalink Download
```bash
# URLs ending in .meta4 / .metalink automatically go through the Metalink flow
falcon-cli https://example.com/file.meta4
```

### RPC Call
```bash
curl -d '{"jsonrpc":"2.0","method":"aria2.addUri","params":[["http://example.com/file"]],"id":1}' \
  http://localhost:6800/jsonrpc
```

For BitTorrent, SFTP, and other entry points along with the full set of arguments, refer to `falcon-cli --help`. The `falcon download` command form, the `--select-file` / `--incremental` arguments, and the
`metalink://` protocol from the original snapshot were never implemented and have been removed.

---

## Next Steps

This is the plan list from the time of the snapshot, kept for archival only; for current progress, refer to the README roadmap:

### Short Term (1-2 months)
1. Complete all missing features (done)
2. Performance optimization (connection pool, memory usage)
3. Improve documentation (API docs, user guide)
4. Release 1.0 Beta

### Medium Term (3-6 months)
1. GUI application (desktop, Qt6 desktop has since shipped)
2. Web management interface
3. Mobile support (Android/iOS)
4. Plugin marketplace

### Long Term (6-12 months)
1. Distributed downloads
2. Edge caching
3. AI-driven speed optimization
4. Commercial support services

---

## License

Apache License 2.0

---

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](../CONTRIBUTING.md) for details.

---

## Contact

- Project home: https://github.com/cuihairu/falcon
- Issue tracker: https://github.com/cuihairu/falcon/issues

---

**Last updated**: 2025-12-27
**Document version**: 1.0
