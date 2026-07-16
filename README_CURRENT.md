# Falcon 下载器 - 当前开发进展

## 项目概述

Falcon 是一个现代化、跨平台的 C++ 下载解决方案，采用 Monorepo 架构，支持多种协议的插件化下载。

## 当前状态概览

截至当前代码状态，`libfalcon-core` 已达到“核心行为稳定”的阶段：任务模型、任务队列、并发控制、事件分发、协议抽象、优先级调度和任务状态持久化均已具备，并通过核心测试验证。

已验证：

- `falcon_core_tests`: 384 个测试通过
- `falcon_split_tests`: 10 个测试通过
- vcpkg manifest 构建链路可用，默认依赖会自动安装 `gtest`、`curl`、`openssl`、`spdlog`、`nlohmann-json` 和 `fmt`

这不等价于“产品功能齐全”。当前更准确的判断是：核心库底座约 75%-80% 可用，整个下载器产品约 45%-55% 完成。

## 已完成的工作

### 1. 四库分层架构 ✅
- 参考了 aria2 的设计理念，采用事件驱动架构
- 实现了四库拆分：`libfalcon-core`、`libfalcon-protocols`、`libfalcon-storage`、`libfalcon-drives`
- 建立了独立 CMake target：`Falcon::core`、`Falcon::protocols`、`Falcon::storage`、`Falcon::drives`
- 保留兼容聚合 target：`Falcon::falcon`
- 已通过 `falcon_split_tests` 验证四库链接边界

### 2. 核心接口定义 ✅
- **类型定义** (`types.hpp`): 定义了任务 ID、状态、进度等核心类型
- **下载任务** (`download_task.hpp`): 任务对象，支持状态查询和控制
- **事件监听器** (`event_listener.hpp`): 事件回调接口
- **协议处理器** (`protocol_handler.hpp`): 协议插件接口
- **下载选项** (`download_options.hpp`): 灵活的配置系统
- **事件分发器** (`event_dispatcher.hpp`): 异步事件处理系统
- **任务管理器** (`task_manager.hpp`): 任务队列和并发控制

### 3. 核心组件实现 ✅
- **事件分发器** (`event_dispatcher.cpp`):
  - 支持同步/异步事件分发
  - 多线程事件处理
  - 事件队列和优先级管理
- **任务管理器** (`task_manager.cpp`):
  - 支持任务队列和最大并发控制
  - 支持任务优先级 `Low/Normal/High/Critical`
  - 支持运行时优先级调整和任务状态持久化
  - 防止重复入队和暂停任务被旧队列条目误启动
- **下载引擎** (`download_engine.cpp`):
  - 统一管理任务、协议注册、事件监听和全局配置
- **版本管理** (`version.cpp`): 版本信息管理

### 4. 协议系统框架 ✅
- **协议注册表** (`ProtocolRegistry`): 协议处理器的注册和 URL 路由
- **协议处理器接口** (`IProtocolHandler`): 具体协议实现的稳定接入点
- **插件支持**:
  - HTTP/HTTPS 插件 (基于 libcurl)
  - FTP/FTPS 插件
  - 私有协议支持框架（迅雷、QQ旋风、快车、ED2K、HLS/DASH）

### 5. aria2 风格多线程下载 ✅
- **分块并行下载** (`segment_downloader.*`): 多连接并行下载 + 合并输出
- **断点续传（分块级别）**: 自动检测已下载分块并继续
- **CLI 批量/并发** (`falcon-cli`): `-i/--input-file`、`-j/--max-concurrent-downloads`、多 URL

### 6. 示例程序 ✅
- 创建了简单下载示例 (`examples/simple_download.cpp`)
- 演示了基本的使用方法

## 架构亮点

1. **事件驱动架构**
   - 异步 I/O 和事件循环
   - 命令模式封装下载操作
   - 非阻塞的任务调度

2. **插件化设计**
   - 协议处理器作为独立插件
   - 统一的插件接口
   - 动态加载和卸载支持

