# Falcon 下载器 vs aria2 功能对比报告（更新）

> [!NOTE]
> 本文档是 2025-12-27 的阶段性分析快照，不是当前用户文档入口，文中状态不再随代码更新。
> 快照之后已有变化：XML-RPC 服务器已删除，RPC 现为 aria2 兼容 JSON-RPC（同端口支持 WebSocket 事件流）；Metalink 解析已替换为 vendored libmetalink；SFTP 数据面依赖是 libssh2；文件路径经四库拆分后全部迁移。当前功能与覆盖率以 [README](../README.md) 和 CI 徽章为准。

**生成日期**: 2025-12-27
**Falcon 版本**: 0.1.0
**aria2 参考版本**: 1.37.0

---

## 总体功能完成度

单一完成度百分比说明不了什么，逐项状态见下方对比表。与快照时点相比，此后的主要变化已列在开头 NOTE 中。

---

## 新增功能汇总

### 1. BitTorrent 协议支持

**状态**: 已完成
**文件位置**:
- `packages/libfalcon-protocols/plugins/bittorrent/bittorrent_plugin.hpp`
- `packages/libfalcon-protocols/plugins/bittorrent/bittorrent_plugin.cpp`

**实现功能**:
- .torrent 文件解析（B 编码）
- Magnet 链接支持
- DHT (Distributed Hash Table)
- PEX (Peer Exchange)
- LSD (Local Service Discovery)
- 文件选择（多文件 torrent）
- 优先级设置
- 速度限制
- 种子制作（计划中）
- 完整的单元测试

**依赖**: libtorrent-rasterbar 2.0+

---

### 2. SFTP 协议支持

**状态**: 已完成
**文件位置**:
- `packages/libfalcon-protocols/plugins/sftp/sftp_plugin.hpp`
- `packages/libfalcon-protocols/plugins/sftp/sftp_plugin.cpp`

**实现功能**:
- SSH 密钥认证
- 密码认证
- 自定义端口
- 断点续传
- 速度限制
- 完整的 URL 解析
- 完整的单元测试

**支持格式**:
```
sftp://user@host/path
sftp://user:pass@host:port/path
```

**依赖**: libssh2

---

### 3. Metalink 协议支持

**状态**: 已完成（快照后实现已重写：解析层现为 vendored libmetalink + expat，见包内 metalink_handler）
**文件位置**:
- `packages/libfalcon-protocols/plugins/metalink/metalink_handler.hpp`
- `packages/libfalcon-protocols/plugins/metalink/metalink_handler.cpp`

**实现功能**:
- Metalink v4 / Metalink3 文件解析
- 多源下载（经 V2 引擎多镜像分段）
- 优先级排序
- 校验和验证（SHA-256, SHA-1, MD5）
- 自动故障转移
- XML 解析（vendored libmetalink）
- 完整的单元测试

**特点**:
- 镜像按 priority 排序依次尝试
- 自动重试：源失败时自动切换备用源
- 哈希验证：下载完成后自动验证文件完整性

---

### 4. XML-RPC 支持（快照后已删除）

**现状**: 快照中的 XML-RPC 服务器实现后来整体删除。当前 RPC 是 aria2 兼容的
JSON-RPC（HTTP + WebSocket 同端口），方法覆盖 aria2.addUri / aria2.remove /
aria2.pause / aria2.unpause / aria2.tellStatus / aria2.getGlobalStat /
aria2.getVersion / system.listMethods 等 28 个。

---

### 5. WebSocket 实时通信

**现状**: 独立的 websocket_server 原型已删除；WebSocket 事件流并入 daemon
JSON-RPC 同端口（`ws://host:6800/jsonrpc`），服务器广播 aria2 兼容通知
（onDownloadStart/Pause/Complete/Error/Stop）与 Falcon 扩展进度通知。

---

### 6. 增量下载功能

**状态**: 已完成
**文件位置**:
- `packages/libfalcon-protocols/include/falcon/incremental_download.hpp`
- `packages/libfalcon-protocols/src/incremental_download.cpp`

**实现功能**:
- 基于哈希的差异比较
- 分块级别的增量更新
- Rsync 算法支持（框架）
- 多种哈希算法（SHA-256, SHA-1, MD5）
- 文件验证
- 补丁应用

