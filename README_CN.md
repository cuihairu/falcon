<div align="center">

  <img src="https://raw.githubusercontent.com/falcon-project/falcon/main/assets/logo.png" alt="Falcon Logo" width="200"/>

  # Falcon 下载器

  [![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
  [![Build Status](https://github.com/cuihairu/falcon/workflows/cmake-multi-platform.yml/badge.svg)](https://github.com/cuihairu/falcon/actions)
  [![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Windows-lightgrey.svg)](https://github.com/cuihairu/falcon)
  [![Version](https://img.shields.io/badge/version-0.1.0-green.svg)](https://github.com/cuihairu/falcon/releases)

  **现代化、高性能、跨平台的下载加速器**

</div>

## 特性 🚀

- **多协议支持**: HTTP/HTTPS、FTP、BitTorrent、磁力链接、私有协议
  - 迅雷 (Thunder)
  - 腾讯旋风 (QQDL)
  - 快车 (FlashGet)
  - 电驴 (ED2K)
  - HLS/DASH 流媒体
- **云存储集成**:
  - 亚马逊 S3
  - 阿里云 OSS
  - 腾讯云 COS
  - 七牛云 Kodo
  - 又拍云 USS
- **远程资源浏览**: 轻松浏览 FTP/SFTP/S3 目录，显示详细信息
- **资源搜索**: 内置搜索引擎，支持种子和文件资源搜索
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

# 或使用包管理器（即将推出）
# npm install -g falcon-downloader
# pip install falcon-downloader
```

### 基本使用

```bash
# 简单下载
falcon-cli https://example.com/file.zip

# 多线程下载（5个连接）
falcon-cli https://example.com/large_file.iso -c 5

# 限速下载（1MB/s）
falcon-cli https://example.com/video.mp4 --limit 1M

# 浏览 FTP 目录
falcon-cli --list ftp://ftp.example.com/pub

# 浏览 S3 存储桶
falcon-cli --list s3://my-bucket --key-id YOUR_KEY --secret-key YOUR_SECRET

# 搜索种子
falcon-cli --search "Ubuntu 22.04" --min-seeds 10
```

## 支持的协议 📡

| 协议 | 状态 | 描述 |
|------|------|------|
| HTTP/HTTPS | ✅ | 标准 Web 协议，支持断点续传 |
| FTP/FTPS | ✅ | 文件传输协议，支持被动模式 |
| BitTorrent | ✅ | 点对点文件共享，支持磁力链接 |
| 迅雷 | ✅ | 迅雷专用链接 |
| 腾讯旋风 | ✅ | 腾讯旋风专用链接 |
| 快车 | ✅ | 快车专用链接 |
| 电驴 | ✅ | eDonkey2000 网络支持 |
| HLS/DASH | ✅ | HTTP Live Streaming 和动态自适应流媒体 |

## 云存储支持 ☁️

### 亚马逊 S3
```bash
falcon-cli --list s3://my-bucket \
  --key-id AKIAIOSFODNN7EXAMPLE \
  --secret-key wJalrXUtnFEMI/ \
  --region us-west-2
```

### 阿里云 OSS
```bash
falcon-cli --list oss://my-bucket/my-folder \
  --access-key-id YOUR_ACCESS_KEY_ID \
  --access-key-secret YOUR_ACCESS_KEY_SECRET \
  --region cn-beijing
```

### 腾讯云 COS
```bash
falcon-cli --list cos://my-bucket-1250000000 \
  --secret-id YOUR_SECRET_ID \
  --secret-key YOUR_SECRET_KEY \
  --region ap-beijing
```

### 七牛云 Kodo
```bash
falcon-cli --list kodo://my-bucket \
  --access-key YOUR_ACCESS_KEY \
  --secret-key YOUR_SECRET_KEY
```

### 又拍云 USS
```bash
falcon-cli --list upyun://my-service \
  --username YOUR_USERNAME \
  --password YOUR_PASSWORD
```

## 配置管理 🔐

Falcon 提供使用 AES-256 加密的安全凭据存储：

### 设置主密码
```bash
falcon-cli --set-master-password
```

### 添加云存储配置
```bash
# 交互模式（提示输入凭据）
falcon-cli --add-config my-s3-bucket --provider s3

# 直接模式
falcon-cli --add-config my-s3-bucket --provider s3 \
  --key-id AKIAIOSFODNN7EXAMPLE \
  --secret-key wJalrXUtnFEMI/ \
  --region us-west-2 \
  --bucket my-bucket
```

### 使用已保存的配置
```bash
falcon-cli --list s3://my-bucket --config my-s3-bucket
```

### 列出所有配置
```bash
falcon-cli --list-configs
```

## 高级功能 ⚙️

### 资源搜索
在多个种子和文件托管网站中搜索：
```bash
# 基础搜索
falcon-cli --search "Ubuntu 22.04"

# 高级搜索和过滤
falcon-cli --search "电影" \
  --category video \
  --min-size 1GB \
  --max-size 10GB \
  --min-seeds 50 \
  --sort-by seeds \
  --download 1
```

### 远程目录浏览
浏览远程目录并显示丰富信息：
```bash
# 短格式列表
falcon-cli --list ftp://ftp.example.com/pub

# 详细列表
falcon-cli --list -L ftp://ftp.example.com/pub

# 树形视图，递归显示
falcon-cli --list --tree --recursive s3://my-bucket/data

# 排序和过滤
falcon-cli --list --sort size --sort-desc s3://my-bucket/
```

### 下载管理
```bash
# 从文件批量下载
falcon-cli --input urls.txt

# 恢复中断的下载
falcon-cli --continue https://example.com/partial.zip

# 自定义请求头和用户代理
falcon-cli https://example.com/file.bin \
  --header "Authorization: Bearer TOKEN" \
  --user-agent "Falcon/1.0"

# 代理支持
falcon-cli https://example.com/file.zip \
  --proxy http://proxy.example.com:8080 \
  --proxy-username user \
  --proxy-password pass
```

## 架构设计 🏗️

Falcon 采用模块化架构：

```
┌─────────────────────────────────────────┐
│              应用层                      │
│  ┌────────────┐ ┌────────────┐          │
│  │ falcon-cli │ │ falcon-gui │          │
│  └────────────┘ └────────────┘          │
└────────────────────┬────────────────────┘
                     │
┌────────────────────▼────────────────────┐
│              Falcon 核心库               │
│  ┌──────────────────────────────────┐  │
│  │     下载引擎                      │  │
│  │     任务管理器                    │  │
│  │     插件管理器                    │  │
│  │     配置管理器                    │  │
│  │     密码管理器                    │  │
│  └──────────────────────────────────┘  │
└────────────────────┬────────────────────┘
                     │
┌────────────────────▼────────────────────┐
│              协议插件层                  │
│  ┌─────┐ ┌─────┐ ┌──────┐ ┌─────┐       │
│  │ HTTP│ │ FTP │ │  BT  │ │ OSS  │ ...  │
│  └─────┘ └─────┘ └──────┘ └─────┘       │
└─────────────────────────────────────────┘
```

## 开发指南 👷

### 系统要求
- CMake 3.15+
- C++17 兼容的编译器
- libcurl 7.68+
- nlohmann/json 3.10+
- OpenSSL 1.1.1+
- SQLite 3.35+

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

- [libcurl](https://curl.se/) 用于 HTTP/FTP 支持
- [libtorrent](https://www.libtorrent.org/) 用于 BitTorrent 支持
- [nlohmann/json](https://github.com/nlohmann/json) 用于 JSON 处理
- [OpenSSL](https://www.openssl.org/) 用于加密操作
- [SQLite](https://sqlite.org/) 用于配置存储

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
- [ ] BitTorrent 插件 (libtorrent)
- [ ] Web 管理界面
- [ ] Windows GUI 应用
- [ ] 更多私有协议

### 计划中 📅
- [ ] macOS 原生应用
- [ ] Linux 原生应用
- [ ] 移动端支持
- [] 分布式下载节点

---

<div align="center">
  Made with ❤️ by Falcon Team

  [English](README.md)
</div>