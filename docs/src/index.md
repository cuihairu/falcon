---
layout: home

hero:
  name: Falcon 下载器
  text: 现代化、跨平台的 C++ 下载解决方案
  tagline: 面向 CLI、Daemon、桌面端和可扩展协议栈的下载能力集合
  actions:
    - theme: brand
      text: 快速开始
      link: /guide/getting-started
    - theme: alt
      text: 项目介绍
      link: /guide/

features:
  - title: 多协议支持
    details: 当前默认启用 HTTP/HTTPS、FTP，并为 BitTorrent、ED2K、迅雷、QQ 旋风、快车、HLS/DASH 等协议保留了可选实现与后续迁移入口。
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
│   ├── libfalcon-core/      # 核心下载引擎、任务模型、事件系统
│   ├── libfalcon-protocols/ # 标准下载协议
│   ├── libfalcon-storage/   # 对象存储与资源浏览
│   ├── libfalcon-drives/    # 网盘与分享链能力
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
| BitTorrent | 可选实现 | 代码库内有实现，当前未统一接入默认自动注册 |
| ED2K | 可选实现 | 代码库内有实现，默认构建通常关闭 |
| 迅雷 | 可选实现 | 代码库内有实现，默认构建通常关闭 |
| QQ旋风 | 可选实现 | 代码库内有实现，默认构建通常关闭 |
| 快车 | 可选实现 | 代码库内有实现，默认构建通常关闭 |
| HLS/DASH | 可选实现 | 代码库内有实现，默认构建通常关闭 |

## 贡献

欢迎贡献代码、报告问题或提出建议！

- [GitHub Issues](https://github.com/cuihairu/falcon/issues)
- [贡献指南](/developer/)

## 许可证

Apache License 2.0