**使用场景**:
- 大文件更新（只下载变化部分）
- 版本更新
- 数据同步

带宽节省幅度取决于文件重复率，没有统一的百分比。

---

### 7. Shell 自动补全

**状态**: 已完成
**文件位置**:
- `packages/falcon-cli/completion/bash-completion.sh`
- `packages/falcon-cli/completion/zsh-completion.zsh`
- `packages/falcon-cli/completion/fish-completion.fish`
- `packages/falcon-cli/completion/README.md`

**支持 Shell**:
- Bash
- Zsh
- Fish

**补全功能**:
- 命令补全
- 选项补全
- GID 补全
- 目录补全
- URL 补全
- 文件名补全

---

## 完整功能对比表

| 功能类别 | aria2 | Falcon | 说明 |
|---------|-------|--------|------|
| **多协议支持** | | | |
| HTTP/HTTPS | 有 | 有 | 支持 HTTP 常用特性 |
| FTP | 有 | 有 | FTP 支持 |
| SFTP | 有 | 有 | 基于 libssh2 |
| BitTorrent | 有 | 有 | 基于 libtorrent-rasterbar |
| Magnet 链接 | 有 | 有 | |
| Metalink | 有 | 有 | v4 与 Metalink3 |
| **核心下载功能** | | | |
| 多线程分块 | 有 | 有 | 支持自适应分块 |
| 断点续传 | 有 | 有 | |
| 限速功能 | 有 | 有 | 全局和单任务 |
| 连接复用 | 有 | 有 | |
| 增量下载 | 无 | 有 | aria2 没有 |
| **任务管理** | | | |
| 任务队列 | 有 | 有 | 优先级队列 |
| 任务持久化 | 有 | 有 | SQLite 数据库 |
| 自动重试 | 有 | 有 | 可配置重试次数 |
| 文件选择 | 有 | 有 | BT 文件选择 |
| **RPC 接口** | | | |
| JSON-RPC | 有 | 有 | |
| XML-RPC | 有 | 无 | 快照中的实现已删除，未重做 |
| WebSocket | 有 | 有 | 实时事件推送 |
| **命令行工具** | | | |
| 基础下载 | 有 | 有 | |
| 批量下载 | 有 | 有 | URL 列表支持 |
| 进度显示 | 有 | 有 | 实时进度条 |
| Shell 补全 | 有 | 有 | Bash/Zsh/Fish |
| **高级功能** | | | |
| URI 解析 | 有 | 有 | 含私有协议 |
| 校验和验证 | 有 | 有 | 多种哈希算法 |
| DHT | 有 | 有 | |
| PEX | 有 | 有 | |
| LSD | 有 | 有 | |
| **扩展功能** | | | |
| 云存储 | 无 | 有 | S3/OSS/COS/Kodo |
| 私有协议 | 无 | 有 | 迅雷/旋风/快车 |
| HLS/DASH | 无 | 有 | 流媒体协议 |

---

## aria2 没有的功能

以下几块是 Falcon 在 aria2 之外增加的能力：

### 增量下载
- 只下载文件变化的部分
- 带宽节省幅度取决于文件重复率
- 适用于频繁更新的文件

### 云存储支持
- Amazon S3
- 阿里云 OSS
- 腾讯云 COS
- 七牛云 Kodo
- 又拍云

### 私有协议支持
- 迅雷 thunder://
- QQ 旋风 qqlink://
- 快车 flashget://
- ED2K 电驴

### 流媒体协议
- HLS (HTTP Live Streaming)
- DASH (Dynamic Adaptive Streaming over HTTP)

### C++17 与四库拆分
- C++17 实现，模块拆为 core / protocols / storage / drives 四库，依赖方向单向
- 协议以插件形式注册，新增协议不改引擎代码

---

## 依赖库清单

### 核心依赖
| 依赖库 | 版本 | 用途 | 必需/可选 |
|--------|------|------|----------|
| spdlog | 1.9+ | 日志 | 必需 |
| nlohmann/json | 3.10+ | JSON 解析 | 必需 |
| OpenSSL | 1.1+ | 加密/哈希 | 必需 |

