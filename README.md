<div align="center">

# Falcon

**A Modern Multi-Protocol Download Library & Tool**

[![CI](https://github.com/cuihairu/falcon/actions/workflows/ci.yml/badge.svg)](https://github.com/cuihairu/falcon/actions/workflows/ci.yml)
[![License](https://img.shields.io/github/license/cuihairu/falcon)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/std/the-standard)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey)](https://github.com/cuihairu/falcon)

[English](#features) | [中文](#功能特性)

</div>

---

## Features

- **Multi-Protocol Support**: HTTP/HTTPS, FTP (planned), BitTorrent (planned), Magnet links
- **Resume Downloads**: Automatic breakpoint resume for interrupted downloads
- **Multi-Connection**: Parallel connections for faster downloads
- **Plugin Architecture**: Extensible protocol handler system
- **Cross-Platform**: Linux, macOS, Windows support
- **Modern C++17**: Clean, maintainable codebase

## Quick Start

### Installation

#### From Source

```bash
# Clone repository
git clone https://github.com/cuihairu/falcon.git
cd falcon

# Build
cmake -B build \
  -DFALCON_BUILD_CLI=ON \
  -DFALCON_ENABLE_HTTP=ON
cmake --build build -j$(nproc)

# Install (optional)
sudo cmake --install build
```

#### Dependencies

| Dependency | Required | Purpose |
|------------|----------|---------|
| CMake 3.15+ | Yes | Build system |
| C++17 compiler | Yes | GCC 8+, Clang 7+, MSVC 2019+ |
| libcurl | Optional | HTTP/HTTPS downloads |
| spdlog | Optional | Logging |

### Usage

```bash
# Basic download
falcon-cli https://example.com/file.zip

# Specify output file
falcon-cli -o myfile.zip https://example.com/file.zip

# Multiple connections
falcon-cli -c 8 https://example.com/large.iso

# Speed limit
falcon-cli -l 1M https://example.com/file.zip

# Show help
falcon-cli --help
```

### Library Usage

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::EngineConfig config;
    falcon::DownloadEngine engine(config);

    falcon::DownloadOptions options;
    options.max_connections = 4;
    options.output_directory = "/tmp/downloads";

    auto task = engine.add_task("https://example.com/file.zip", options);
    task->wait();

    return task->status() == falcon::TaskStatus::Completed ? 0 : 1;
}
```

## Project Structure

```
falcon/
├── packages/
│   ├── libfalcon/          # Core download library
│   │   ├── include/        # Public API headers
│   │   ├── src/            # Implementation
│   │   ├── plugins/        # Protocol plugins (HTTP, FTP, BT)
│   │   └── tests/          # Unit tests
│   ├── falcon-cli/         # Command-line tool
│   └── falcon-daemon/      # Background service (planned)
├── apps/
│   ├── desktop/            # Desktop GUI (planned)
│   └── web/                # Web interface (planned)
└── docs/                   # Documentation
```

## Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `FALCON_BUILD_TESTS` | OFF | Build unit tests |
| `FALCON_BUILD_CLI` | ON | Build CLI tool |
| `FALCON_BUILD_DAEMON` | OFF | Build daemon service |
| `FALCON_ENABLE_HTTP` | ON | Enable HTTP/HTTPS plugin |
| `FALCON_ENABLE_FTP` | OFF | Enable FTP plugin |
| `FALCON_ENABLE_BITTORRENT` | OFF | Enable BitTorrent plugin |

## Roadmap

- [x] Core download engine
- [x] HTTP/HTTPS plugin (libcurl)
- [x] Command-line interface
- [x] Unit tests (61 tests)
- [x] CI/CD (GitHub Actions)
- [ ] FTP plugin
- [ ] BitTorrent plugin (libtorrent)
- [ ] Daemon service with RPC
- [ ] Desktop GUI (Qt)
- [ ] Web management interface

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

1. Fork the repository
2. Create your feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## License

This project is licensed under the Apache License 2.0 - see the [LICENSE](LICENSE) file for details.

---

<div align="center">

## 功能特性

- **多协议支持**: HTTP/HTTPS、FTP (计划中)、BitTorrent (计划中)、磁力链接
- **断点续传**: 自动恢复中断的下载
- **多连接下载**: 并行连接加速下载
- **插件架构**: 可扩展的协议处理器系统
- **跨平台**: 支持 Linux、macOS、Windows
- **现代 C++17**: 简洁、可维护的代码库

</div>

## 快速开始

### 从源码安装

```bash
# 克隆仓库
git clone https://github.com/cuihairu/falcon.git
cd falcon

# 编译
cmake -B build \
  -DFALCON_BUILD_CLI=ON \
  -DFALCON_ENABLE_HTTP=ON
cmake --build build -j$(nproc)

# 安装 (可选)
sudo cmake --install build
```

### 使用方法

```bash
# 基础下载
falcon-cli https://example.com/file.zip

# 指定输出文件
falcon-cli -o myfile.zip https://example.com/file.zip

# 多连接下载
falcon-cli -c 8 https://example.com/large.iso

# 限速
falcon-cli -l 1M https://example.com/file.zip

# 显示帮助
falcon-cli --help
```

---

<div align="center">

Made with ❤️ by [cuihairu](https://github.com/cuihairu)

</div>
