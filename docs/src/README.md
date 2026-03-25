---
home: true
title: Falcon 下载器
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
    details: 已集成 HTTP/HTTPS、FTP 等核心协议，并为 BitTorrent、ED2K、迅雷、QQ 旋风、快车、HLS/DASH 等协议保留了插件实现。
  - title: 高性能
    details: 基于 C++17 事件驱动架构，支持多线程分块下载、断点续传和可配置并发控制。
  - title: 多入口形态
    details: 仓库内包含核心库、CLI、Daemon、桌面端和浏览器扩展，便于按场景组合使用。
  - title: 插件化
    details: 协议处理器作为独立插件组织，便于扩展新协议并隔离实现复杂度。
  - title: 易于集成
    details: 提供 libfalcon 核心库，可在其他 C++ 项目中复用下载能力。
  - title: 开源许可
    details: 基于 Apache 2.0 许可证发布。

footer: Apache-2.0 Licensed | Copyright © 2025-present Falcon Team
---

## 快速预览

### 安装

\`\`\`bash
# 从源码构建
git clone https://github.com/cuihairu/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
\`\`\`

当前文档以源码构建为主，不再假设 Homebrew、APT 或预编译安装包已经对外发布。

### 基本使用

\`\`\`bash
# 下载单个文件
falcon-cli https://example.com/file.zip

# 多线程下载
falcon-cli https://example.com/large.zip -c 5

# 从文件批量读取 URL
falcon-cli -i urls.txt -j 3

# 通过代理下载
falcon-cli https://example.com/file.zip --proxy http://127.0.0.1:7890
\`\`\`

### C++ API

\`\`\`cpp
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
│   └── browser_extension/   # 浏览器扩展
└── docs/                    # 项目文档
```

## 协议支持

| 协议 | 状态 | 说明 |
|------|------|------|
| HTTP/HTTPS | 已启用 | 断点续传、分块下载 |
| FTP/FTPS | 已启用 | 主动/被动模式 |
| BitTorrent | 可选插件 | 代码库内有插件实现，默认构建为关闭 |
| ED2K | 可选插件 | 代码库内有插件实现，默认构建为关闭 |
| 迅雷 | 可选插件 | 代码库内有插件实现，默认构建为关闭 |
| QQ旋风 | 可选插件 | 代码库内有插件实现，默认构建为关闭 |
| 快车 | 可选插件 | 代码库内有插件实现，默认构建为关闭 |
| HLS/DASH | 可选插件 | 代码库内有插件实现，默认构建为关闭 |

## 贡献

欢迎贡献代码、报告问题或提出建议！

- [GitHub Issues](https://github.com/cuihairu/falcon/issues)
- [贡献指南](/developer/README.html)

## 许可证

Apache License 2.0