### 协议插件依赖
| 依赖库 | 版本 | 用途 | 必需/可选 |
|--------|------|------|----------|
| libcurl | 7.68+ | HTTP/FTP | 推荐 |
| libtorrent-rasterbar | 2.0+ | BitTorrent | 可选 |
| libssh2 | 1.9+ | SFTP | 可选 |

### RPC 依赖
| 依赖库 | 版本 | 用途 | 必需/可选 |
|--------|------|------|----------|
| nlohmann/json | 3.10+ | JSON-RPC | 必需 |
| 微型 HTTP 服务器 | - | RPC 服务器 | 可选 |

### 测试依赖
| 依赖库 | 版本 | 用途 | 必需/可选 |
|--------|------|------|----------|
| Google Test | 1.12+ | 单元测试 | 推荐 |
| Google Mock | 1.12+ | Mock 测试 | 推荐 |

---

## 编译配置

### 最小化编译（仅 HTTP/HTTPS）
```bash
cmake -B build -DFALCON_ENABLE_BITTORRENT=OFF \
                 -DFALCON_ENABLE_FTP=OFF \
                 -DFALCON_ENABLE_SFTP=OFF \
                 -DFALCON_ENABLE_METALINK=OFF
cmake --build build
```

### 完整编译（所有协议）
```bash
cmake -B build -DFALCON_ENABLE_BITTORRENT=ON \
                 -DFALCON_ENABLE_FTP=ON \
                 -DFALCON_ENABLE_SFTP=ON \
                 -DFALCON_ENABLE_METALINK=ON \
                 -DFALCON_BUILD_DAEMON=ON \
                 -DFALCON_BUILD_TESTS=ON
cmake --build build
```

### 带 vcpkg 依赖
```bash
cmake -B build -DCMAKE_TOOLCHAIN_FILE=[vcpkg-root]/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

---

## 测试覆盖率

快照时点没有可信的覆盖率统计，手写百分比已删除。当前覆盖率由 CI Coverage job
持续产出（gcovr 口径），数值以
[codecov 徽章](https://codecov.io/gh/cuihairu/falcon)与 CI 日志为准。

---

## 性能对比

仓库没有做过受控的性能对比（相同硬件、相同网络、相同任务集），快照里那几张
百分比表没有测量条件支撑，已删除。CI 中的性能用例（如 PerformanceLargeFile）
只作回归阈值，不构成与 aria2 的横向对比。需要评估请在受控条件下自行测量。

---

## 使用示例

### HTTP 下载
```bash
# 简单下载
falcon-cli https://example.com/file.zip

# 多连接分段
falcon-cli -x 8 -s 8 https://example.com/large-file.bin

# 实验性 V2 引擎
falcon-cli --http-engine v2 https://example.com/file.zip
```

### Metalink 下载
```bash
# URL 以 .meta4 / .metalink 结尾时自动走 Metalink 流程
falcon-cli https://example.com/file.meta4
```

### RPC 调用
```bash
curl -d '{"jsonrpc":"2.0","method":"aria2.addUri","params":[["http://example.com/file"]],"id":1}' \
  http://localhost:6800/jsonrpc
```

BitTorrent、SFTP 等入口与全部参数以 `falcon-cli --help` 为准。快照原文的
`falcon download` 命令形态、`--select-file` / `--incremental` 参数与
`metalink://` 协议并未实现，已移除。

---

## 下一步计划

快照时点的计划清单，仅存档，当前进展以 README 路线图为准：

### 短期（1-2 个月）
1. 完成所有缺失功能（已完成）
2. 性能优化（连接池、内存使用）
3. 完善文档（API 文档、用户指南）
4. 发布 1.0 Beta 版本

### 中期（3-6 个月）
1. GUI 应用（桌面版，Qt6 桌面端后续已发布）
2. Web 管理界面
3. 移动端支持（Android/iOS）
4. 插件市场

### 长期（6-12 个月）
1. 分布式下载
2. 边缘缓存
3. AI 驱动的速度优化
4. 商业支持服务

---

## 许可证

Apache License 2.0

---

## 贡献

欢迎贡献！请查看 [CONTRIBUTING.md](../CONTRIBUTING.md) 了解详情。

---

## 联系方式

- 项目主页: https://github.com/cuihairu/falcon
- 问题反馈: https://github.com/cuihairu/falcon/issues

---

**最后更新**: 2025-12-27
**文档版本**: 1.0
