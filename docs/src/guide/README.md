# Falcon 下载器

**Falcon（猎鹰下载器）** 是一个现代化、跨平台的 C++ 下载解决方案，采用 Monorepo 架构，致力于提供高性能、可扩展的多协议下载能力。

## 为什么选择 Falcon？

- **高性能**：基于 C++17/20 异步架构，充分利用现代硬件性能
- **多协议**：支持 HTTP、FTP、BitTorrent、ED2K、迅雷、HLS 等多种协议
- **易用性**：提供直观的 CLI 工具和 C++ API
- **可扩展**：插件化架构，轻松添加新协议支持
- **跨平台**：Windows、Linux、macOS 全平台支持

## 核心特性

### 1. 多协议下载

Falcon 支持广泛的下载协议，满足各种下载需求：

- **基础协议**：HTTP/HTTPS、FTP/FTPS
- **P2P 协议**：BitTorrent（Magnet 链接、.torrent 文件）
- **私有协议**：迅雷、QQ旋风、快车
- **流媒体协议**：HLS (.m3u8)、DASH (.mpd)
- **电驴协议**：ED2K

### 2. 高性能下载

- **多线程分块下载**：将文件分成多个块同时下载
- **智能断点续传**：自动检测服务器支持，中断后可继续下载
- **速度控制**：支持全局和单任务速度限制
- **连接复用**：减少建立连接的开销

### 3. 灵活的部署方式

- **CLI 工具**：命令行界面，适合脚本和自动化
- **Daemon 服务**：后台守护进程，支持远程管理
- **核心库**：可集成到其他项目中
- **图形界面**：桌面应用（开发中）

## 项目架构

Falcon 采用 Monorepo 架构，所有模块统一管理：

```
falcon/
├── packages/
│   ├── libfalcon/           # 核心下载引擎库
│   │   └── plugins/         # 协议插件
│   ├── falcon-cli/          # 命令行工具
│   └── falcon-daemon/       # 后台守护进程
├── apps/
│   ├── desktop/             # 桌面应用
│   └── web/                 # Web 管理界面
└── docs/                    # 项目文档
```

## 快速开始

### 安装

```bash
# macOS
brew install falcon

# Linux (从源码编译)
git clone https://github.com/yourusername/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
sudo cmake --install build
```

### 基本使用

```bash
# 下载单个文件
falcon-cli https://example.com/file.zip

# 多线程下载
falcon-cli https://example.com/large.zip -c 5

# 下载迅雷链接
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# 下载磁力链接
falcon-cli "magnet:?xt=urn:btih:HASH&dn=Example"

# 下载 HLS 流
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4
```

### C++ API 使用

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadAllPlugins();

    falcon::DownloadOptions options;
    options.max_connections = 5;

    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    task->wait();
    return 0;
}
```

## 文档导航

- [入门指南](getting-started.md) - 快速了解 Falcon
- [安装说明](installation.md) - 详细的安装步骤
- [使用指南](usage.md) - 命令行和 API 使用
- [协议支持](../protocols/README.md) - 所有支持的协议说明
- [开发者指南](../developer/README.md) - 贡献代码和插件开发
- [常见问题](../faq.md) - 常见问题解答

## 社区

- **GitHub**: [https://github.com/yourusername/falcon](https://github.com/yourusername/falcon)
- **Issues**: [https://github.com/yourusername/falcon/issues](https://github.com/yourusername/falcon/issues)
- **Discussions**: [https://github.com/yourusername/falcon/discussions](https://github.com/yourusername/falcon/discussions)

## 许可证

Apache License 2.0

---

::: tip 获取帮助
如果遇到问题，请查看 [常见问题](../faq.md) 或在 [GitHub Issues](https://github.com/yourusername/falcon/issues) 中提问。
:::
