---
home: true
title: Falcon 下载器
heroImage: /logo.png
heroText: Falcon 下载器
tagline: 现代化、跨平台的 C++ 下载解决方案
actions:
  - text: 快速开始
    link: /guide/getting-started.html
    type: primary
  - text: 项目介绍
    link: /guide/README.html
    type: secondary

features:
  - title: 多协议支持
    details: 支持 HTTP/HTTPS、FTP、BitTorrent、ED2K、迅雷、QQ旋风、快车、HLS/DASH 等多种下载协议。
  - title: 高性能
    details: 基于 C++17/20 异步架构，支持多线程分块下载，智能断点续传，最大化下载速度。
  - title: 跨平台
    details: 支持 Windows、Linux、macOS，提供 CLI 工具、Daemon 守护进程和图形界面。
  - title: 插件化
    details: 协议处理器作为独立插件，易于扩展新协议，模块化设计便于维护。
  - title: 易于集成
    details: 提供核心库 libfalcon，可轻松集成到其他 C++ 项目中。
  - title: 开源免费
    details: 基于 Apache 2.0 许可证开源，完全免费使用。

footer: MIT Licensed | Copyright © 2025-present Falcon Team
---

## 快速预览

### 安装

\`\`\`bash
# macOS
brew install falcon

# Linux (从源码编译)
git clone https://github.com/yourusername/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
sudo cmake --install build
\`\`\`

### 基本使用

\`\`\`bash
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
\`\`\`

### C++ API

\`\`\`cpp
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
\`\`\`

## 项目结构

```
falcon/
├── packages/
│   ├── libfalcon/           # 核心下载引擎库
│   ├── falcon-cli/          # 命令行工具
│   └── falcon-daemon/       # 后台守护进程
├── apps/
│   ├── desktop/             # 桌面应用
│   └── web/                 # Web 管理界面
└── docs/                    # 项目文档
```

## 协议支持

| 协议 | 状态 | 说明 |
|------|------|------|
| HTTP/HTTPS | ✅ | 断点续传、分块下载 |
| FTP/FTPS | ✅ | 主动/被动模式 |
| BitTorrent | ✅ | Magnet、.torrent 文件 |
| ED2K | ✅ | 电驴协议 |
| 迅雷 | ✅ | thunder:// 协议 |
| QQ旋风 | ✅ | qqlink:// 协议 |
| 快车 | ✅ | flashget:// 协议 |
| HLS/DASH | ✅ | 流媒体协议 |

## 贡献

欢迎贡献代码、报告问题或提出建议！

- [GitHub Issues](https://github.com/yourusername/falcon/issues)
- [贡献指南](/developer/README.html)

## 许可证

Apache License 2.0
