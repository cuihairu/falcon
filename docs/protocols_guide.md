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

Supports FTP and FTPS protocols (FTP uses passive mode by default).

```bash
# Download an FTP file
falcon-cli ftp://ftp.example.com/file.zip

# With username and password
falcon-cli ftp://user:pass@ftp.example.com/file.zip

# FTPS (libcurl's ftps://, explicit and implicit negotiation)
falcon-cli ftps://ftp.example.com/file.zip
```

## Private Protocols

Falcon supports private protocols from several commonly used download tools in China:

### Thunder

Supports `thunder://` and `thunderxl://` link formats.

```bash
# Classic Thunder link
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg=="

# Thunder offline link
falcon-cli "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
```

### QQDL

Supports `qqlink://` and `qqdl://` formats.

```bash
# QQDL link
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# Link with GID
falcon-cli "qqlink://1234567890ABCDEF|http://example.com/video.mp4"
```

### FlashGet

Supports `flashget://` and `fg://` formats.

```bash
# Full Base64 format (the &ref= suffix is stripped; the payload is Base64-decoded
# first, then the [FLASHGET] prefix is removed)
falcon-cli "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA==&ref=http://example.com/page"

# Short format (the fg:// payload is URL-encoded, not Base64)
falcon-cli "fg://http%3A%2F%2Fexample.com%2Ffile.zip"

# Base64 shorthand (works without the &ref= suffix too)
falcon-cli "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA=="
```

### ED2K (Donkey)

Supports `ed2k://` file links (server links `ed2k://server|...` are not supported; parsing them fails with an error).

```bash
# File link
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# File link with sources
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|s,192.168.1.1:4662|s,192.168.1.2:4662"

# With AICH checksum
falcon-cli "ed2k://file|example.zip|1048576|HASH|/|h=ABCDEF123456789"

```

## Streaming Protocols

### HLS (HTTP Live Streaming)

Supports `.m3u8` playlists, automatically downloading all media segments and merging them.

```bash
# Download an HLS stream
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4
```

HLS features:
- Parses `.m3u8` playlists (EXTINF duration info)
- Downloads all media segments in order and merges them via binary concatenation
- Master-playlist (stream selection) and encrypted streams (AES keys) are not supported yet

### DASH (Dynamic Adaptive Streaming over HTTP)

DASH (`.mpd` manifest parsing) is not implemented yet; submitting an `.mpd`
URL downloads the manifest file itself as a regular file. Use HLS playlists
for streaming downloads.

## P2P Protocols

### BitTorrent

Supports `.torrent` files and `magnet:` links.

```bash
# Use a Magnet link
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example&tr=tracker.example.com"

# Download a .torrent file via its direct HTTP link
falcon-cli "https://example.com/file.torrent"

# Download a local torrent file
falcon-cli "file:///path/to/file.torrent"

# Set the save directory
falcon-cli "magnet:?xt=urn:btih:HASH" -d /downloads/torrents
```

BitTorrent features:
- Supports DHT and PEX (libtorrent data plane, with LSD/UPnP/NAT-PMP)
- Automatically discovers more peers
- Supports seeding (ratio/duration policies; seeds to 1.0x by default)

## Usage Examples

### Batch Download

Create a text file `urls.txt`:

```
# HTTP files
https://example.com/file1.zip
https://example.com/file2.zip

# Thunder link
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZTMuemlwWlo=

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
    // Create a download engine (all built-in protocol handlers are
    // registered automatically at construction; no manual registration needed)
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

    std::vector<falcon::DownloadTask::Ptr> tasks;

    for (const auto& url : urls) {
        if (engine.is_url_supported(url)) {
            auto task = engine.add_task(url, options);
            if (task) {
                engine.start_task(task->id());
                tasks.push_back(task);
                std::cout << "Started download: " << url << std::endl;
            }
        }
    }

    // Wait for all downloads to complete
    for (auto& task : tasks) {
        task->wait();

        if (task->status() == falcon::TaskStatus::Completed) {
            std::cout << "Download completed!" << std::endl;
            std::cout << "File size: " << task->total_bytes() << " bytes" << std::endl;
        } else {
            std::cout << "Download failed: " << task->error_message() << std::endl;
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
  -DFALCON_ENABLE_SFTP=ON \
  -DFALCON_ENABLE_WEBDAV=ON \
  -DFALCON_ENABLE_METALINK=ON \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON

# Enable only the basic protocols (HTTP/FTP/BT/SFTP/WebDAV/Metalink default ON;
# the five private protocols default OFF)
cmake -B build \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_SFTP=ON \
  -DFALCON_ENABLE_WEBDAV=ON \
  -DFALCON_ENABLE_METALINK=ON \
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
   - Ensure the BT listening port (default 6881) is accessible (the actual port appears in the run logs)
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

- [API Documentation](api_guide.md)
- [Developer Guide](developer_guide.md)
- [FAQ](src/faq.md)
- [Contributing Guide](../CONTRIBUTING.md)