3. **并发控制**
   - 任务优先级队列
   - 最大并发数限制
   - 速度限制功能

4. **断点续传支持**
   - 每个协议插件自主实现断点续传
   - 任务状态持久化

## 当前分层

```text
falcon_core        # 核心领域模型、任务调度、事件、协议抽象、引擎门面
falcon_protocols   # 具体协议实现、HTTP/FTP 插件、分段下载、网络 I/O
falcon_storage     # 资源浏览、对象存储浏览器
falcon_drives      # 网盘、分享链、资源搜索、配置管理
```

依赖方向：

```text
falcon_protocols  -> falcon_core
falcon_storage    -> falcon_core
falcon_drives     -> falcon_core
```

`falcon_core` 不反向依赖具体协议库。内置协议注册通过弱符号桩实现解耦；最终程序链接 `falcon_protocols` 后，真实注册函数会接管 HTTP/FTP 等协议注册。

## 编译和测试状态

- ✅ `falcon_core` 编译通过
- ✅ `falcon_core_tests` 全部通过
- ✅ `falcon_split_tests` 全部通过
- ✅ HTTP/FTP 插件可在 vcpkg 依赖齐全时编译
- ⚠️ 完整产品测试尚未全部建立，特别是桌面端、daemon、网盘和真实协议集成场景

## 下一步计划

1. **协议层收口**: 补齐 HTTP/FTP 真实下载集成测试，清理协议层编译警告，明确 `DownloadEngineV2` 与当前 `DownloadEngine` 的关系。
2. **CLI 完整性**: 校准 aria2 风格参数行为，补齐配置、错误码、批量任务、断点续传、优先级等端到端测试。
3. **桌面端稳定性**: 明确 Qt6/vcpkg 构建路径，补齐任务列表、右键菜单、优先级、暂停/恢复等 UI 行为验证。
4. **Daemon 和远程控制**: 明确 RPC/REST 取舍，补齐任务持久化和多客户端访问语义。
5. **Storage/Drives 分层收口**: 澄清 `storage` 与 `drives` 职责边界，减少重复浏览器/云存储抽象。
6. **发布工程**: 建立 Linux/macOS/Windows CI，覆盖 vcpkg manifest 构建、核心测试、分层测试和包产物验证。

## 离功能齐全还差多少

按模块粗略估算：

| 模块 | 完成度 | 说明 |
|------|--------|------|
| `libfalcon-core` | 75%-80% | 核心调度、任务、事件、优先级和持久化已稳定，仍需更多压力和异常恢复测试 |
| `libfalcon-protocols` | 45%-55% | HTTP/FTP 基础存在，但分段下载、增量下载和私有协议仍需真实场景验证 |
| `falcon-cli` | 45%-55% | aria2 风格入口已有基础，仍需系统化参数兼容和端到端测试 |
| `apps/desktop` | 30%-40% | UI 框架和任务操作已有雏形，但发布级稳定性不足 |
| `falcon-daemon` | 20%-30% | 仍需明确服务接口和持久化语义 |
| `libfalcon-storage` / `libfalcon-drives` | 25%-40% | 能力分散，边界仍需收敛 |

整体产品距离“功能齐全、可发布”大约还差 45%-55% 的工程工作。优先级应是先把 core/protocols/CLI 的下载闭环打透，再推进桌面端和 daemon。

## 如何运行示例

```bash
# 编译
cmake -B build -S . -DFALCON_BUILD_EXAMPLES=ON
cmake --build build --target simple_download

# 运行（需要一个测试 URL）
./build/bin/simple_download https://example.com/file.zip
```

## 技术栈

- **语言**: C++17
- **构建系统**: CMake 3.15+
- **核心依赖**:
  - libcurl: HTTP/HTTPS/FTP 支持
  - libtorrent: BitTorrent 支持（可选）
  - spdlog: 日志（可选）
  - nlohmann/json: JSON 配置（可选）
- **测试框架**: Google Test
- **文档生成**: Doxygen

---

*最后更新: 2026-07-16*
