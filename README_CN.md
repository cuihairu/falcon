<div align="center">

  <img src="./assets/falcon.png" alt="Falcon Logo" width="200"/>

  # Falcon 下载器

  [![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
  [![Build Status](https://github.com/cuihairu/falcon/workflows/CMake%20Build/badge.svg)](https://github.com/cuihairu/falcon/actions)
  [![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/cuihairu/falcon)
  [![Version](https://img.shields.io/badge/version-0.1.0-green.svg)](https://github.com/cuihairu/falcon/releases)

  **现代化、高性能、跨平台的下载加速器**

</div>

## 特性 🚀

- **aria2 风格架构**: 事件驱动命令模式、I/O 多路复用 (epoll/kqueue/poll)、连接复用、请求组管理
- **多协议支持**: HTTP/HTTPS、FTP、Metalink、BitTorrent、磁力链接、私有协议
  - 迅雷 (Thunder)
  - 腾讯旋风 (QQDL)
  - 快车 (FlashGet)
  - 电驴 (ED2K)
  - HLS/DASH 流媒体
- **守护进程与 RPC 服务** (`falcon-daemon`): aria2 兼容 JSON-RPC (HTTP + WebSocket
  事件流)、任务持久化 (SQLite)、`daemon.json` 配置与 SIGHUP 热重载
- **桌面应用** (Qt6): Fluent 设计语言、亮暗主题、表格/网格双视图、云盘浏览、Daemon
  RPC 后端
- **云存储集成**:
  - 亚马逊 S3
  - 阿里云 OSS
  - 腾讯云 COS
  - 七牛云 Kodo
  - 又拍云 USS
- **远程资源浏览**: 浏览 FTP/SFTP/S3 目录，显示详细信息
- **资源搜索**: 搜索引擎框架，支持种子和文件资源搜索
- **安全配置**: AES-256 加密存储凭据，主密码保护
- **高性能**: 多线程下载，支持速度控制和带宽限制
- **断点续传**: 自动恢复中断的下载
- **代理支持**: HTTP/HTTPS/SOCKS5 代理支持

## 快速开始 ⚡

### 安装

```bash
# 克隆仓库
git clone https://github.com/cuihairu/falcon.git
cd falcon

# 从源码编译
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

当前以源码构建为准。包管理器分发和预编译安装包应以正式 Release 说明为准，而不是这里的占位示例。

### 基本使用

```bash
# 简单下载
falcon-cli https://example.com/file.zip

# 多线程下载（5个连接）
falcon-cli https://example.com/large_file.iso -c 5

# 限速下载（1MB/s）
falcon-cli https://example.com/video.mp4 --limit 1M

# 从文件批量下载
falcon-cli -i urls.txt -j 3

# 通过代理并附加请求头
falcon-cli --proxy http://127.0.0.1:7890 \
  -H "Authorization: Bearer TOKEN" \
  https://example.com/file.bin
```

## 支持的协议 📡

| 协议 | 状态 | 描述 |
|------|------|------|
| HTTP/HTTPS | 已启用 | 标准 Web 协议，支持断点续传 |
| FTP/FTPS | 已启用 | 文件传输协议，支持被动模式 |
| Metalink | 已启用 | RFC 5854 `.meta4` / Metalink3 `.metalink` 镜像列表，整文件哈希校验 |
| SFTP | 可选插件 | 仓库内已实现，默认构建关闭 |
| BitTorrent | 可选插件 | 仓库内已实现，默认构建关闭 |
| 迅雷 | 可选插件 | 仓库内已实现，默认构建关闭 |
| 腾讯旋风 | 可选插件 | 仓库内已实现，默认构建关闭 |
| 快车 | 可选插件 | 仓库内已实现，默认构建关闭 |
| 电驴 | 可选插件 | 仓库内已实现，默认构建关闭 |
| HLS/DASH | 可选插件 | 仓库内已实现，默认构建关闭 |

## 云存储支持 ☁️

仓库已实现库层级的云存储浏览（`packages/libfalcon-storage`），覆盖亚马逊 S3、阿里云
OSS、腾讯云 COS、七牛云 Kodo、又拍云 USS：列举、树形视图、对象信息、建目录/改名/
递归删除、配额查询，并支持自定义 endpoint（MinIO / 私有化网关）。桌面应用的云盘页
面即基于这些模块构建。

> **注意**：CLI 目前未提供存储浏览命令。部分旧示例中出现的 `--list`、`--tree`、
> `--search`、`--add-config`、`--set-master-password` 等参数**并未实现**——请使用
> 桌面应用，或直接调用相关库。

## 安全配置 🔐

`libfalcon-drives` 提供加密配置管理器（SQLite 存储，AES-256-GCM 凭据加密，主密码
保护）。桌面应用的设置页基于它实现。CLI 管理命令尚未开放。

## 高级功能 ⚙️

### 资源搜索
`libfalcon-drives` 内置搜索提供者框架（`resource_search`）：提供者接口、URL 校验、
磁力链接解析、过滤/排序/截断，以及通用爬虫搜索提供者。CLI 搜索命令尚未实现，集成
请参见库 API。

### 远程目录浏览
`libfalcon-storage` 实现 `ResourceBrowser` 接口，覆盖 FTP、SFTP、S3、OSS、COS、
Kodo、又拍云——格式化树/表格列举、路径校验、递归操作。桌面云盘页面即基于它构建。

### 下载管理
```bash
# 从文件批量下载
falcon-cli --input urls.txt

# 恢复中断的下载（默认启用，也可用 aria2 风格显式开关）
falcon-cli --continue true https://example.com/partial.zip

# 自定义请求头和用户代理
falcon-cli https://example.com/file.bin \
  --header "Authorization: Bearer TOKEN" \
  --user-agent "Falcon/1.0"

# 代理支持
falcon-cli https://example.com/file.zip \
  --proxy http://proxy.example.com:8080 \
  --proxy-user user \
  --proxy-passwd pass
```

## 架构设计 🏗️

Falcon 采用模块化架构：

```
┌─────────────────────────────────────────────┐
│                   应用层                    │
│ falcon-cli · falcon-daemon · desktop (Qt6)  │
└──────────────────────┬──────────────────────┘
                      │
┌──────────────────────▼──────────────────────┐
│         Falcon 核心库 (aria2 风格)          │
│  ┌─────────────────────────────────┐        │
│  │  下载引擎 / 事件循环            │        │
│  │  命令队列 / 例程命令            │        │
│  │  事件轮询 (epoll/kqueue/poll)   │        │
│  │  请求组管理 / 等待队列          │        │
│  │  Socket 连接复用                │        │
│  │  任务管理器 / 事件分发          │        │
│  └─────────────────────────────────┘        │
└──────────────────────┬──────────────────────┘
                      │
┌──────────────────────▼──────────────────────┐
│                 协议插件层                  │
│  ┌─────┐ ┌────┐ ┌────┐ ┌────────┐ ┌─────┐   │
│  │ HTTP│ │ FTP│ │ BT │ │Metalink│ │ ... │   │
│  └─────┘ └────┘ └────┘ └────────┘ └─────┘   │
└─────────────────────────────────────────────┘
```

## 开发指南 👷

### 系统要求
- CMake 3.15+
- C++17 兼容的编译器（推荐 GCC 11+ / Clang 14+ / MSVC 2019+）
- libcurl 7.68+
- OpenSSL 1.1.1+
- nlohmann/json 3.10+
- spdlog 1.9+
- SQLite 3.35+
- libtorrent-rasterbar 2.0+（可选，BitTorrent 插件）
- Qt 6（可选，桌面应用）

### 编译选项
```bash
# 启用/禁用功能
cmake -B build -S . \
  -DFALCON_ENABLE_HTTP=ON \
  -DFALCON_ENABLE_FTP=ON \
  -DFALCON_ENABLE_BITTORRENT=ON \
  -DFALCON_ENABLE_CLOUD_STORAGE=ON \
  -DFALCON_ENABLE_RESOURCE_BROWSER=ON \
  -DFALCON_ENABLE_RESOURCE_SEARCH=ON
```

## 贡献指南 🤝

我们欢迎贡献！请查看我们的[贡献指南](CONTRIBUTING_CN.md)了解详情。

### 代码风格
- 遵循 Google C++ 风格指南
- 使用 `clang-format` 进行代码格式化
- 为新功能编写单元测试

## 许可证 📄

本项目采用 Apache License 2.0 许可证 - 详见 [LICENSE](LICENSE) 文件。

## 致谢 🙏

- [libcurl](https://curl.se/) 用于 HTTP/FTP/SFTP 支持
- [libtorrent](https://www.libtorrent.org/) 用于 BitTorrent 支持
- [nlohmann/json](https://github.com/nlohmann/json) 用于 JSON 处理
- [spdlog](https://github.com/gabime/spdlog) 用于日志
- [OpenSSL](https://www.openssl.org/) 用于加密操作
- [SQLite](https://sqlite.org/) 用于任务持久化与配置存储
- [Qt 6](https://www.qt.io/) 用于桌面应用

## 功能路线图 📋

### 已完成 ✅
- [x] 核心下载引擎
- [x] HTTP/HTTPS 插件 (libcurl)
- [x] FTP/FTPS 插件
- [x] 命令行界面
- [x] 私有协议支持（迅雷、QQDL、FlashGet、ED2K）
- [x] 云存储支持（S3、阿里云OSS、腾讯云COS、七牛云、又拍云）
- [x] 远程资源浏览
- [x] 资源搜索功能
- [x] 安全配置管理

### 进行中 🚧
- [ ] 完善可选协议插件的默认构建与文档一致性
- [ ] 完善桌面端能力
- [ ] 补齐发布制品与安装分发
- [ ] 更多私有协议

### 计划中 📅
- [ ] macOS 原生应用
- [ ] Linux 原生应用
- [ ] 移动端支持
- [ ] 分布式下载节点

---

<div align="center">
  Made with ❤️ by Falcon Team

  [English](README.md)
</div>
