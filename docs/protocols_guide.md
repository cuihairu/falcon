[中文](protocols_guide.zh.md) | **English**

# Falcon Downloader - Protocol Support Guide

> [!WARNING]
> This document is part of the legacy protocol documentation archive.
> For the currently maintained protocol documentation, refer to the pages under `docs/src/protocols/`.

This document provides a detailed overview of all download protocols supported by Falcon Downloader and how to use them.

## Table of Contents

- [Basic Protocols](#basic-protocols)
- [Private Protocols](#private-protocols)
- [Streaming Protocols](#streaming-protocols)
- [P2P Protocols](#p2p-protocols)
- [Usage Examples](#usage-examples)
- [Configuration](#configuration)
- [Troubleshooting](#troubleshooting)

## Basic Protocols

### HTTP/HTTPS

Falcon supports the full HTTP/HTTPS protocol, including:

- **Resume support**: Automatically detects server support and allows downloads to continue after interruption
- **Segmented download**: Multi-threaded concurrent downloads to improve large file download speeds
- **Speed control**: Supports both global and per-task speed limits
- **Custom headers**: Set User-Agent and other HTTP headers
- **Proxy support**: Supports HTTP/HTTPS/SOCKS proxies
- **Authentication support**: Basic authentication and digest authentication

```bash
# Basic download
falcon-cli https://example.com/file.zip

# Multi-threaded download (5 connections)
falcon-cli https://example.com/large.zip -c 5

# Resume download
falcon-cli https://example.com/file.zip --continue

# Speed limit
falcon-cli https://example.com/file.zip --limit 1M

# Use a proxy
falcon-cli https://example.com/file.zip --proxy http://proxy:8080
```

### FTP

Supports FTP and FTPS protocols, including both active and passive modes.

```bash
# Download an FTP file
falcon-cli ftp://ftp.example.com/file.zip

# With username and password
falcon-cli ftp://user:pass@ftp.example.com/file.zip

# FTPS (explicit TLS)
falcon-cli ftpes://ftp.example.com/file.zip
```

## Private Protocols

Falcon supports private protocols from several commonly used download tools in China:

### Thunder

Supports `thunder://` and `thunderxl://` link formats.

```bash
# Classic Thunder link
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# Thunder offline link
falcon-cli "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
```

### QQDL

Supports `qqlink://` and `qqdl://` formats.

```bash
# QQDL link
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# Link with GID
falcon-cli "qqlink://1234567890ABCDEF|video.mp4"
```

### FlashGet

Supports `flashget://` and `fg://` formats.

```bash
# Full format (with referrer)
falcon-cli "flashget://[URL]&ref=http://example.com"

# Short format
falcon-cli "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw"

# Base64-encoded format
falcon-cli "flashget://[FLASHGET]aHR0cDovL2V4YW1wbGUuY29tLw=="
```

### ED2K (Donkey)

Supports `ed2k://` format, including file links and server links.

```bash
# File link
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# File link with sources
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|s,192.168.1.1:4662|s,192.168.1.2:4662"

# With AICH checksum
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|h=ABCDEF123456789"

# Server link (used by server plugin only)
falcon-cli "ed2k://server|server.example.com|4242|Main Server/"
```

## Streaming Protocols

### HLS (HTTP Live Streaming)

Supports `.m3u8` playlists, automatically downloading all media segments and merging them.

```bash
# Download an HLS stream
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# Auto-select the best quality
falcon-cli "https://example.com/master.m3u8" -o best.mp4
```

HLS features:
- Automatically parses master and variant playlists
- Supports encrypted streams (key required)
- Auto-selects the best bandwidth
- Uses FFmpeg to merge media segments

### DASH (Dynamic Adaptive Streaming over HTTP)

Supports `.mpd` manifest files.

```bash
# Download a DASH stream
falcon-cli "https://example.com/manifest.mpd" -o video.mp4"

# Download a specific video quality
falcon-cli "https://example.com/manifest.mpd" -o 1080p.mp4
```

## P2P Protocols

### BitTorrent

Supports `.torrent` files and `magnet://` links.

```bash
# Use a Magnet link
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# Download a torrent file
falcon-cli "https://example.com/file.torrent"

# Download a local torrent file
falcon-cli "file:///path/to/file.torrent"

# Set the save directory
falcon-cli "magnet:?xt=urn:btih:HASH" -d /downloads/torrents
```

BitTorrent features:
- Supports DHT, LSD, UPnP, and NAT-PMP
- Automatically discovers more peers
- Optionally select specific files to download
- Supports seeding (continue uploading after completion)

## Usage Examples

### Batch Download

Create a text file `urls.txt`:

```
# HTTP files
https://example.com/file1.zip
https://example.com/file2.zip

# Thunder link
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTMuemlwWg==

# ED2K link
ed2k://file|video.mp4|1073741824|ABCDEF123456789|/

# HLS stream
https://example.com/playlist.m3u8
```

Then batch download:

```bash
falcon-cli -i urls.txt
```

### C++ API Usage

```cpp
#include <falcon/falcon.hpp>

int main() {
    // Create a download engine
    falcon::DownloadEngine engine;

    // Configure download options
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";
    options.speed_limit = 1024 * 1024;  // 1MB/s

    // Download files from different protocols
    std::vector<std::string> urls = {
        "https://example.com/file.zip",
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==",
        "ed2k://file|test.zip|1048576|HASH|/",
        "magnet:?xt=urn:btih:HASH&dn=Test"
    };

    std::vector<std::shared_ptr<falcon::DownloadTask>> tasks;

    for (const auto& url : urls) {
        if (engine.supportsUrl(url)) {
            auto task = engine.startDownload(url, options);
            tasks.push_back(task);
            std::cout << "Started download: " << url << std::endl;
        }
    }

    // Wait for all downloads to complete
    for (auto& task : tasks) {
        task->wait();

        if (task->getStatus() == falcon::TaskStatus::Completed) {
            std::cout << "Download completed!" << std::endl;
            std::cout << "File size: " << task->getTotalBytes() << " bytes" << std::endl;
        } else {
            std::cout << "Download failed: " << task->getErrorMessage() << std::endl;
        }
    }

    return 0;
}
```

## Configuration

### Build Configuration

Use CMake to control which protocols are compiled in:

```bash
# Enable all protocols
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON

# Enable only the basic protocols
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=OFF \
  -DFALCON_ENABLE_THUNDER=OFF \
  -DFALCON_ENABLE_QQDL=OFF \
  -DFALCON_ENABLE_FLASHGET=OFF \
  -DFALCON_ENABLE_ED2K=OFF \
  -DFALCON_ENABLE_HLS=OFF
```

### Runtime Configuration

The more stable runtime interfaces today are CLI arguments and CMake build options. The configuration file format is still evolving, and treating any specific JSON file path as a stable interface is not recommended.

## Troubleshooting

### Common Issues

1. **Protocol not supported**
   - Make sure the protocol was enabled at build time
   - Check the corresponding `FALCON_ENABLE_*` option in `CMakeLists.txt`

2. **Download failure**
   - Check your network connection
   - Verify that the URL format is correct
   - Check the logs for detailed error information

3. **Resume not working**
   - Ensure the server supports Range requests
   - Check whether the file has been modified

4. **Slow BitTorrent downloads**
   - Ensure port 6881 is accessible
   - Check your firewall settings
   - Add more trackers

### Debug Mode

Enable verbose logging:

```bash
falcon-cli <URL> --verbose

# Or set the log level
export FALCON_LOG_LEVEL=debug
falcon-cli <URL>
```

### Performance Optimization

1. **HTTP download optimization**
   - Use segmented download (`-c 5`)
   - Adjust the buffer size
   - Use a CDN or mirror

2. **BitTorrent optimization**
   - Enable DHT and PEX
   - Use more trackers
   - Set an appropriate upload limit

3. **System optimization**
   - Increase TCP connection limits
   - Use SSD storage
   - Close unnecessary programs

## Protocol Comparison

| Protocol | Features | Use Cases | Limitations |
|----------|----------|-----------|-------------|
| HTTP/HTTPS | Simple, widely supported | Downloading webpages and files | Depends on server speed |
| FTP | Traditional file transfer | Enterprise file sharing | Not secure enough |
| Thunder/FlashGet | Common in China | Chinese-language resources | Requires decoding |
| ED2K | P2P network | Legacy resource sharing | Unstable speeds |
| BitTorrent | Decentralized | Large file distribution | Requires peers |
| HLS/DASH | Adaptive streaming | Video on demand | Requires merging |

## More Resources

- [API Documentation](api/)
- [Developer Guide](developer_guide.md)
- [FAQ](faq.md)
- [Contributing Guide](contributing.md)
