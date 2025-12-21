<div align="center">

  <img src="./assets/falcon.png" alt="Falcon Logo" width="200"/>

  # Falcon Downloader

  [![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
  [![Build Status](https://github.com/cuihairu/falcon/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/cuihairu/falcon/actions)
  [![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/cuihairu/falcon)
  [![Version](https://img.shields.io/badge/version-0.1.0-green.svg)](https://github.com/cuihairu/falcon/releases)

  **A modern, high-performance, cross-platform download accelerator**

</div>

## Features 🚀

- **Multi-Protocol Support**: HTTP/HTTPS, FTP, BitTorrent, Magnet links, Private protocols
  - Thunder (迅雷)
  - QQDL (腾讯旋风)
  - FlashGet
  - ED2K (电驴)
  - HLS/DASH streaming
- **Cloud Storage Integration**:
  - Amazon S3
  - Alibaba Cloud OSS (阿里云OSS)
  - Tencent COS (腾讯云)
  - Qiniu Kodo (七牛云)
  - Upyun USS (又拍云)
- **Remote Resource Browsing**: Browse FTP/SFTP/S3 directories with rich information
- **Resource Search**: Built-in search engine for torrent and file resources
- **Secure Configuration**: AES-256 encrypted credential storage with master password protection
- **High Performance**: Multi-threaded downloading with speed control and bandwidth throttling
- **Resume Support**: Automatic resumption of interrupted downloads
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

# Or use package manager (coming soon)
# npm install -g falcon-downloader
# pip install falcon-downloader
```

### Basic Usage

```bash
# Simple download
falcon-cli https://example.com/file.zip

# Multi-threaded download (5 connections)
falcon-cli https://example.com/large_file.iso -c 5

# Download with speed limit (1MB/s)
falcon-cli https://example.com/video.mp4 --limit 1M

# Browse FTP directory
falcon-cli --list ftp://ftp.example.com/pub

# Browse S3 bucket
falcon-cli --list s3://my-bucket --key-id YOUR_KEY --secret-key YOUR_SECRET

# Search for torrents
falcon-cli --search "Ubuntu 22.04" --min-seeds 10
```

## Supported Protocols 📡

| Protocol | Status | Description |
|----------|--------|-------------|
| HTTP/HTTPS | ✅ | Standard web protocols with resume support |
| FTP/FTPS | ✅ | File Transfer Protocol with passive mode |
| BitTorrent | ✅ | Peer-to-peer file sharing with magnet links |
| Thunder | ✅ | Xunlei (Thunder) links |
| QQDL | ✅ | Tencent QQ Download links |
| FlashGet | ✅ | FlashGet links |
| ED2K | ✅ | eDonkey2000 network support |
| HLS/DASH | ✅ | HTTP Live Streaming and Dynamic Adaptive Streaming |

## Cloud Storage Support ☁️

### Amazon S3
```bash
falcon-cli --list s3://my-bucket \
  --key-id AKIAIOSFODNN7EXAMPLE \
  --secret-key wJalrXUtnFEMI/ \
  --region us-west-2
```

### Alibaba Cloud OSS (阿里云OSS)
```bash
falcon-cli --list oss://my-bucket/my-folder \
  --access-key-id YOUR_ACCESS_KEY_ID \
  --access-key-secret YOUR_ACCESS_KEY_SECRET \
  --region cn-beijing
```

### Tencent COS (腾讯云)
```bash
falcon-cli --list cos://my-bucket-1250000000 \
  --secret-id YOUR_SECRET_ID \
  --secret-key YOUR_SECRET_KEY \
  --region ap-beijing
```

### Qiniu Kodo (七牛云)
```bash
falcon-cli --list kodo://my-bucket \
  --access-key YOUR_ACCESS_KEY \
  --secret-key YOUR_SECRET_KEY
```

### Upyun USS (又拍云)
```bash
falcon-cli --list upyun://my-service \
  --username YOUR_USERNAME \
  --password YOUR_PASSWORD
```

## Configuration Management 🔐

Falcon provides secure credential storage with AES-256 encryption:

### Set Master Password
```bash
falcon-cli --set-master-password
```

### Add Cloud Storage Configuration
```bash
# Interactive mode (prompts for credentials)
falcon-cli --add-config my-s3-bucket --provider s3

# Direct mode
falcon-cli --add-config my-s3-bucket --provider s3 \
  --key-id AKIAIOSFODNN7EXAMPLE \
  --secret-key wJalrXUtnFEMI/ \
  --region us-west-2 \
  --bucket my-bucket
```

### Use Saved Configuration
```bash
falcon-cli --list s3://my-bucket --config my-s3-bucket
```

### List Configurations
```bash
falcon-cli --list-configs
```

## Advanced Features ⚙️

### Resource Search
Search across multiple torrent sites and file hosting services:
```bash
# Basic search
falcon-cli --search "Ubuntu 22.04"

# Advanced search with filters
falcon-cli --search "movies" \
  --category video \
  --min-size 1GB \
  --max-size 10GB \
  --min-seeds 50 \
  --sort-by seeds \
  --download 1
```

### Remote Directory Browsing
Browse remote directories with rich information display:
```bash
# Short listing
falcon-cli --list ftp://ftp.example.com/pub

# Detailed listing
falcon-cli --list -L ftp://ftp.example.com/pub

# Tree view with recursion
falcon-cli --list --tree --recursive s3://my-bucket/data

# Sort and filter
falcon-cli --list --sort size --sort-desc s3://my-bucket/
```

### Download Management
```bash
# Batch download from file
falcon-cli --input urls.txt

# Resume interrupted download
falcon-cli --continue https://example.com/partial.zip

# Custom headers and user agent
falcon-cli https://example.com/file.bin \
  --header "Authorization: Bearer TOKEN" \
  --user-agent "Falcon/1.0"

# Proxy support
falcon-cli https://example.com/file.zip \
  --proxy http://proxy.example.com:8080 \
  --proxy-username user \
  --proxy-password pass
```

## Architecture 🏗️

Falcon follows a modular architecture:

```
┌─────────────────────────────────────────┐
│              Applications                │
│  ┌────────────┐ ┌────────────┐          │
│  │ falcon-cli │ │ falcon-gui │          │
│  └────────────┘ └────────────┘          │
└────────────────────┬────────────────────┘
                     │
┌────────────────────▼────────────────────┐
│              Falcon Core                 │
│  ┌──────────────────────────────────┐  │
│  │     Download Engine              │  │
│  │     Task Manager                  │  │
│  │     Plugin Manager                │  │
│  │     Config Manager                │  │
│  │     Password Manager              │  │
│  └──────────────────────────────────┘  │
└────────────────────┬────────────────────┘
                     │
┌────────────────────▼────────────────────┐
│            Protocol Plugins              │
│  ┌─────┐ ┌─────┐ ┌──────┐ ┌─────┐       │
│  │ HTTP│ │ FTP │ │  BT  │ │ OSS  │ ...  │
│  └─────┘ └─────┘ └──────┘ └─────┘       │
└─────────────────────────────────────────┘
```

## Development 👷

### Prerequisites
- CMake 3.15+
- C++17 compatible compiler
- libcurl 7.68+
- nlohmann/json 3.10+
- OpenSSL 1.1.1+
- SQLite 3.35+

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

- [libcurl](https://curl.se/) for HTTP/FTP support
- [libtorrent](https://www.libtorrent.org/) for BitTorrent support
- [nlohmann/json](https://github.com/nlohmann/json) for JSON handling
- [OpenSSL](https://www.openssl.org/) for cryptographic operations
- [SQLite](https://sqlite.org/) for configuration storage

---

<div align="center">
  Made with ❤️ by the Falcon Team

  [中文文档](README_CN.md)
</div>