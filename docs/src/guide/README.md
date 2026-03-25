# Falcon 下载器

**Falcon（猎鹰下载器）** 是一个现代化、跨平台的 C++ 下载解决方案，采用 Monorepo 架构，目标是提供高性能、可扩展的多协议下载能力。

## 为什么选择 Falcon？

- **高性能**：基于 C++17 事件驱动架构，强调并发下载和断点续传
- **多协议**：当前默认启用 HTTP/HTTPS、FTP，并保留多种可选协议插件实现
- **易用性**：提供 CLI 工具和 C++ API
- **可扩展**：插件化架构，便于新增协议或能力
- **跨平台**：面向 Windows、Linux、macOS

## 核心特性

### 1. 多协议下载

Falcon 当前聚焦于可组合的协议能力：

- **基础协议**：HTTP/HTTPS、FTP/FTPS
- **P2P / 扩展协议**：BitTorrent、ED2K
- **私有协议插件**：迅雷、QQ 旋风、快车
- **流媒体协议插件**：HLS、DASH

### 2. 高性能下载

- **多线程分块下载**：将文件切分为多个块并行下载
- **断点续传**：在支持范围请求的场景中恢复中断任务
- **速度控制**：支持单任务限速
- **连接复用**：减少重复建连带来的额外开销

### 3. 多入口部署

- **CLI 工具**：适合脚本和自动化任务
- **Daemon 服务**：适合后台任务和远程管理
- **核心库**：可嵌入其他 C++ 项目
- **桌面端 / 浏览器扩展**：仓库中已包含相关代码

## 项目架构

```
falcon/
├── packages/
│   ├── libfalcon/           # 核心下载引擎库
│   │   └── plugins/         # 协议插件
│   ├── falcon-cli/          # 命令行工具
│   └── falcon-daemon/       # 后台守护进程
├── apps/
│   ├── desktop/             # 桌面应用
│   └── browser_extension/   # 浏览器扩展
└── docs/                    # 项目文档
```

## 快速开始

### 安装

```bash
git clone https://github.com/cuihairu/falcon.git
cd falcon
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### 基本使用

```bash
# 下载单个文件
falcon-cli https://example.com/file.zip

# 多线程下载
falcon-cli https://example.com/large.zip -c 5

# 下载迅雷链接
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# 从文件批量读取 URL
falcon-cli -i urls.txt -j 3
```

### C++ API 使用

```cpp
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
```

## 文档导航

- [入门指南](getting-started.md) - 快速了解 Falcon
- [安装说明](installation.md) - 详细的构建步骤
- [使用指南](usage.md) - 命令行和 API 使用
- [协议支持](../protocols/README.md) - 所有支持协议的概览
- [开发者指南](../developer/README.md) - 贡献代码和插件开发
- [常见问题](../faq.md) - 常见问题解答

## 社区

- **GitHub**: [https://github.com/cuihairu/falcon](https://github.com/cuihairu/falcon)
- **Issues**: [https://github.com/cuihairu/falcon/issues](https://github.com/cuihairu/falcon/issues)
- **Discussions**: [https://github.com/cuihairu/falcon/discussions](https://github.com/cuihairu/falcon/discussions)

## 许可证

Apache License 2.0

---

::: tip 获取帮助
如果遇到问题，请先查看 [常见问题](../faq.md)，或在 [GitHub Issues](https://github.com/cuihairu/falcon/issues) 中提问。
:::
