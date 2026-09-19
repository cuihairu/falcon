# TODO

## 目标

将当前单体 `packages/libfalcon` 拆分为四个职责清晰的库：

- `libfalcon-core`
- `libfalcon-protocols`
- `libfalcon-storage`
- `libfalcon-drives`

拆分原则：

- 下载核心抽象只放在 `core`
- 标准下载协议实现只放在 `protocols`
- 对象存储和远程资源浏览只放在 `storage`
- 各种网盘、分享链、账号态能力只放在 `drives`
- 目录结构、CMake target、C++ namespace 三层语义保持一致

补充架构原则：

- 基础下载协议默认内置并随发行版集成
- 云盘分享解析、对象存储浏览、第三方远端能力走插件化扩展
- 主程序只依赖稳定接口，不直接硬编码具体厂商实现
- 插件启用状态由安装结果和配置共同决定
- 优先支持官方插件，后续再开放第三方生态

---

## 架构决策

### A1. 基础下载协议默认内置

- `http/https`
- `ftp/ftps`
- `magnet/bittorrent`
- `ed2k`
- `thunder`
- `flashget`
- `qqdl`
- `hls/dash`

要求：

- 默认构建产物应包含常用下载协议
- `DownloadEngine` 默认加载这些内置 handler
- 同时保留按构建选项裁剪能力
- 后续允许通过运行时配置禁用特定内置协议

说明：

- 这类能力直接服务下载引擎主路径
- 应作为产品基础能力，而不是要求用户手动安装插件才能可用

### A2. 云能力拆分为两类扩展点

1. `StorageProvider`

- 面向对象存储、远程文件系统、可浏览资源
- 典型实现：`s3`、`oss`、`cos`、`kodo`、`upyun`、`webdav`、`sftp`

2. `DriveResolver`

- 面向分享链、提取码、账号态、直链解析
- 典型实现：百度网盘、阿里云盘、夸克、蓝奏云、天翼云盘等

要求：

- 两类扩展点不能再混用同一个接口抽象
- `StorageProvider` 负责浏览、上传、下载入口构造
- `DriveResolver` 负责分享链接解析、目录枚举、直链获取、认证

### A3. 主程序不再硬编码厂商

- Desktop UI 不再手写 `S3/OSS/COS/Kodo/Upyun` 固定列表
- `StorageService` 不再通过 `if/else` 创建具体 browser
- URL 检测层不再把所有网盘规则散落在 UI 代码中
- 所有可选能力由 registry / plugin manager 提供元数据

### A4. 引入 SPI 风格插件系统

目标：

- 支持官方插件目录扫描
- 支持通过配置启用/禁用插件
- 支持动态库加载：Linux `.so`，macOS `.dylib`，Windows `.dll`

约束：

- 跨动态库边界优先使用稳定 C ABI
- 不直接暴露脆弱的 C++ ABI 作为长期插件协议
- 需要显式的 `plugin_api_version` 与 `falcon_version` 校验

插件最小元数据需要包含：

- `id`
- `name`
- `version`
- `type`
- `capabilities`
- `supported_schemes` 或 `supported_platforms`
- `enabled_by_default`
- `min_falcon_version`

### A5. 配置驱动启用

配置系统需要支持：

- 插件目录列表
- 已启用插件 ID 列表
- 已禁用插件 ID 列表
- 每个插件的独立配置块

示例目标：

```json
{
  "plugins": {
    "directories": ["./plugins", "~/.falcon/plugins"],
    "enabled": ["falcon.protocol.http", "falcon.storage.s3"],
    "disabled": ["falcon.drive.baidu"]
  }
}
```

---

## 目标结构

```text
packages/
├── libfalcon-core/
├── libfalcon-protocols/
├── libfalcon-storage/
├── libfalcon-drives/
├── falcon-cli/
└── falcon-daemon/
```

目标 target：

- `falcon_core`
- `falcon_protocols`
- `falcon_storage`
- `falcon_drives`

目标 CMake alias：

- `Falcon::core`
- `Falcon::protocols`
- `Falcon::storage`
- `Falcon::drives`

目标依赖方向：

- `falcon_protocols -> falcon_core`
- `falcon_storage -> falcon_core`
- `falcon_drives -> falcon_core`

禁止出现的反向依赖：

- `falcon_core -> falcon_protocols`
- `falcon_core -> falcon_storage`
- `falcon_core -> falcon_drives`
- `falcon_storage -> falcon_drives`
- `falcon_drives -> falcon_protocols`

---

## 第一阶段：边界冻结

### 0. 冻结当前三类能力边界

- 下载协议：进入 `ProtocolRegistry`
- 存储浏览：进入 `StorageProviderRegistry`
- 网盘解析：进入 `DriveResolverRegistry`

验收标准：

- 三类能力不再共享模糊的 “plugin” 命名
- 每类能力都有独立接口、注册中心、配置模型
- Desktop / CLI / Daemon 只通过 registry 查询能力

### 1. 明确 `core` 的职责

- 保留下载引擎、任务模型、事件系统、调度器、插件注册接口
- 保留通用异常、版本、基础下载选项
- 不再在 `core` 暴露云存储、远程资源浏览、网盘平台等概念

候选保留文件：

- `download_engine.*`
- `download_task.*`
- `task_manager.*`
- `thread_pool.*`
- `event_dispatcher.*`
- `plugin_manager.*`
- `version.*`
- `include/falcon/download_engine.hpp`
- `include/falcon/download_options.hpp`
- `include/falcon/download_task.hpp`
- `include/falcon/event_dispatcher.hpp`
- `include/falcon/event_listener.hpp`
- `include/falcon/exceptions.hpp`
- `include/falcon/plugin_manager.hpp`
- `include/falcon/protocol_handler.hpp`
- `include/falcon/types.hpp`
- `include/falcon/version.hpp`

### 2. 明确 `protocols` 的职责

- 收纳标准下载协议及可归一化的下载协议
- 使用 `falcon::protocols::<name>` 命名空间
- 不再继续使用 `falcon::plugins` 作为最终命名空间
- 基础协议默认内置，不要求用户单独安装
- 为后续动态协议插件保留统一注册入口

候选迁移模块：

- `plugins/http`
- `plugins/ftp`
- `plugins/bittorrent`
- `plugins/sftp`
- `plugins/metalink`
- `plugins/thunder`
- `plugins/qqdl`
- `plugins/flashget`
- `plugins/ed2k`
- `plugins/hls`
- `src/segment_downloader.cpp`
- `src/incremental_download.cpp`
- `src/request_group.cpp`
- `src/file_hash.cpp`
- `src/net/*`
- `src/commands/*`

### 3. 明确 `storage` 的职责

- 收纳对象存储协议和远程资源浏览能力
- 使用 `falcon::storage::<name>` 命名空间
- 从 `core` 移出 `resource_browser` 及云存储浏览接口
- 引入 `StorageProvider` 元数据和能力描述
- 用 registry 替代 Desktop 层的硬编码 provider 列表

候选迁移模块：

- `include/falcon/resource_browser.hpp`
- `include/falcon/ftp_browser.hpp`
- `include/falcon/s3_browser.hpp`
- `include/falcon/oss_browser.hpp`
- `include/falcon/cos_browser.hpp`
- `include/falcon/kodo_browser.hpp`
- `include/falcon/upyun_browser.hpp`
- `include/falcon/cloud_protocol.hpp`
- `include/falcon/cloud_url_protocols.hpp`
- `include/falcon/s3_plugin.hpp`
- `src/resource_browser.cpp`
- `src/resource_browser_utils.cpp`
- `plugins/s3`
- `plugins/oss`
- `plugins/cos`
- `plugins/kodo`
- `plugins/upyun`
- `plugins/ftp/ftp_browser.cpp`

### 4. 明确 `drives` 的职责

- 收纳各种网盘、分享链、提取码、认证态、直链提取能力
- 使用 `falcon::drives::<name>` 命名空间
- 从 `core` 移出 `cloud_storage_plugin` 和云盘配置概念
- 引入 `DriveResolver` 元数据、认证能力和分享链解析接口
- UI 根据 resolver 元数据动态展示支持的平台

### 4.1 审核现状并清理硬编码点

需要清理的现状问题：

- `ProtocolRegistry` 当前仍是编译期内置注册，不是动态 SPI
- `builtin_protocol_handlers.cpp` 当前只显式注册了 HTTP/FTP
- `StorageService::create_browser()` 仍硬编码具体厂商实现
- `CloudPage` 仍硬编码固定云存储类型下拉框
- `UrlDetector` 仍在 UI 层维护网盘识别规则
- `IResourceBrowser` 接口能力不足，尚不能完整表达上传/直链/能力描述
- `ICloudStoragePlugin` 与 `IResourceBrowser` 并行存在，但缺统一 registry / plugin manager

当前代码进度备注（2026-04-25）：

- `CloudPage` 已改为通过 `BrowserFactory::available_browsers()` 动态生成对象存储类型列表
- `StorageService` 已改为通过 `BrowserFactory` 创建 browser，不再直接 `if/else` new 具体厂商类型
- `BrowserFactory` 已具备基础元数据查询与工厂注册能力，但仍是进程内静态 registry，不是 SPI 插件系统
- `ProtocolRegistry` 已补充 `describe_builtin_protocols()`，可区分“已编译进来”与“已接入自动注册”
- `register_builtin_protocol_handlers()` 目前实际只接入了 HTTP/FTP；BT、SFTP、Thunder、ED2K、HLS 等仍未统一迁移到 `IProtocolHandler`
- `drives` 侧仍主要是 `ICloudStoragePlugin` / `CloudStorageManager` 旧抽象，`DriveResolverRegistry` 尚未落地
- `UrlDetector` 仍在 Desktop UI 侧硬编码网盘规则，尚未迁移到 resolver / registry 元数据驱动

候选迁移模块：

- `include/falcon/cloud_storage_plugin.hpp`
- `src/cloud_storage_plugin.cpp`
- `config_manager.*` 中与云盘账号、云盘配置强耦合的部分

---

## 第二阶段：先拆 target，再搬目录

### 5. 顶层构建拆分

- 新增四个 package 目录和各自 `CMakeLists.txt`
- 顶层 `CMakeLists.txt` 改为分别 `add_subdirectory()`
- 停止用单一 `packages/libfalcon/CMakeLists.txt` 承载所有能力

### 6. 建立四个库 target

- `falcon_core`
- `falcon_protocols`
- `falcon_storage`
- `falcon_drives`

验收标准：

- `falcon_core` 可单独编译
- `falcon_protocols` 只依赖 `falcon_core`
- `falcon_storage` 只依赖 `falcon_core`
- `falcon_drives` 只依赖 `falcon_core`

### 7. 调整应用层链接关系

- `falcon-cli` 按需链接 `Falcon::core`、`Falcon::protocols`、`Falcon::storage`、`Falcon::drives`
- `falcon-daemon` 按需链接对应库
- `apps/desktop` 按需链接对应库

验收标准：

- CLI 不因未启用 `drives` 而被强制带入网盘逻辑
- Daemon 不因未启用 `storage` 而强制依赖对象存储实现

---

## 第三阶段：物理目录重组

### 8. 迁移为新的目录布局

#### `libfalcon-core`

公共头路径保持为：

- `<falcon/download_engine.hpp>`
- `<falcon/protocol_handler.hpp>`

命名空间保持为：

- `falcon`

#### `libfalcon-protocols`

目录目标：

```text
packages/libfalcon-protocols/
├── include/falcon/protocols/http/
├── include/falcon/protocols/ftp/
├── include/falcon/protocols/bittorrent/
└── src/
```

公共头路径目标：

- `<falcon/protocols/http/http_handler.hpp>`
- `<falcon/protocols/ftp/ftp_handler.hpp>`

命名空间目标：

- `falcon::protocols::http`
- `falcon::protocols::ftp`
- `falcon::protocols::bittorrent`

#### `libfalcon-storage`

目录目标：

```text
packages/libfalcon-storage/
├── include/falcon/storage/
└── src/
```

公共头路径目标：

- `<falcon/storage/resource_browser.hpp>`
- `<falcon/storage/s3/s3_browser.hpp>`
- `<falcon/storage/oss/oss_browser.hpp>`

命名空间目标：

- `falcon::storage`
- `falcon::storage::s3`
- `falcon::storage::oss`
- `falcon::storage::cos`

#### `libfalcon-drives`

目录目标：

```text
packages/libfalcon-drives/
├── include/falcon/drives/
└── src/
```

公共头路径目标：

- `<falcon/drives/cloud_storage_plugin.hpp>`
- `<falcon/drives/baidu/baidu_drive.hpp>`
- `<falcon/drives/aliyundrive/aliyundrive.hpp>`

命名空间目标：

- `falcon::drives`
- `falcon::drives::baidu`
- `falcon::drives::aliyundrive`

---

## 第四阶段：接口收口

### 9. 收口 `core` 的公共 API

- 检查 `include/falcon/*.hpp`
- 移除不属于核心库的头文件
- 为应用层保留最小稳定入口

验收标准：

- `core` 头文件不再出现 `S3`、`OSS`、`COS`、`Kodo`、`Upyun`
- `core` 头文件不再出现 `CloudStorage`、`ResourceBrowser`

### 10. 收口配置管理

- 拆分 `config_manager.*`
- 通用配置设施保留在 `core` 或应用层
- 对象存储配置进入 `storage`
- 网盘账号与 token 配置进入 `drives`

验收标准：

- `core` 中不再保存云盘 provider 配置
- 云盘账号态不再成为所有消费者的强制依赖

### 11. 废弃旧的 `falcon::plugins`

- 将协议实现逐步迁移到 `falcon::protocols::*`
- 清理与新架构冲突的旧命名

验收标准：

- 新增代码不再使用 `falcon::plugins`
- 协议实现命名空间与库职责一致

---

## 第五阶段：安装与分发

### 12. 完善安装导出

- 每个库独立安装头文件、库文件和 CMake config
- 修正可选依赖的 `find_dependency()` 策略
- 修正插件目标导出顺序和安装顺序

验收标准：

- 各库可被外部 CMake 项目独立消费
- 不启用的 feature 不应要求额外依赖

### 13. 设计 `vcpkg` 包拆分

包名候选：

- `libfalcon-core`
- `libfalcon-protocols`
- `libfalcon-storage`
- `libfalcon-drives`

feature 候选：

- `libfalcon-protocols[http,ftp,bittorrent,sftp,hls]`
- `libfalcon-storage[s3,oss,cos,kodo,upyun,webdav]`
- `libfalcon-drives[baidu,aliyundrive,115,quark,pikpak]`

验收标准：

- 包依赖清晰
- 核心库可单独安装
- 对象存储和网盘能力均可按 feature 选择安装

---

## 当前优先级

### P0 ✅ 已完成 (2026-04-14)

- ✅ 明确四个库的职责边界
- ✅ 从 `core` 移出 `resource_browser` 和 `cloud_storage_plugin`
- ✅ 拆分四个 CMake target（独立 target + alias）
- ✅ core CMakeLists 清理：移除非 core option，提升至顶层 CMakeLists
- ✅ core 头文件收口：不再暴露 S3/OSS/COS/CloudStorage/ResourceBrowser 等概念
- ✅ config_manager 已归属 drives，core 无残留引用

### P1 ✅ 已完成 (2026-04-14)

- ✅ 删除旧 `packages/libfalcon/` 单体目录
- ✅ 修复所有 CMake 中硬编码的旧 `packages/libfalcon/include` 路径
- ✅ 迁移测试文件到对应包（protocols/storage/drives 各自独立 tests/CMakeLists.txt）
- ✅ `http`、`ftp` 等协议已在 `libfalcon-protocols` 中
- ✅ `s3`、`oss`、`cos`、`kodo`、`upyun` 已在 `libfalcon-storage` 中

### P2 ✅ 已完成 (2026-04-14)

- ✅ 废弃 `falcon::plugins`，迁移到 `falcon::protocols`（22个文件）
- ✅ 各库独立 install target + 聚合 FalconTargets 导出
- ✅ 更新根 CLAUDE.md 反映新目录结构

### Phase 3 ✅ 已完成 (2026-04-15)

- ✅ 公共头文件路径重组：24 个头文件迁移到 namespace 对齐路径
- ✅ drives: 3 headers → `falcon/drives/`
- ✅ storage: 11 headers → `falcon/storage/`
- ✅ protocols: 10 headers → `falcon/protocols/`（含 commands/、http/、net/ 子目录）
- ✅ 保留向后兼容 shim 头文件（24 个），代码中已无旧路径引用
- ✅ 更新 45+ 源文件/测试文件的 include 路径

### Phase 4 ✅ 已完成 (2026-04-15)

- ✅ 接口收口：core 无残留文件，`falcon::plugins` 命名空间清理完成
- ✅ 文档更新：3 个文档文件的旧接口引用已修正
- ✅ shim 警告清理：移除 24 个 shim 头文件的 #warning 编译警告
- ✅ 安装导出完善：各库 install/export 配置已完善，Config.cmake.in 齐全

---

## 完成定义 ✅ 全部通过

- ✅ 仓库中不再存在承担全部能力的单体 `libfalcon`
- ✅ `core / protocols / storage / drives` 均有独立 target 和安装规则
- ✅ 协议实现不再使用 `falcon::plugins` 作为最终命名空间
- ✅ `core` 不再暴露对象存储和网盘客户端概念
- ✅ CLI、Daemon、Desktop 均能按需组合依赖（仅链接 core + protocols）

---

## Desktop 应用开发进度

### 2026-04-15 - 迅雷风格 UI 重构

**已完成：**
- ✅ 组件化拆分：TopBar、StatusBar、SideBar 独立组件
- ✅ DownloadPage 迅雷风格布局
- ✅ 任务列表表格视图（文件名、进度、大小、速度、状态、操作）
- ✅ 视图模式切换（下载中/已完成/云添加）
- ✅ 任务操作按钮状态动态更新
- ✅ 更多选项菜单（全部开始/暂停、清除已完成、打开目录）

**进行中：**
- ✅ CloudPage 与 libfalcon-storage 集成
- ✅ DiscoveryPage 搜索 API 集成

**待实现：**
- ✅ 网格/卡片视图切换
- ✅ 云添加任务功能
- ✅ 主题切换（亮色/暗色）
- ✅ 系统托盘集成

---

## 核心功能开发进度

### 2026-04-21 - S3 插件与 HTTPS 支持

**已完成：**
- ✅ S3 插件认证功能实现
  - 从 options 中获取认证信息
  - 实现带签名的下载
  - 完善 S3Authenticator 静态方法
- ✅ Daemon RPC HTTP 服务器实现
  - 使用原生 socket 实现 HTTP 服务器
  - 完整的 XML-RPC 参数解析
- ✅ HTTP TLS 握手实现
  - OpenSSL 集成
  - SSL_write/SSL_read 支持
  - HTTPS 请求/响应完整支持
- ✅ Daemon RPC 与 DownloadEngine 集成
  - GID 管理系统
  - aria2.addUri/remove/pause/unpause 实现
  - aria2.tellStatus/getGlobalStat 实现
- ✅ Daemon 配置热重载
  - 实现配置重载回调
- ✅ HTTP 重定向处理
  - 创建新连接跟随重定向
- ✅ HTTP 分块下载框架
  - 多线程分段下载基础实现

### 2026-04-21 - BitTorrent 插件与桌面应用集成

**已完成：**
- ✅ BitTorrent 插件纯 C++ 实现完善
  - 添加 base32Decode 函数（用于磁力链接）
  - 添加 bencodeToString 函数（B编码序列化）
  - 添加 getTrackers 函数（获取 tracker 列表）
  - 添加 urlDecode 函数（URL 解码）
  - 实现纯 C++ 的 BitTorrent 下载框架
    - startDownloadThread 下载线程
    - connectToTracker 连接 tracker
    - findPeersViaDHT DHT 查找
    - downloadPiece 下载 piece
    - verifyPiece 验证 piece
- ✅ S3Browser 资源浏览器实现
  - 已存在于 plugins/s3/s3_browser.cpp
  - 支持 list_directory、create_directory、remove、rename、copy 等功能
  - libcurl 集成实现 HTTP 请求
  - AWS 签名支持
- ✅ CloudPage 与 StorageService 集成
  - 创建 StorageService 桥接层
  - 实现 connect/disconnect、list_directory、download 等功能
  - 配置持久化支持
  - 信号/槽机制连接 UI 与服务
- ✅ DiscoveryPage 与 SearchService 集成
  - 创建 SearchService 搜索服务
  - 支持磁力链接、HTTP、网盘、FTP 资源搜索
  - 后台线程搜索实现
  - 搜索结果回调机制
- ✅ 完整的多线程分段下载（SegmentDownloader 737 行，多线程并行）
- ✅ BitTorrent 插件与 DHT/PEX 集成（完成）
  - ✅ DHT 路由表实现（K-bucket、XOR 距离计算）
  - ✅ DHT 客户端（UDP socket、引导节点）
  - ✅ PEX 管理器（peer 生命周期、候选 peer 管理）
  - ✅ PEX 扩展协议处理器
  - ✅ B 编码实现（BencodeValue 完整编解码）
  - ✅ 完整消息处理逻辑（UDP 收发、路由表更新）
  - ✅ 与 BitTorrentHandler 集成
    - 添加 DhtClient 成员变量
    - 添加 PexExtensionHandler 管理器
    - download() 方法集成 DHT peer 发现
    - pause/resume/cancel 方法完善
    - PEX 处理器辅助方法实现

**待实现：**
- ✅ CloudPage 与 libfalcon-storage 的完整 C++ 绑定
- ✅ DiscoveryPage 与真实搜索 API 集成
- ✅ 网格/卡片视图切换
- ✅ 主题切换（亮色/暗色）
- ✅ 系统托盘集成
- ✅ 云添加任务功能（云盘链接识别）
- ✅ 多线程分段下载完善
- ✅ 更新项目文档

### 2026-04-22 - Desktop 应用功能完善

**已完成：**
- ✅ 网格/卡片视图切换实现
  - 添加 TaskDisplayStyle 枚举（Table/Grid）
  - 实现 create_task_grid() 创建网格视图
  - 实现 set_display_style() 切换视图
  - 实现任务卡片组件（create_task_card）
  - 网格视图右键菜单支持
  - 新增 style_toggle_button_ 切换按钮
- ✅ 系统托盘集成实现
  - 创建 QSystemTrayIcon 托盘图标
  - 实现托盘菜单（显示/退出）
  - 支持单击/双击切换窗口可见性
  - 关闭按钮最小化到托盘
- ✅ 主题切换功能实现
  - 创建 ThemeManager 主题管理器
  - 实现亮色/暗色主题样式表（完整 QSS）
  - 设置页面添加主题切换 UI
  - 主题持久化到配置文件
  - 支持运行时主题切换
- ✅ 云添加任务功能（云盘链接识别）
  - 扩展 UrlProtocol 枚举支持云盘协议
  - 添加云盘链接正则模式检测
  - 支持百度网盘、阿里云盘、夸克网盘、天翼云盘、蓝奏云
  - 解析分享码并显示友好文件名
- ✅ 多线程分段下载功能完善
  - SegmentDownloader 已完整实现
  - HTTP handler 集成分段下载
  - 支持自适应分段、断点续传、慢速检测
  - 完整单元测试覆盖（641 行测试代码）
- ✅ StorageService 与 libfalcon-storage 完整 C++ 绑定
  - 重写 storage_service.cpp 使用真实的 libfalcon-storage API
  - 实现类型转换（falcon::RemoteResource <-> RemoteResourceInfo）
  - 集成 S3Browser、OSSBrowser、COSBrowser、KodoBrowser、UpyunBrowser
  - 后台线程执行网络操作（QtConcurrent）
  - 完整错误处理与信号通知机制
  - 更新 CMakeLists.txt 链接 Falcon::storage
- ✅ DiscoveryPage 真实搜索 API 集成
  - 重写 search_service.cpp 实现真实网络请求
  - 支持多搜索源并行查询（BT 天堂、Torrent Kitty、DHT 网络）
  - HTML 解析提取磁力链接和资源信息
  - 搜索结果去重和数量限制
  - 后台线程执行搜索操作
  - 支持取消搜索功能
- ✅ CloudPage UI 交互完善
  - 实现刷新目录功能（调用 list_directory）
  - 实现下载文件功能（调用 request_download）
  - 实现上传文件功能（新增 upload_file API）
  - 实现删除功能（调用 remove_resource）
  - 实现新建文件夹功能（调用 create_directory）
  - 实现重命名功能（调用 rename_resource）
  - 添加配置名称跟踪（current_config_name_）
  - 完善错误提示和状态反馈

**待实现：**
- ✅ CloudPage 与 libfalcon-storage 的 UI 交互完善
- ✅ DiscoveryPage 与真实搜索 API 集成

---

## CI/CD 修复

### 2026-05-07 - Windows 链接问题修复

**问题：**
- Windows (MinGW) 上链接失败：`undefined reference to register_builtin_protocol_handlers`
- `incremental_download.hpp` 缺少 `<cstdint>` 头文件
- 弱符号机制在 Windows 上需要特殊处理

**已完成：**
- ✅ 添加 `#include <cstdint>` 到 incremental_download.hpp
- ✅ 修复 builtin_protocol_handlers_stub.cpp 的弱符号声明
- ✅ 确保 stub 文件在所有平台上正确编译
- ✅ 验证符号正确解析（弱符号在 core，强符号在 protocols）
- ✅ 所有可执行文件成功编译链接

### 2026-05-07 - BitTorrent DHT/PEX 与 BitTorrentHandler 集成

**目标：**
- 将独立实现的 DHT 客户端和 PEX 协议处理器集成到 BitTorrentHandler
- 实现完整的 peer 发现和交换功能

**已完成：**
- ✅ 在 BitTorrentHandler 中添加 DhtClient 成员变量
- ✅ 在 BitTorrentHandler 中添加 PexExtensionHandler 管理器
- ✅ 实现 startDht() 和 stopDht() 方法
- ✅ download() 方法集成 DHT peer 发现（info_hash 提取和 findPeers 调用）
- ✅ download() 方法集成 PEX 处理器创建和回调设置
- ✅ pause/resume/cancel 方法完善（纯 C++ 模式支持）
- ✅ 添加 getPexHandler() 和 removePexHandler() 辅助方法

**技术要点：**
- DHT 客户端在构造函数中自动启动
- PEX 处理器按 info_hash 管理多个 torrent 下载
- 支持通过 setPexEnabled() 动态启用/禁用 PEX
- 纯 C++ 模式同样支持 DHT/PEX（不依赖 libtorrent）

### 2026-05-07 - PEX 协议完善

**已完成：**
- ✅ 清理空的 bt_plugin.cpp 存根文件
- ✅ 实现 IPv6 字符串解析（stringToIPv6 函数）
  - 支持 ::ffff:x.x.x.x 格式的 IPv4 映射
  - 支持 :: 压缩格式展开
  - 标准十六进制段解析

### 2026-05-07 - 旧协议插件接口重构

**目标：**
- 将 ED2K/Thunder/QQDL/FlashGet/HLS 插件从旧接口迁移到新的 IProtocolHandler
- 在 builtin_protocol_handlers.cpp 中注册所有协议处理器

**已完成：**
- ✅ ED2K 插件重构 (ED2KPlugin → ED2KHandler)
  - 新接口方法：protocol_name(), supported_schemes(), can_handle(), get_file_info(), download(), pause(), resume(), cancel()
  - 添加 TaskContext 和活动任务管理
  - Factory 函数：create_ed2k_handler()
- ✅ Thunder 插件重构 (ThunderPlugin → ThunderHandler)
  - 新接口实现
  - Base64 解码支持
  - Factory 函数：create_thunder_handler()
- ✅ QQDL 插件重构 (QQDLPlugin → QQDLHandler)
  - 新接口实现
  - GID 格式解析
  - Factory 函数：create_qqdl_handler()
- ✅ FlashGet 插件重构 (FlashGetPlugin → FlashGetHandler)
  - 新接口实现
  - 镜像链接支持
  - Factory 函数：create_flashget_handler()
- ✅ HLS 插件重构 (HLSPlugin → HLSHandler)
  - 新接口实现
  - M3U8 播放列表解析
  - Factory 函数：create_hls_handler()
- ✅ builtin_protocol_handlers.cpp 更新
  - 添加所有插件的头文件引用
  - 在 register_builtin_protocol_handlers() 中注册所有处理器

**技术要点：**
- 所有插件统一使用 IProtocolHandler 接口
- 支持任务暂停、恢复、取消
- 异步下载线程模型
- 错误处理和事件回调机制

### 2026-04-30 - GitHub Actions Nightly Build 修复

**问题：**
- macOS: DMG 文件在 `build-desktop/bin/` 创建，但上传时在根目录查找
- Linux: `${Qt6_DIR}/bin/linuxdeploy-x86_64.AppImage` 不存在
- Windows: vcpkg 缺少 curl 依赖

**已完成：**
- ✅ macOS DMG 输出路径改为 `../../falcon-desktop-macos-${VERSION}.dmg`
- ✅ Linux: 添加 linuxdeploy 和 linuxdeploy-plugin-qt 安装步骤
- ✅ Linux: 修改打包步骤使用全局安装的 linuxdeploy
- ✅ Windows: 添加 `.\vcpkg\vcpkg install curl:x64-windows`
- ✅ Windows: 更新 Package 步骤正确复制 vcpkg DLL

---

### 2026-05-07 - HTTP 分块传输编码与 BitTorrent Peers 集成

**已完成：**
- ✅ BitTorrent DHT/PEX peers 添加到下载任务
  - 在 libtorrent 模式下使用 `connect_peer()` 添加 DHT/PEX 发现的 peers
  - 维护 `torrentHandles_` 映射以支持回调中的 peer 添加
  - 在 cancel 方法中正确清理句柄映射
- ✅ HTTP 分块传输编码解析
  - 实现完整的分块编码状态机（READ_SIZE, READ_DATA, READ_CR, READ_LF, READ_TRAILER）
  - 支持十六进制块大小解析
  - 支持可选的尾部头部
  - 提供 Windows memmem 兼容实现
- ✅ HTTP 重试命令实现
  - 创建新的 `HttpInitiateConnectionCommand` 重试下载
  - 使用 `schedule_next` 调度新命令
- ✅ 资源搜索功能完善
  - 添加 `response_format`、`selectors`、`path_pattern` 成员到 `SearchEngineConfig`
  - 实现 `parse_json_response` 方法支持三种 JSON 格式（results 数组、顶层数组、单对象）
  - 实现 `path_pattern` 支持路径模式替换（`{query}`, `{query_letter}`, `{first_char}`, `{page}`）
  - 启用配置文件中 `response_format`/`selectors`/`path_pattern` 的解析

**技术要点：**
- BitTorrent: `torrentHandles_` 映射管理，`connect_peer()` 添加 peers
- HTTP: 5态解析器，缓冲区管理，CRLF 处理，memmem Windows 兼容
- Resource Search: nlohmann/json 解析，多格式支持，路径模式替换

### 2026-05-07 - 协议委托机制与完整下载实现

**已完成：**
- ✅ 协议处理器扩展接口 (IProtocolHandlerExtension)
  - 新增 `protocol_handler_extension.hpp` 定义扩展接口
  - `set_protocol_registry()` 方法允许处理器访问 ProtocolRegistry
  - `get_extension()` 辅助函数进行 dynamic_cast 转换
- ✅ DownloadEngine 集成扩展接口
  - `load_all_handlers()` 后自动设置 registry
  - 支持内置处理器和工厂创建的处理器
- ✅ Thunder 插件委托下载实现
  - 实现 `IProtocolHandlerExtension` 接口
  - 解析迅雷链接后委托给实际协议处理器（HTTP/FTP/BT等）
  - 添加 `delegateDownload()` 方法处理协议委托
- ✅ ED2K 插件委托下载实现
  - 实现 `IProtocolHandlerExtension` 接口
  - 从 ED2K 源地址列表尝试下载
  - 添加 `downloadFromSources()` 和 `delegateDownload()` 方法
- ✅ HLS 插件完整下载逻辑实现
  - 实现 `IProtocolHandlerExtension` 接口
  - `downloadM3U8()` - 下载播放列表
  - `downloadSegment()` - 下载单个媒体段
  - `mergeSegments()` - 合并所有段到最终文件
  - `downloadAllSegments()` - 并行下载所有段（最大并发 4）

**技术要点：**
- 协议委托: 通过 `IProtocolHandlerExtension` 注入 ProtocolRegistry
- Thunder: Base64 解码 + 链接解析 → 委托给目标协议处理器
- ED2K: 解析源地址 → 尝试 HTTP 下载 → 失败则尝试下一个源
- HLS: M3U8 解析 → 段并行下载 → 二进制合并 → 临时文件清理

### 2026-05-07 - FlashGet 和 QQDL 插件委托下载实现

**已完成：**
- ✅ FlashGet 插件委托下载实现
  - 实现 `IProtocolHandlerExtension` 接口
  - 解析快车链接后委托给实际协议处理器
  - 支持 `flashget://` 和 `fg://` 格式
- ✅ QQDL 插件委托下载实现
  - 实现 `IProtocolHandlerExtension` 接口
  - 解析 QQ 旋风链接后委托给实际协议处理器
  - 支持 `qqlink://` 和 `qqdl://` 格式

**技术要点：**
- FlashGet: `[FLASHGET]` 前缀处理 + Base64/URL 解码 → 委托
- QQDL: GID 格式解析 + Base64 解码 → 委托
- 所有包装协议现在都通过统一的 `delegateDownload()` 方法委托

### 2026-06-14 - 网盘插件矩阵扩展（10 个平台默认注册）

**目标：**
- 将 `CloudStorageManager` 的默认插件从单一 `LanzouCloudPlugin` 扩展到主流网盘全覆盖
- 让任意主流网盘链接都能被正确识别并路由到对应插件

**已完成：**
- ✅ 新增 `BaiduNetdiskPlugin`（百度网盘）
  - 解析 `pan.baidu.com/s/...`、`yun.baidu.com/s/...`、`baidupan://` 链接
  - 通过分享页面提取文件标题作为候选文件名
- ✅ 新增 `AliyunDrivePlugin`（阿里云盘）
  - 解析 `aliyundrive.com`、`alipan.com`、`alipan://` 链接
  - 分享页面标题解析
- ✅ 新增 `QuarkDrivePlugin`（夸克网盘）
  - 解析 `pan.quark.cn/s/...`、`quark://` 链接
- ✅ 引入 `LightweightCloudPluginBase` 模板基类
  - 统一识别/规范化/HTTP 标题解析流程
  - DRY 原则：消除每个轻量插件 ~200 行模板代码重复
  - 子类只需提供平台名、平台枚举、错误提示
- ✅ 基于 `LightweightCloudPluginBase` 实现 7 个轻量插件
  - `TencentWeiyunPlugin`（腾讯微云）
  - `Cloud115Plugin`（115 网盘）
  - `PikPakPlugin`（PikPak）
  - `MegaPlugin`（MEGA）
  - `GoogleDrivePlugin`（Google Drive）
  - `OneDrivePlugin`（OneDrive）
  - `DropboxPlugin`（Dropbox）
- ✅ `CloudStorageManager::register_default_plugins()` 注册全部 10 个插件
- ✅ 单元测试补充
  - `BaiduNetdiskPluginRegistered`、`AliyunDrivePluginRegistered`、`QuarkDrivePluginRegistered`
  - `DefaultPluginCount`（≥4 个默认插件）
  - `BaiduNetdiskLinkRouting`、`AliyunDriveLinkRouting`、`QuarkDriveLinkRouting`
  - 验证链接路由到正确插件的 `platform_name` 与 `platform_type`
- ✅ g++ 语法验证通过（缺 curl/json/gtest 的本地环境用最小 stub）

**技术要点：**
- 设计上区分两类插件：
  1. **完整插件**（如 `LanzouCloudPlugin`）：实现 `get_download_url` 等真实拉取逻辑
  2. **轻量插件**（基于 `LightweightCloudPluginBase`）：仅完成识别 + 元数据 + 路由，直链获取待后续接入各平台 API
- 所有 `extract_share_link` 都返回 `success=true`，并通过 `error_message` 提示需要何种额外步骤（客户端/账号/插件）才能完成真正下载
- `can_handle()` 委托给 `CloudLinkDetector::detect_platform()`，保证识别规则集中维护

**待实现（后续迭代）：**
- 各网盘平台的真实直链获取 API（需账号态/OAuth/API Key）
- YandexDisk 插件（URL 模式已在 `CloudLinkDetector` 注册，缺 plugin 实现）
- 集成测试覆盖（mock HTTP 服务器）

### 2026-06-15 - 提取结果语义重构 + YandexDisk 补全

**目标：**
- 修正「轻量插件返回 `success=true` 但实际无直链」的语义混淆
- 让上层能精确区分「无法识别」「识别成功但需额外步骤」「可直接下载」三种状态
- 补齐最后一个缺失的 YandexDisk 插件

**已完成：**
- ✅ `CloudExtractionResult` 引入 `recognized` 字段
  - `success=true`：完整解析成功（含可直接下载的 URL）
  - `recognized=true / success=false`：识别到平台并提取了元数据，但直链需额外步骤
  - `recognized=false`：完全无法识别或处理失败
- ✅ `LightweightCloudPluginBase` 基类改为返回 `recognized=true, success=false`
- ✅ `BaiduNetdiskPlugin`、`AliyunDrivePlugin`、`QuarkDrivePlugin` 三个早期实现同步语义
- ✅ `CloudStorageManager::get_direct_download_url` 增加 recognized 诊断日志
- ✅ 新增 `YandexDiskPlugin` 轻量插件
  - URL 模式 `disk.yandex.ru/d/...` 和 `yadi.sk/d/...`
  - 提示直链可通过 `?dl=1` 参数或 Yandex Disk API 获取
- ✅ 默认插件矩阵扩充至 11 个（蓝奏云、百度网盘、阿里云盘、夸克、腾讯微云、115、PikPak、MEGA、Google Drive、OneDrive、Dropbox、Yandex Disk）
- ✅ 单元测试同步新语义
  - LinkRouting 测试改为 `EXPECT_TRUE(recognized) + EXPECT_FALSE(success)`
  - 新增 `YandexDiskLinkRouting` 测试
  - 新增 `UnknownLinkNotRecognized` 测试覆盖「完全无法识别」分支
  - 新增 `YandexDiskPluginRegistered` 测试
  - `DefaultPluginCount` 阈值从 4 提升到 11

**技术要点：**
- 语义重构遵循「最小惊讶原则」：`success` 的含义保持「可直接下载」，避免破坏老代码对 `get_direct_download_url` 的假设
- `recognized` 是「软成功」标志，上层 UI 可据此显示「识别到 X 网盘，需要客户端才能下载」类提示
- YandexDisk 利用 `LightweightCloudPluginBase` 模板，整个插件实现仅需 ~15 行

### 2026-06-15 - resource_search 编译修复 + selectors 通用解析实现

**目标：**
- 修复 `resource_search.cpp` 多个被本地环境（无 curl/json）掩盖的真实编译问题
- 实现 `selectors` 配置驱动的通用 HTML 解析，取代站点硬编码
- 暴露可单元测试的内部接口

**已完成：**
- ✅ 真实编译错误修复
  - 删除重复的 `parse_json_response` 方法（行 304 与行 390 完全相同签名）
  - 修复 `result.leeches` → `result.peers`（字段不存在于 `SearchResult`）
  - 添加缺失的 `#include <iomanip>`（`std::setw` 在 `url_encode` 中使用）
  - 删除无调用者的 `replace_all(std::string&, ...)` 重载，消除与按值版本的重载歧义
- ✅ 实现 selectors 通用 HTML 解析
  - `SearchEngineConfig::selectors` 中配置正则模式映射
  - 必需键：`item`（用于切分结果项）
  - 可选键：`title/url/magnet/size/seeds/peers/leeches/hash/date/type`
  - 支持任何配置驱动的搜索引擎，无需修改代码即可接入新站点
  - 向后兼容：未配置 selectors 时返回空（移除 1337x 硬编码回退以保持架构纯净）
- ✅ 重构 GenericSearchProvider 提升可测试性
  - 将 `parse_html_by_selectors / apply_selector_field / calculate_confidence / parse_size` 实现下沉到 `falcon::search::detail` 命名空间
  - GenericSearchProvider 内同名方法保留为转发（避免改动其他调用点）
  - 暴露在 header 中以便单元测试直接调用，无需 WebCrawler/libcurl
- ✅ 单元测试覆盖（新增 7 个测试用例）
  - `ParsesMultipleItems`：验证多 item 解析、字段映射、leeches→peers
  - `ReturnsEmptyWhenItemMissing`：未配置 item selector 时返回空
  - `ReturnsEmptyWhenHtmlEmpty`：空 HTML 安全处理
  - `SkipsItemsMissingTitleOrUrl`：不完整项被过滤
  - `InvalidRegexIsHandled`：无效正则不崩溃（被 try/catch 捕获）
  - `MagnetFieldSetsType`：magnet 字段自动设置 type="magnet"
  - `ApplyFieldMapsAllKnownKeys`：所有字段映射覆盖（含 metadata）
  - `ConfidenceScoringSanity`：置信度评分单调性

**技术要点：**
- 测试代码使用 `R"re(...)re"` raw string 形式避免 `)"` 序列冲突
- `detail` 命名空间是「为测试暴露的内部 API」，明确区分公开 API 和实现细节
- selectors 设计遵循 ISP（接口隔离）：SearchEngineConfig 是数据载体，detail 是无状态函数集
- LSP：通过 detail::parse_size 让 GenericSearchProvider::parse_size 行为与外部测试完全一致

### 2026-06-16 - download_engine_v2 等待命令超时清理机制

**目标：**
- 修复事件驱动下载引擎中「park 后 socket 永不就绪」的资源泄漏场景
  - 对端异常断开但 EventPoll 未触发（部分平台行为）
  - one-shot 监听丢失、注册时序竞态等
- 防止 `waiting_commands_` / `socket_wait_map_` / `socket_command_map_` 长期累积
- 单元测试可验证清理行为，无需真实网络/EventPoll

**已完成：**
- ✅ EngineConfigV2 新增可配置 `command_wait_timeout_seconds`（默认 120s）
- ✅ 头文件补充 `#include <map>`（修复 `socket_command_map_` 用 `std::map` 时的潜在编译错误）
- ✅ `execute_commands` 在 park 命令时记录时间戳到 `waiting_command_times_`
- ✅ `register_socket_event` 回调恢复命令时同步清除时间戳
- ✅ `cleanup_completed_commands` 实现：
  - 双阶段：锁内收集超时项与对应 fd，锁外执行 EventPoll.remove_event 与日志
  - timeout <= 0 时直接返回（提供禁用清理的能力）
  - 一次扫描清理全部关联映射：`waiting_commands_` / `waiting_command_times_` /
    `socket_wait_map_` / `socket_command_map_`，并移除 EventPoll 监听
- ✅ 友元测试访问：`friend class ::DownloadEngineV2Test`（全局命名空间前向声明）
- ✅ 单元测试新增 6 个用例：
  - `Config_DefaultCommandWaitTimeout`：默认值断言
  - `CleanupCompletedCommands_NoWaiting_NoOp`：空状态调用安全
  - `CleanupCompletedCommands_DisabledWhenZeroTimeout`：禁用时映射保持不变
  - `CleanupCompletedCommands_RemovesExpiredEntries`：超时项被全量清除
  - `CleanupCompletedCommands_PreservesFreshEntries`：未超时项保留
  - `CleanupCompletedCommands_PartialExpiry`：混合场景下选择性清理
  - `CleanupCompletedCommands_DropsParkedCommandOwnership`：超时命令的
    `unique_ptr<Command>` 被移出并销毁（避免命令对象泄漏）

**技术要点：**
- 锁外执行 `event_poll_->remove_event` 与日志，避免在 socket_map_mutex_ 持有期间
  触发平台 IO，符合「不在锁内做 IO」的最佳实践
- 使用 `steady_clock` 而非 `system_clock`，避免系统时间回拨导致的错误清理
- 测试通过 friend 直接注入 `time_point::min()` 模拟「远古」时间戳，无需 sleep，
  避免单元测试变慢；这种 fake-time-by-injection 模式适用于无法重构为 Clock
  抽象的存量代码
- DRY：所有超时清理逻辑集中在 `cleanup_completed_commands`，主循环只负责调用

### 2026-09-07 - 覆盖率测试套件收尾与真实缺陷修复

**背景：**
- 上一会话为 CLI/Daemon/Protocols/Drives 补充了大量覆盖率测试（11 个新测试文件 +
  对应 CMake 注册），并顺带做了若干小修复（builtin 协议锚点对象库、socket_pool
  size() 语义、json_rpc multicall 校验、CLI -C 指定默认配置输出路径等）
- 全套构建后 ctest 有 11 个失败，本次逐一定位并修复

**测试基建修复（测试代码自身缺陷）：**
- ✅ CLI 集成测试管道泄漏：`exec_child` 未关闭子进程继承的 `in_pipe[1]` 写端，
  导致 `-i -`（stdin 读 URL）永远读不到 EOF、子进程挂起 48s 后被 SIGKILL。
  修复：所有管道 fd 在 fork 前设置 `FD_CLOEXEC`（dup2 目标 fd 不继承 CLOEXEC，
  execve 后未用端自动关闭）
- ✅ 批量下载断言错位：失败汇总行 `N FAILED` 实际输出在 stdout（generate_summary），
  每任务 `FAIL <url>` 在 stderr；修正断言流归属
- ✅ 取消路径测试前提不成立：连接拒绝（127.0.0.1:1）立即失败不重试，无法构造
  「长时间运行的下载」。新增 `open_stall_listener()`（listen 后永不 accept 的
  停滞服务器），让 SIGINT / 交互 q 键取消测试有真实的进行中任务可取消
- ✅ `DaemonizeSuccessAndSignals`：子进程分支直接 `std::exit(0)` 跳过局部
  `DaemonManager` 析构，pid 文件永不删除。修复：子进程逻辑收进
  `run_daemonize_success_probe()` 函数，依赖作用域结束触发析构
- ✅ `AddUriReturnsGidAndTellStatusReflectsOptions` 竞态：addUri 立即 start_task，
  StubHandler 瞬时完成（100/100），tellStatus 与引擎工作线程存在时序竞态。
  修复：轮询等待终态后断言 `complete` + 100/100

**真实产品缺陷修复：**
- ✅ falcon-daemon 前台模式 SIGTERM 无法优雅退出
  - 根因：`setup_signal_handlers()` 仅在 `daemonize()` 内调用；前台模式
    （未守护化）从不安装信号处理器，SIGTERM 按默认行为直接杀死进程
  - 修复：`setup_signal_handlers()` 提升为 public（头文件补充说明注释），
    main.cpp 在前台分支进入 run() 前显式调用
  - 影响：前台运行下 SIGTERM/SIGINT 现在走 `stop()` → run 循环退出 →
    stop 回调 → exit 0，与守护模式行为一致

**验证：**
- ✅ build-cov 全量构建通过
- ✅ ctest 1391/1391 全部通过（连续两轮）

**已知问题（未处理）：**
- `DownloadEngineTest.EventListener` 在高并行负载下偶发失败（单独运行稳定通过），
  属既有的时序敏感测试，非本次改动引入

### 2026-09-07 - 修复 V2 引擎 HTTP 分段分支文件截断 bug

**问题（严重，数据损坏）：**
- `HttpResponseCommand::process_response` 中，当 `Accept-Ranges: bytes` 且
  `content_length_ > min_segment_size` 时进入「多线程分段」分支，但该分支只
  调度了第 0 段（长度 `segment_size`），其余分段的连接命令创建后直接丢弃：
  - 下载到 `segment_size` 字节即触发完成判定（`check_completion`），
    **文件被截断却标记为 Completed**
  - 即使不截断，后续收到的数据仍会被 `write_to_segment` 全量顺序写入
    （不按 offset 定位），多段并发写同一 `ofstream` 也会交错损坏

**修复：**
- ✅ V2 引擎 HTTP 下载统一走单连接完整 body（`length=content_length_`），
  与分支前注释「单连接下载」的设计意图一致
- ✅ `accepts_range_` 保留为元数据（断点续传能力探测），不再用于调度分段
- ✅ 代码注释说明多连接分段的前置条件：每段独立连接 + 按 offset 定位写入
  （或段文件最后合并）

**测试（回归保护）：**
- ✅ 原 `Response200WithAcceptRangesUsesSegments` 只断言标志位，无法捕获截断；
  重写为 `Response200WithAcceptRangesDownloadsFullBody`：
  - 响应头与 body 分批投递（socketpair），第一批恰为旧分支的 `segment_size`
  - 关键断言：body 未到齐时任务必须仍是 `Downloading`（旧代码此处置信 Completed）
  - 第二批 + 对端关闭后经事件回调恢复命令，最终断言文件内容完整
- ✅ 已验证：回滚修复后该测试稳定失败，应用修复后通过
- ✅ 测试基建：`HttpCommandsCoverageTest` 夹具作为 `DownloadEngineV2` 友元
  （头文件前向声明），通过夹具成员函数驱动 private `execute_commands()` 与
  `event_poll_->poll()`——gtest TEST_F 测试体位于派生类，友元不继承

**验证：**
- ✅ ctest 1391/1391 全部通过

### 2026-09-07 - 编译警告清零、TODO 治理与 ProtocolRegistry 并发修复

**编译警告：87 → 0**（-Wall -Wextra -Wconversion -Wsign-conversion 全严格集，含 BT 启用构建）

生产代码：
- ✅ `incremental_download.cpp`：18 处 streamoff/streamsize 符号转换显式化；
  占位函数未使用参数 `(void)` 化并注明待实现
- ✅ `http_commands.cpp`：`memmem_alt` 包裹进 `#ifdef _WIN32`（仅 Windows 经宏
  使用，Linux 下未使用告警）；块大小行偏移 ptrdiff_t → size_t 显式转换
- ✅ `builtin_protocol_handlers.cpp`：`registry` 参数 `[[maybe_unused]]`
  （全部插件宏裁剪时未使用）
- ✅ storage 浏览器插件：cos/oss/s3 未使用参数 `[[maybe_unused]]`；
  OpenSSL `HMAC`/`BIO_write` 的 int 参数显式转换；`time_t`/`std::stoul` 转换；
  `ftp_browser.cpp` 废弃 API `CURLINFO_CONTENT_LENGTH_DOWNLOAD` →
  `CURLINFO_CONTENT_LENGTH_DOWNLOAD_T`（负值 -1 视为未知大小）；
  本地 `LOG_*(msg, ...)` 变参宏（无参调用触发 -Wc++20-extensions）统一替换
  为项目 `FALCON_LOG_*`
- ✅ bittorrent 插件：bencode `intValue`（int64_t）→ Bytes 显式转换（torrent
  长度字段按规范非负）；节点 ID 生成循环改用 size_t 索引

测试代码：
- ✅ 未使用变量清理（common_utils/version/thread_pool/segment_downloader/
  event_poll 等 12 处）
- ✅ 6 处 `[[nodiscard]] add_task` 忽略返回值显式 `static_cast<void>`
- ✅ `int` 迭代变量索引容器/赋值无符号字段的 ~25 处显式转换
- ✅ `socket_pool_test` 端口 99999 溢出为 34463（测试意图缺陷）：改用合法
  uint16_t 端口 65535，广播地址不可达本身即触发失败路径
- ✅ `protocol_registry_test` 两处"取出后不断言"的变量改为真实行为断言

**真实 bug 修复：ProtocolRegistry 并发数据竞争**
- 根因：`register_handler` 及全部读方法无锁并发访问 `std::unordered_map`，
  多线程注册时条目丢失（UB）。`ConcurrentPluginRegistration` 偶发
  `handler_count()=9` 即此竞争所致
- 修复：`std::shared_mutex`（读共享/写独占）+ `find_handler_unlocked` 内部
  辅助函数——`get_handler_for_url` 内部递归查找在持锁状态下进行，
  避免 shared_mutex 不可重入死锁；`protocol_name()` 在锁外调用（不在锁内
  执行外部代码）
- 头文件注明线程安全契约

**真实 bug 修复：EventListener 测试竞态**
- `on_completed` 经 EventDispatcher 工作线程异步送达（设计行为），测试在
  `wait_for`（任务终态）后立即断言事件计数，高负载下事件尚未出队
- 修复：改为带 2s 超时的轮询等待

**TODO/FIXME 治理（6 处）：**
- 删除 3 处过时标记：
  - `http_request.hpp` ×2「占位实现/TODO 完整实现」——类功能完整
    （请求构建 to_string/响应头存储），注释更新为真实职责
  - `hls_plugin.cpp`「TODO 实际实现需要 4 步」——M3U8 下载全流程已实现
    （downloadM3U8/downloadAllSegments/mergeSegments），改为流程说明注释
- 保留 4 处真实待办并精确化（异步查找回调接线、Kademlia 迭代逼近、
  spdlog 迁移），补充见下方「未完成事项」

**验证：**
- ✅ 全量构建零警告（标准构建 + BT 启用构建）
- ✅ ctest 1391/1391 连续 5 轮全部通过（两个偶发失败根因均已修复）

### 2026-09-07 - V2 引擎多连接分段下载实现

**目标（来自「未完成事项」#4）：**
- V2 引擎 HTTP 下载在服务器支持 Range 时启用多连接分段下载：
  每段独立连接 + 按 offset 定位写入，替代此前的恒单连接模式

**核心机制（aria2 风格命令链）：**
- ✅ 分段计划 `compute_http_segment_ranges()`（公共 API，可单测）：
  段数 clamp 到 [1, max_connections] 且不超过 content/min_segment_size
  （每段平均长度不低于最小分段大小），余数并入最后一段——与
  SegmentDownloader 的等分策略一致
- ✅ `HttpInitiateConnectionCommand::set_range(segment_id, offset, length)`：
  分段连接在 `prepare_http_request()` 附加 `Range: bytes=A-B` 头，
  并把分段信息传递给响应命令
- ✅ `HttpResponseCommand` 分段分支：段 1..N-1 的响应必须为
  **206 Partial Content** 且 `Content-Length == 计划段长`
  （服务器忽略 Range 返回 200 或长度不符 → 该段判定失败，防止数据错位）
- ✅ `HttpResponseCommand::schedule_multi_segment_download()`：段 0 复用
  当前连接，段 1..N-1 各自创建新连接命令（每连接携带原始 URL）
- ✅ `HttpDownloadCommand` 定位写入：段 >0 以
  `in|out` 模式打开（不截断），每次写入前 `seekp(offset + downloaded)`；
  **越界保护**：超出计划段长的数据被丢弃（同时惠及单连接模式——
  服务器超额发送不再写入文件）
- ✅ 完成门控：`RequestGroup` 新增分段跟踪器
  （`begin_multi_segment/finish_segment/has_segment_failure`，互斥保护）。
  仅当全部分段结束且无失败时置任务 Completed/组 COMPLETED；
  任一分段失败立即置任务 Failed/组 FAILED，其余分段静默退出
  （不覆盖终态）
- ✅ 进度聚合：多连接模式下任务进度 = `group->downloaded_bytes() /
  file_info().total_size`（段内局部进度不再误报为任务进度）
- ✅ 失败传播：分段连接的连接/TLS/发送失败经
  `notify_segment_failure()` 传播到任务与组

**兼容性说明：**
- `HttpResponseCommand` 新参数均为带默认值的可选参数，既有调用点不变
- 单连接回退路径完整保留：不接受 Range / 文件小于 min_segment×N /
  max_connections<=1 / 手工构造的响应命令（无 source_url）均走单连接
- 既有回归测试 `Response200WithAcceptRangesDownloadsFullBody` 因
  source_url 为空自动走单连接路径，仍验证截断防护

**测试（9 个新增）：**
- ✅ 分段计划 5 个：等分/余数并入末段/min_segment 钳制/退化输入
- ✅ 定位写入 2 个：预置文件 + offset 写入、分批到达（park/恢复链路）
- ✅ 越界保护 1 个：段长 8 收到 16 字节，多余 8 字节丢弃
- ✅ 端到端 1 个：本地 206 Range 服务器（支持 `Range: bytes=A-B` 解析、
  200 全量 + Accept-Ranges、206 切片）+ `engine.run()` 完整事件循环，
  验证 4 连接并发下载 16KB 文件内容与原数据逐字节一致

**顺带修复（测试基建 use-after-free）：**
- `download_engine_v2_run_test.cpp` 的 `InstantCommand/SocketWaitCommand/
  RequeueCommand` 在 `run()` 期间被引擎弹出销毁，测试仍持有裸指针断言
  （读悬垂内存得到垃圾值）。改为 `shared_ptr<atomic<int>>` 共享计数器

**验证：**
- ✅ ctest 1400/1400 连续 3 轮全部通过（新增 9 个测试）
- ✅ 全量构建零警告（标准 + BT 启用构建）

### 2026-09-07 - 增量下载功能实现（远程哈希列表 + Range 下载）

**目标（来自「未完成事项」#5）：**
- 实现 `IncrementalDownloader::downloadRemoteHashList` 与
  `downloadRange` 的真实逻辑，打通 compare → downloadChanged 全流程

**核心实现：**
- ✅ `http_get(url, out)`：阻塞式 libcurl GET（FALCON_USE_CURL 守卫；
  连接 10s / 总时长 60-120s 兜底超时，FOLLOWLOCATION + FAILONERROR）
- ✅ `downloadRange(url, offset, size)`：`CURLOPT_RANGE` 执行 HTTP Range
  请求；响应字节数必须与请求长度一致（不符返回空）。
  **修复**：CURLOPT_RANGE 的值是原始字节范围（`"1024-2047"`），curl 自动
  添加 `Range: ` 前缀——传 `"bytes=..."` 会产生 `Range: bytes=bytes=...`
- ✅ `downloadRemoteHashList(url, ...)`：按约定请求 `<file url>.falconhash`
  并解析；哈希列表文本格式：
  ```
  # falcon-hash-list v1
  # chunkSize: <n> / # algorithm: <alg> / # fileSize: <n> / # chunks: <n>
  <hex hash 行，按分块顺序>
  ```
- ✅ `serializeHashList()`（公共静态）：generateHashList 结果 → 文本，
  可部署为服务端 .falconhash 文件
- ✅ `parseHashList()`（公共静态）：严格校验——哈希行必须为偶数长度
  十六进制；`chunks` 计数与哈希行数一致；algorithm 与调用方期望一致
  （不一致 → 不可比较 → 空列表）；fileSize 元数据收缩最后一块实际大小
- ✅ `downloadChanged` 本地读取越界修复：localSize > remoteSize 时
  clamp 到可容纳字节数（此前 read 会越界）
- ✅ 无 libcurl 构建保持优雅回退（警告 + 空结果）

**测试（新增 5 个，共 13 个通过）：**
- ✅ 哈希列表序列化/解析往返（元数据覆盖默认参数、末块收缩）
- ✅ 损坏列表拒绝：非法 hex / 奇数长度 / chunks 计数不符 / 算法不匹配
- ✅ 端到端：本地服务器（/file.bin 支持 Range、/file.bin.falconhash），
  compare 精确识别唯一变化分块（1024/2500 = 40.96%），
  downloadChanged 输出与远程内容逐字节一致
- ✅ 无变化场景（totalChanged=0）与本地文件缺失场景（全量下载）
- ✅ `Compare_Integration` 的 example.com 改为立即拒绝的回环端口
  （保持离线快速失败）

**验证：**
- ✅ ctest 1406/1406 连续 3 轮全部通过
- ✅ 全量构建零警告（标准 + BT 启用构建）

### 2026-09-07 - spdlog 日志后端迁移（logger.hpp）

**目标（来自「未完成事项」#1）：**
- 手写流式 logger + FALCON_LOG_* 宏迁移到 spdlog；
  core 的 CMake 此前已 PUBLIC 链接 spdlog 并定义 `FALCON_USE_SPDLOG`
  （vcpkg 构建下 spdlog_FOUND=true），但 logger.hpp 从未实现后端

**迁移设计（接口零变更，调用点零改动）：**
- ✅ 双后端结构：`#ifdef FALCON_USE_SPDLOG` → spdlog 后端；`#else` →
  原手写实现原样保留（无 spdlog 的最小构建继续可用）
- ✅ 公共接口完全不变：`LogLevel` / `get_log_level` / `set_log_level` /
  `log_*` 函数 / `FALCON_LOG_*` 与 `FALCON_LOG_*_STREAM/FMT` 宏签名一致
- ✅ 自定义 `FalconConsoleSink`（base_sink<std::mutex>）精确复刻旧输出：
  `[LEVEL] message\n`（TRACE..CRITICAL 全大写），WARN 及以上 → stderr，
  其余 → stdout，每条消息立即 flush（等价旧 `std::endl` 行为）
- ✅ 消息以纯文本 payload 进入 spdlog：`logger->log(level, "{}", msg)`，
  消息内含 `{}`/`{0}` 等字符按数据处理（有测试覆盖，不触发 fmt 错误）
- ✅ FMT 风格宏沿用 `detail::format_log_message` 预格式化（ostream 渲染
  任意类型参数的兼容层），避免 fmt 编译期格式化器要求破坏数百个调用点；
  后续可按调用点逐个迁移到原生 fmt 语法
- ✅ `set_log_level` 同步 spdlog logger 级别（含越界 int clamp：
  <0 → off，>Trace → trace）
- ✅ 新增 `falcon_logger()` 访问器（高级用法：附加 sink / flush）
- ✅ STREAM 宏统一为「级别快速判断 → ostringstream → log_*」，
  两种后端行为一致（旧实现绕过 log_* 直写流的重复逻辑消除）

**顺带清理：**
- ✅ `ftp_browser.cpp` / `s3_browser.cpp`：删除本地 `LOG_*(msg, ...)`
  变参宏定义（调用点已迁移 FALCON_LOG_*，宏为死代码）；
  printf 风格 `"…%s"` + 空参的调用点改为纯文本
- ✅ `calculateHash` 无 OpenSSL 回退分支 `(void)algorithm`
  （无 vcpkg 构建的警告）
- ✅ 删除 `logger.hpp` 顶部的「TODO: Replace with spdlog」标记（本次完成）

**测试（新增 5 个，spdlog 构建下编译）：**
- ✅ `logger_spdlog_test.cpp`（callback_sink 捕获验证）：
  宏/FMT/STREAM/log_* 全路由 spdlog、级别过滤全宏生效、
  set_log_level 双向同步（含越界 clamp）、消息花括号按数据处理、
  logger 命名与 sink 完整性；无 spdlog 构建编译占位测试
- ✅ 运行时输出格式回归：daemon 前台输出 `[INFO] ` 前缀与旧版逐字符一致

**验证：**
- ✅ ctest 1411/1411 连续 3 轮全部通过（vcpkg/spdlog 路径）
- ✅ 三种构建配置零警告：vcpkg（spdlog 后端）、vcpkg+BT、无 vcpkg
  （回退后端 + 无 OpenSSL）
- ✅ 安装导出：`FALCON_PACKAGE_NEEDS_SPDLOG` → Config.cmake 中
  find_dependency(spdlog) 既有接线不变

## 未完成事项（代码内 TODO 对应的架构级待办）

1. ~~**日志库迁移**~~ ✅ 已完成（2026-09-07，见上方条目；
   后续可选：FMT 风格调用点逐个迁移到 spdlog 原生 fmt 语法、
   异步 sink / 文件轮转 sink 配置化）
2. ~~**DHT 异步查找**~~ ✅ 已完成（2026-09-13，见下方条目）
3. ~~**DHT 迭代查找**~~ ✅ 已完成（2026-09-13，见下方条目）
4. ~~**V2 引擎多连接分段下载**~~ ✅ 已完成（2026-09-07，见上方条目）
5. ~~**增量下载远程哈希列表/Range 下载**~~ ✅ 已完成（2026-09-07，见上方条目；
   剩余可选增强：rsync rolling-hash 算法、增量结果端到端哈希校验）

### 2026-09-13 - DHT Kademlia 迭代查找与异步回调接线

**目标（原未完成事项 #2/#3）：**
- `findPeers/findNode` 的回调参数从未接线（`pendingRequests_` 只读不写，
  永远为空）；`performLookup` 只对最接近的 8 个节点做单轮 fire-and-forget
  查询。实现完整的异步 Kademlia 迭代查找

**实现：**
- ✅ `LookupContext`（替代从未使用的死结构 `DhtLookupRequest`）：
  候选集/已查询/待响应/已发现 peers/已响应节点 + 回调，按查找
  （lookupId）管理，支持并发查找互不干扰
- ✅ 迭代驱动：`continueLookup` 每轮向最近的未查询候选（α=3）发查询并
  注册事务回调；响应（`handleLookupResponse`）吸收 compact nodes/values
  后继续下一轮；候选耗尽且全部响应收齐 → `finalizeLookup` 收敛终结
- ✅ 回调语义：收敛/超时时一次性上报——peers 回调收到全部发现的
  peers（空列表同样触发，表示查找结束）；node 回调对最近的已响应
  节点（≤k）逐个触发
- ✅ 已查询/待响应以端点（ip:port）为键而非节点 ID——引导节点的 ID
  未知（随机占位），响应中带回的真实 ID 不得导致同一节点重复查询
- ✅ 超时终结（默认 30s，`set_lookup_timeout` 可配）：receiveLoop 每秒
  扫描，超时查找上报已收集的部分结果；回调一律在锁外执行
- ✅ 发送失败快速终结：`sendMessage` 返回 false（域名无法解析/socket
  无效/sendto 失败）→ `noteUnreachable` 将节点移出待响应集，查找立即
  收敛而不悬挂到超时。**修复真实缺陷**：构造时预置的公网域名引导节点
  （router.bittorrent.com 等）此前从不做 DNS 解析、`inet_pton` 失败后
  `sin_addr=0` 静默发往 0.0.0.0——公网 bootstrap 自始从未工作过且拖住
  查找永不收敛
- ✅ `clear_bootstrap_nodes()`：清空预置公网引导节点（纯私有网络/
  测试场景）；α=3/k=8/候选上限 64 常量化
- ✅ 协议正确性顺带修复：`nodeIdFromString` 原为 memcpy 截断而非 hex
  解码——`findPeers` 的 40 位 hex info_hash 与消息 decode 的 id 字段
  全部解析错误（encode/decode 不对称）；改为 hex 解码（合法 40 位 hex
  才解码，否则回退字节截断），与 `nodeIdToString` 构成往返；
  get_peers 的 `info_hash` 参数从 40 字符 hex 文本改为 20 字节原始值
  （BEP-005）
- ✅ `handleMessage` 响应分支重构：`pendingRequests_` 锁内取出并消费
  （一请求一响应），回调锁外调用——回调内部会再获取 mutex_，锁内
  调用即死锁；compact nodes/values 解析提取为共享辅助函数（原两处
  重复实现合一）
- ✅ 新增 `dht_node_test.cpp` 9 用例（本地 UDP mock DHT 网络）：
  DhtUtils 往返/XOR 距离序、两跳迭代逼近（新节点驱动下一轮）、
  find_node 距离序上报且收敛后无多余查询、空网络空结果、超时终结
  不悬挂、并发查找独立、无效端点立即终结、公网域名引导快速终结；
  mock 节点记录收到的查询并按编程构造器响应

**验证：**
- ✅ DHT 9 用例全绿；全量 ctest 1548/1548 通过（2 项 CLI NOT_BUILT
  为 build-ci 未编 CLI 的残留注册）

---

## 2026-09-13 测试覆盖率专项（68.1% → 74.0% 行覆盖）

**新增测试（+1365 行覆盖）：**
- PEX 协议 25 用例（`pex_protocol_test.cpp`）：消息编解码往返、
  Add/Drop 字节布局、握手/无握手分发、候选去重、回调触发、异常
  输入；顺带修复产品缺陷——Add 分支对已存在 peer 重复触发发现回调
  （仅新插入候选集才触发）
- bencode 边界 ~12 用例（`bencode_edge_test.cpp`）：深层嵌套、
  空容器、键序校验、非最小整数编码、截断输入、大整数
- BitTorrent 解析 ~10 用例（`bittorrent_parse_test.cpp` 补全）：
  can_handle/get_file_info 单多文件/错误路径；修好手写 bencode
  测试数据的长度前缀错误（`3:aa` → `2:aa`）
- S3 浏览器 27 用例（`s3_browser_mock_test.cpp`，本地 mock HTTP
  服务器）：URL 解析、连接（自定义 endpoint/不可达失败）、列表
  （JSON 解析/隐藏过滤/递归 CommonPrefixes/损坏响应/403）、HEAD
  信息（响应头解析）、exists 状态码语义、建目录 marker、删除
  （对象/递归降序）、复制改名重命名、配额解析；顺带修复 mock
  服务器挂死模式（Linux close() 不唤醒阻塞 accept 线程，须先
  shutdown(SHUT_RDWR) 再 close 再 join）

**S3 浏览器五项产品缺陷修复（`s3_browser.cpp`）：**
- endpoint 配置从未消费：build_s3_url 只会拼 AWS 官方域名，MinIO
  等 S3 兼容服务不可用；现 endpoint 优先生效（path-style
  `endpoint/bucket/key`）
- 对象 key 的 '/' 被 url_encode 编成 %2F，子目录 key 在真实服务
  必然 404；新增 encode_key 逐段编码保留 '/'
- HEAD 语义错误：CURLOPT_CUSTOMREQUEST("HEAD") 只改请求行方法
  字符串，响应仍按 GET 处理；真 HEAD 须 CURLOPT_NOBODY，且与
  CUSTOMREQUEST 互斥清设（handle 复用时残留覆盖 NOBODY 的方法
  切换——实测 curl 报 "Weird server reply"）
- get_resource_info 此前解析从不存在的 body（HEAD 无体，info
  恒空）——响应头经 HEADERFUNCTION 回传后填充
  name/path/size/modified_time/etag/mime_type
- 成功判定不能看 body 非空（S3 写操作成功常为 204 No Content）：
  perform_s3_request 状态码判定（200≤status<400）+ ok 出参，
  create_directory/remove/copy 改按请求结果返回

**覆盖率终值（gcovr，packages/ 范围排除 tests/）：**
- 行 74.0%（15730/21252）、函数 81.0%（1471/1816）、分支 39.4%
- 基线为 68.1%/77.0%/36.2%；+1365 行覆盖

**100% 不可达的诚实评估（剩余 top 缺口）：**
- http_commands.cpp 899 miss（57.6%）：平台分支（Winsock/POSIX
  双路径仅一路可在单平台执行）+ 真实网络交互
- ftp_browser 456：走真实 FTP 控制连接，需同类 endpoint 改造
- cos 356 / kodo 333 / oss 312 / upyun 293：与 S3 同构的五个
  browser，官方 virtual-host 域名 mock 不可行，需各自 endpoint
  改造 + mock 测试（本轮 S3 已立样板）
- http_handler 319、cli main.cpp 239（入口 main 不可测）、
  bittorrent_plugin 195
- logger.hpp 417 miss 为 gcc 15 行号漂移的测量伪影，非真实缺口

### 2026-09-14 覆盖率专项续（四云 browser endpoint 改造 + mock 测试）

**OSS/COS/Kodo/Upyun 四浏览器 endpoint 改造 + 缺陷修复：**
- endpoint（OSS/COS/Kodo）/api_domain（Upyun）携带 scheme 时优先
  生效 path-style `endpoint[/bucket[-app_id]]/key`，否则官方
  virtual-host；四个 browser 的 endpoint 配置此前全链路零消费
- query_string 从未拼到请求 URL（只进签名）——列举的 prefix/
  max-keys 从未真正发到服务端（OSS/COS）
- 递归列举边遍历边向同一 vector 插入（Kodo 目录合成、Upyun 子
  目录列举）——先收集/快照再插入
- Upyun 递归删除双斜杠：子目录列举 path 已带前导 '/' 再拼 "/"
  产生 "//"
- get_resource_info 恒真条件（对象不存在也报"存在"）——改按
  状态码 ok 化
- 全套 curl 修复与 S3 模板对齐：真 HEAD（NOBODY 与 CUSTOMREQUEST
  互斥清设）、POSTFIELDS 恒设（handle 复用残留）、HEADERFUNCTION
  响应头回传、状态码成功判定 + ok 出参；encode_key 逐段编码保留 '/'

**mock 测试首跑即曝光的三个额外真实缺陷：**
- COS/OSS 签名 URI 提取用 `find('/')+bucket.length()+N` 偏移算术
  ——host/port 长度不同即把 authority 片段混进规范资源（签名恒
  错），COS 端口个位数时 substr 越界抛 out_of_range；改为 scheme
  后定位 path
- COS 签名 canonical 头键小写化后 `all_headers.at(小写键)` 查原
  大小写 map——带大写键头（Content-Type）的请求必抛 map::at
  （建目录从未成功过）；改为插入时统一小写
- Kodo base64url（OpenSSL BIO）输出带尾部 '\n' 拼进 URL——curl
  报 "bad/illegal format"，stat 探测从未成功过；改
  BIO_FLAGS_BASE64_NO_NL

**新增 59 用例（oss/cos/kodo/upyun_browser_mock_test.cpp，共享
mock_http_server.hpp；新增防 RST 排空——POST 带请求体未读即
close 会以 RST 收场吞掉刚写出的响应）：**
- 全量 ctest 1701 用例通过，ASan 下五 browser 套件 81 用例零告警

### 2026-09-14 覆盖率专项第二轮（75.9% 行 / 87.5% 函数，进行中）

**基线与方法论：** gcovr 8.6 必加
`--merge-mode-functions=merge-use-line-min`（header-only inline 函数
多 TU 行号漂移 merge 崩溃）与
`--gcov-ignore-parse-errors=negative_hits.warn_once_per_file`
（gcc #68080）；gcovr 行数分母含无代码行严重虚高（s3 报 310 实为
36），**真实缺口以 gcov 文本 `#####` 标记为准**。基线 75.9% 行 /
87.5% 函数 / 41.1% 分支（1727 ctest 全绿）。

**FTP 浏览器收敛到 2 行（已完成，7de47a7 + e042766）：**
- storage 版 ftp_browser.cpp 六项协议缺陷修复（QUOTE 写操作、
  RNFR/RNTO 拆分、stale WRITEDATA UAF、get_resource_info 恒真、
  modified_time 解析、endpoint/ssl 消费）+ mock FTP 服务器 26 用例
- normalize_path 内部 "/./" 段不消除（path 带脏前缀）修复 +
  不可达 "./" 开头剥离死分支裁剪；测试 26 → 37
- 剩余 2 行：CURL init throw（OOM 防御）+ gcc static 初始化
  行归属伪影

**S3 浏览器 36 → 8 行（已完成）：** 排序/通配符/无凭据/endpoint
尾斜杠/key 特殊字符编码/rename copy 失败七用例；顺带修复 S3
modified_time 排序忽略 sort_desc（与 ftp 同构缺陷）。剩余 8 行
为官方域名 fallback 簇（真实网络）+ OOM throw。

**OSS/COS/Kodo/Upyun 四 browser 收敛（已完成，12→11/20/30→29/18）：**
四家并行补充 53 用例（排序/通配符/协议变体/options 消费/递归
删除/无效 URL/官方域名离线必败等）；五 browser 套件 103 用例
cov+asan 双绿。**顺带修复五项 fork 报告的产品缺陷**：① Kodo
`qn://` 协议断裂（can_handle/protocols 承诺接受、parse 拒绝，
`connect()` 直接把 invalid_argument 抛出公共 API，新增
PROTOCOL_QN）；② OSS sort_resources 完全忽略 modified_time 排序
键（补 sort_desc 双向分支）；③ COS get_cos_action 的 ListObjects
分支永不可达（uri 是纯 path，改判 query_string）；④ COS 签名
资源前缀误报（官方域名 + key 以 bucket 名开头即漏 /bucket 前缀
→ 真实 403；path-style 判定改为与 build_cos_url 分支同构）；
⑤ Upyun sort_resources 死代码（无调用点，客户端排序从未生效，
接线进 list_directory 作 x-list-order 被服务器无视时的兜底）。
剩余缺口均不可离线测：官方域名簇（真实网络）、CURL init/EVP/
HMAC/base64 长度防御（需注入）、`~Browser()` 的 gcov D0/D2 析构
变体伪影（行实际已被 D2 执行，`78*` 标记）。
**测试陷阱记录**：api_domain/endpoint 带 https:// 时绝不能指向
活着的明文 mock——TLS ClientHello 与等请求行双向互等到 curl 级
超时（单用例 300s+），改指无人监听端口（URL 构造分支照样执行）。

**覆盖率（批次 C 收口）：行 76.8% / 函数 89.8% / 分支 41.8%**
（基线 75.9/87.5/41.1），1790 ctest 全绿（净增 63 用例）。

### 2026-09-14 覆盖率批次 D（http_commands.cpp 真实缺口收敛 256 → 176 miss）

**两个真实缺陷修复（写测试先行 trace 时发现）：**
- **chunked 分片错帧**（READ_SIZE）：TCP 可把块大小行的 CRLF 拆开送
  达，旧代码把 CR 预消费进大小行缓冲——LF 与后续块数据被并进
  size_str，`stoul` 在 '\r' 处静默截断得错误块大小，数据错位后
  CR/LF 校验失败挂死下载。新增 `chunk_cr_pending_` 状态位：CR 在缓
  冲末尾时置位等待下批数据补判 LF，CR 绝不预消费；pending 后非 LF
  显式判帧错误
- **chunked trailer 跨缓冲 CR 丢失**（READ_TRAILER）：CR 恰在缓冲末
  尾时 `chunk_buffer_.clear()` 把它丢掉，终止 CRLF 被 TCP 分片拆开
  时永不可见，挂到 EOF 判截断。同一 pending 机制收口；trailer 侧宽
  松（CR 后非 LF 不消费该字节继续扫描——尾部不承载数据，与
  READ_SIZE 的严格判定有意不对称）
- **SIGPIPE 三层防护**（测试 SIGPIPE 暴露的生产缺陷）：引擎三处
  `send(...,0)` 均无 MSG_NOSIGNAL——对端 RST 后写 socket 即杀死整
  个进程（daemon 虽已 SIG_IGN 兜底，CLI 完全没有）。修复：引擎
  POSIX send 统一 `kSendFlags`（MSG_NOSIGNAL，Windows=0）、macOS
  socket 级 SO_NOSIGPIPE、CLI main 补 SIGPIPE SIG_IGN（与 daemon
  对称）
- 顺带消除重编显形的既有符号转换告警（base64 移位提前进无符号域、
  send/recv 长度参数 Windows/POSIX 分支化）

**测试（+30 用例，全量 ctest 全绿）：**
- proxy 套件扩展 4 用例：连接应答跨分片到达（200 先到 15 字节再补
  齐——would-block 重入路径）、base64 短凭据填充向量、`http:///path`
  与 IPv6 字面量 authority 判 Unsupported（parse 表驱动）
- 新文件 `http_commands_edges_test.cpp` 26 用例（编程式剧本服务器
  ScriptableServer：poll accept + 每连接一线程按剧本队列处理 + 可
  选 TLS 上下文 + 请求记录）：
  - 传输中断 4：TLS close_notify 干净关闭截断必 FAILED（总长未收
    满不构成完成证据）、TLS 半截头断连、明文体 RST 截断、accept 即
    RST（发送失败终态收口）
  - 段失败收口 2：段响应非 206（Range 撒谎）"response rejected"
    组 FAILED；TLS 多段段 1 握手前被裸关 → 段失败收口聚合终态，段
    0 体延迟到达唤醒后静默退出不覆盖
  - 大流量让出 1：5MB 单次 execute 4MB 读上限 NEED_RETRY 留队重执
    至完成（半分批让出不丢数据）
  - 发布失败 1：最终名被目录占用时 rename 失败按失败收尾，绝不假
    报 COMPLETED（错误消息"发布失败"，临时文件保留）
  - 重定向 5：query 剥除、相对 `..` 归一化到根、空 Location 失败、
    协议相对 `//host` 跟随、304+Location 不跟随失败
  - 续传调度 3：零断点全量响应继续、Content-Range 不可解析放弃续
    传转全新下载（控制文件删除+成品逐字节一致）、跨会话三段计划
    初始连接承载部分进度段（已完成段跳过不建连接，恰好 2 连接）
  - chunked 5：块大小行 CRLF 跨分片（回归）、trailer 终止序列跨分
    片（回归）、trailer 头块双 CRLF、数据后 CR/LF 错位各判帧错误
  - 解析容错 1：头区裸 LF 空行跳过后正常完成
  - TLS/代理 5：上述 TLS 截断/半截头/多段握手失败 + 代理 CONNECT
    前断连（recv==0）与 CONNECT 后 RST 两条失败收口
  - 不可解析域/仅 IPv6 主机 2：.invalid 快速失败、ip6-localhost
    （::1）AF_INET 数据面干净失败

**覆盖率（批次 D 收口）：行 77.3% / 函数 89.8% / 分支 42.1%**
（批次 C 同款全包口径 76.8/89.8/41.8；涨幅 0.5 点含四云批次贡献
——全包分母 22932 行下 80 行收敛即 0.35 点）。文件级铁账：
http_commands.cpp gcov 文本 miss 256 → **176**（行 87.95%/1461 行）
。剩余缺口定性：TLS 防御分支（SSL_CTX/OOM/证书解析失败）、
send/recv/CONNECT 硬错误路径、resume 理论不可达防御、Windows 平台
分支。ASan 引擎相关 194 用例零告警。

**后续批次（真实缺口）：** bittorrent_plugin 187 → ftp_plugin 135
→ http_handler 82 → task_manager 78。

### 2026-09-14 覆盖率批次 E（bittorrent_plugin.cpp 187 → 7 miss + 四缺陷修复）

**四个真实缺陷修复（gcov ##### 定位 + 写测试先行 trace 发现）：**
- **magnet infoHash off-by-one**（D1）：`"xt=urn:btih:"` 是 12 字
  符，旧代码 `pos + 11` 截取——magnet 任务的 infoHash 恒带前导冒
  号（can_handle 用 kXtPrefix.size() 正确而 download() 错误，两处
  不一致即证据）。提取/归一化收口为公开 static
  `extract_info_hash`/`info_hash_to_hex`：hex 输入校验后小写归一，
  32 位 Base32 解 20 字节转 hex，非法返回空串跳过查找
- **Base32 magnet 不解码**（D2）：can_handle 接受 32 位 base32，
  download() 却把 base32 文本原样传 findPeers——nodeIdFromString
  只认合法 40 位 hex 否则回退字节截断，必然查询错误 info_hash；
  base32Decode 辅助函数存在却从未被调用（死代码激活）。查表改大
  小写不敏感（RFC 4648）
- **parseBencode 宽松解析**（D3）：截断输入（"i42" 缺 'e'）静默
  返回假值、stoll 宽松接受空白/'+'、越界抛裸 out_of_range——
  get_file_info 纯模式 .torrent 路径真实使用这套内嵌解析器（与
  公共 BencodeValue 是两套独立实现）。三处严格化：容器/整数截断
  显式 throw、整数内容校验（可选负号+全数字）、stoll 包 try/catch
- **DHT 僵尸客户端**（D5）：DhtClient::start() bind 失败只记日志
  不抛异常（running_=false、socket_=-1），startDht 照常持有客户
  端——isDhtRunning() 撒谎、findPeers 的查找无人驱动、回调永不
  触发。startDht 检查 isRunning() 失败即 reset+错误日志；
  dht_node.hpp 补 isRunning() 访问器；新增 clearDhtBootstrapNodes()

**死代码删除：** bencodeToString/sha1/getTrackers/generateNodeId/
urlDecode 全库零引用（沿用 resume_if_exists/http_plugin_v2 先例）。

**测试（+32 用例，BT 过滤套件 86 → 118 全绿）：**
- magnet 提取 6（含 off-by-one 回归的精确匹配断点）+ 归一化 7
  （Base32 向量经 Python base64.b32encode 独立生成——批次 C 方法
  论：手写编码数据必须用独立工具验证；大小写不敏感/非法字符/31 位
  拒绝）
- parse 严格化 8（截断整数/容器、ie/i5e/i+5e、越界、整数字典键误
  解析；负整数不误伤对照）+ validateTorrent 3（非 dict 根/无
  pieces/无 length+files——钉死既有不抛契约，解析错误才 throw）
- DHT 生命周期 2（DhtClient(0) 随机端口绕开 6881 争用 + 清引导
  节点空网络立即收敛；HeldUdpPort 占口测冲突无僵尸客户端）+ 下载
  生命周期 6（hex/base32 magnet、pause/resume/cancel、无 hash
  跳过查找、PEX 查询）

**覆盖率（批次 E 收口）：行 78.3% / 函数 90.9% / 分支 42.6%**
（批次 D 77.3/89.8/42.1；净涨 1.0/1.1/0.5 点——180 行缺口收敛
+ 死函数删除分母缩小）。文件级铁账：bittorrent_plugin.cpp gcov
文本 miss 187 → **7**（行 95.72%）。剩余缺口定性：resume 路径
DHT 重启组合分支、findPeers 回调推进 peers（需真实 P2P 网络返回
peer，空网络不可达）、parseBencode 入口防御 throw（get_file_info
层已先挡空数据）。ASan BT 套件 118 用例零告警。

**测量教训（gcda checksum 覆盖陷阱）**：改源码后只重建测试二进制
时，daemon/CLI 等未重建二进制仍内嵌旧 checksum 对象，全量 ctest
中它们运行时对 gcda 执行 "overwriting with a different checksum"
= 整体替换而非合并——已测行为的覆盖数据被清掉，表现为全绿测试但
can_handle 等显示未覆盖。修复：全量重建 → 清全部 gcda → 重跑全量。

**后续批次（真实缺口）：** ftp_plugin 135 → http_handler 82 →
task_manager 78。

### 2026-09-14 覆盖率批次 F（ftp_plugin.cpp 135 → 3 miss：占位测试重写 + weak stub 链接陷阱）

**缺口定性（测量先行）：** 135/146 miss = 整个文件未执行——不是
局部缺口而是"零真实测试"。旧 `ftp_handler_test.cpp` 55 个用例全
是自说自话的占位测试（断言字符串字面量 `EXPECT_TRUE(true)` 式，
不触达产品代码）；唯一真实的 registry 测试长期 GTEST_SKIP。

**weak stub 链接陷阱（registry 0 注册的根因，nm 实证）：**
- core 的 `builtin_protocol_handlers_stub.cpp` 提供 weak 空实现，
  真实实现编译进独立对象 `falcon_builtin_protocol_handlers`（宏
  从 falcon_protocols 镜像，CMake 层闭合）
- GNU ld 归档**一次扫描**语义：`falcon_ftp_tests` 只用 core API、
  不引用任何 protocols 符号 → 处理 protocols 归档时无未定义引用
  → 真实实现对象**从未拉入**；core 拉入 protocol_registry.o 后其
  未定义引用由 core 内的 weak stub 解析（nm 显示 `W`）→ 空 stub
  生效 → load_builtin_handlers() 0 注册 → 测试跳过
- daemon/CLI 生产免疫：RPC 层引用 `describe_builtin_protocols`
  强符号把真实对象拉入（nm 显示 `T`，强胜弱）；但任何不碰该符号
  的新消费方都会静默 0 注册——脆弱机制，记录在案
- 测试侧收口：registry 测试显式引用
  `describe_builtin_protocols()`（本来就该断言注册结果），Skip 变
  真断言；重写后全部测试二进制 nm 验证强定义在场

**测试基建：** 新 `tests/unit/mock_ftp_server.hpp`（自 storage 包
mock 复制 + 下载语义扩展，storage 侧不动）：RETR 按 set_file_
content 应答、REST 续传偏移、一次性/永久命令失败（重试语义）、
分块慢发（暂停窗口）；控制协议命令逐条记录供断言。

**测试重写（55 占位 → 20 真实用例，cov + ASan 双绿）：**
- SIZE 探测：成功（213 → total_size）/ 未知文件 throw——curl 对
  SIZE 550 判 "Remote file not found"，**RETR 之前即弃**（命令序
  列 dump 实证，第一版测试预期因此翻正）
- 下载端到端：RETR 落盘逐字节一致 + rename 发布 + tmp 消失 +
  Completed；curl 选项黑盒（verify_ssl=false/timeout/限速）
- 断点续传：预置 .falcon.tmp → REST 偏移断言 + app 拼接成品
- 重试：瞬态 RETR 失败后恢复（恰 2 次 RETR + retry_delay=1s 指数
  退避时长下界断言）；重试耗尽 throw；SIZE 探测失败两轮探测零
  RETR 快速失败
- 失败收口：输出目录打不开 → FileIOException（零 RETR，先于网络
  阶段）；成品路径被目录占用 → rename 失败 throw、状态保持
  Pending 绝不假报完成、temp 保留
- 慢发进度记账：越过 progress_callback 200ms 节流窗，
  downloaded/total 真实更新；暂停中止（回调返回 1 →
  CURLE_ABORTED_BY_CALLBACK → 静默返回，tmp 保留为断点）；
  cancel 先行跳过；resume 重启；proxy + 凭据生效性（无人监听
  代理必败 + 流量从未直连）

**覆盖率（批次 F 收口）：ftp_plugin.cpp gcov miss 135 → 3（行
97.26%）**，剩余为 curl_easy_init OOM throw×2 + write_callback
`!is_open()` 防御（回调时 file 必开着）——全部不可测项。全包
（批次 C 同款 gcovr 口径）：**行 78.9% / 函数 91.5% / 分支 42.9%**
（批次 E 78.3/90.9/42.6；净涨 0.6/0.6/0.3 点）。

**后续批次（真实缺口）：** http_handler 82 → task_manager 78。

### 2026-09-14 覆盖率批次 G（http_handler.cpp 80 → 16 miss：V1 curl 数据面回环测试 + 空指针缺陷修复）

**缺口定性（80 miss 分簇）：** 响应头解析（Content-Disposition 引
号/无引号、filename 从 URL 推导）、进度中止/记账/限速热应用、curl
选项簇（proxy/SSL/referer/cookie/http auth/自定义头）、单连接续传
与 Range 防护、HTTP 错误重试语义、rename/打开失败、分段限速均摊
与生命周期。V2 分叉默认关（v2_http_enabled()=false），全部测试走
V1 curl 路径。

**缺陷修复（测试曝光，与 FtpHandler 对齐）：**
- `HttpHandler::pause/resume/cancel` 缺空指针防御——`pause(nullptr)`
  直接解引用崩溃（FTP 插件同位置有 `if (!task) return;`，跨插件
  不一致）；在外层三入口统一补防御（impl 与 V2 转发共用）

**测试基建：** 新 `tests/unit/http_handler_edges_test.cpp` 自包含
可编程 HTTP 服务器：按路径应答/剧本（末位无限重复，**HEAD 探测
恒用末位放行**——download() 顶部 get_file_info 先行，否则 500
剧本直接炸掉）、Range 自动 206 + Content-Range 切片、按 Range 起
始差异化慢发、一次性部分发送后硬断连（段短传）、Range 撒谎
（HEAD 宣称 Accept-Ranges / GET 一律 200）、无 Content-Length
（Connection: close EOF 定界）、起零死端口（连接拒绝/必败代理）。

**26 用例（cov + ASan 双绿）：**
- get_file_info：CD 引号/无引号 filename、URL 推导（query 剥除/
  根路径默认 "download"）、连接拒绝 throw
- 选项传播（服务器侧观测）：UA/Referer/自定义头原样到达、
  COOKIEFILE 激活 cookie 引擎（真实 cookie 头回传 + COOKIEJAR
  会话写出）、401 挑战后 Basic 凭据重放（CURLAUTH_ANY 不预发）、
  无人监听代理必败且流量从未直连
- 单连接：慢发进度记账（越 200ms 节流窗）、限速热应用（编程
  listener 首窗 0 次窗 32KB/s，want != applied 分支）、REST 续传
  （Range: bytes=2- 断言 + app 拼接）、Range 撒谎服务器绝不产出
  损坏成品（现代 curl resume 守卫 CURLE_RANGE_ERROR 先拒——
  handler 内 200-check 分支是老 curl 纵深防御，本 curl 不可达）、
  500×2 重试剧本（恰 3 次 GET）与 404 立即抛（恰 1 次 GET）、
  rename 失败（成品路径被目录占用 → FileIOException + 状态
  Pending + temp 保留）、输出打不开零网络、重试退避窗口内 Paused
  静默退出、EOF 定界未知总长下载（total 记 0、进度照常记账）
- 生命周期：暂停中止（进度回调返回 1 → CURLE_ABORTED_BY_
  CALLBACK，tmp 保留断点）、resume 重入（**恢复前置位
  Downloading 是 TaskManager 职责**——handler resume 只重跑
  download()，Paused 守卫直接返回）、cancel 先行跳过 + 三入口
  空任务防御
- 分段：64KB 4 段端到端逐字节一致（listener 限速按连接均摊）、
  段 HTTP 错误失败收口（非零段一律 500 → FileIOException + 成品
  不发布）、传输中 pause/cancel 经 active_segmented_downloads_
  转发 downloader->cancel（**分段路径的段只看 cancelled 标志不查
  task 状态**，仅 set_status 无法中止）、暂停后 resume 重入（
  **SegmentDownloader 析构即清段文件**——handler 层暂停不保留段
  断点，重新全量下载仍逐字节一致）、段短传重试从部分数据续传
  （4096 字节断连 → 精确尺寸校验失败 → worker 按段文件已有尺寸
  算续传起点 → app 模式补齐，无洞无重叠）、Range 撒谎服务器分段
  必须失败干净（V1 段完整性闭环回环钉）

**覆盖率（批次 G 收口）：http_handler.cpp gcov miss 80 → 16（行
96.2%）**，剩余定性：143 单连接 cancelled 标志防御（外部接口未
暴露恒 false）、287/401/537 curl_easy_init OOM、310-311 段文件
打开失败（输出目录创建先行）、359-360 段续传截回（worker 超尺寸
best-effort 先删，结构性不可达）、611-617 200-instead-of-206 纵
深防御（本 curl 守卫先拒）、619 else 行归属伪影、634 重试间隙
Paused 检查（500 返回与状态置位间毫秒级竞态窗口）。全包（批次 C
同款 gcovr 口径）：**行 79.2% / 函数 91.5% / 分支 43.1%**（批次 F
78.9/91.5/42.9；净涨 0.3/0.0/0.2 点）。

**后续批次（真实缺口）：** task_manager 78。

### 2026-09-14 覆盖率批次 H（task_manager.cpp 75 → 13 miss：状态持久化容错 + 事件转发层测试）

**缺口定性（75 miss 分簇）：** ① 序列化/解析辅助函数全部容错分支
（read_int/read_download_options/read_task_data 的逐字段截断
return false、越界优先级与活动状态净化）；② add_task/start_task
防御（非法 id/重复 id/终结任务/重复入队）；③ auto-save 异步保存
链（save_state_async CAS + state_pool，add_task/remove_task/
cleanup_loop 三触发点全 miss）；④ set_state_file、on_completed、
公共 API 转发层（set_state_file/on_task_status_changed/
on_task_progress/双参 start_task）；⑤ 轮询循环 cleanup/auto_save
与 worker 出队过期条目。

**测试基建：** 新 `tests/unit/task_manager_edges_test.cpp`（挂
falcon_core_tests，不新增 target）。损坏状态文件以 save_state
version-2 行格式的镜像手工构造——options 字段序列拆成 22 个字段
组，按前缀截断生成损坏行变体，每行独立解析失败互不影响，一批行
（18 行）即可精确对准 read_download_options/read_task_data 的每
个解析失败分支。确定性技巧：worker 出队过期条目 = start_task 入
队后**绕过 TaskManager 直接 set_status 终结任务**（cancel_task 会
清入队版本走另一分支）；stop 取消活动下载 = 自定义 handler 在
download() 内置 Downloading 后挂起等取消；无 handler 任务 =
worker 下载线程内 throw → catch 收口 Failed。

**21 用例（cov + ASan 双绿）：** add_task 非法/重复 id 拒绝；
start_task 拒绝簇（不存在/终结/已入队，单参与双参重载分别命中
357/298 两条 return）；无 handler 任务启动即 Failed；stop 先取消
活动下载再拆线程（handler 侧观测取消）；worker 出队过期条目静默
丢弃；save_state 不可打开路径失败；空文件/坏版本 load 拒绝；截
断行跳过（基础数据簇 + options 18 变体簇）；非法 id/空 URL/重复
id 行跳过；越界优先级回落 Normal + Downloading/Preparing 净化为
Paused（恢复不自动启动）；options 全字段 save/load 往返（含引
号/反斜杠/空格转义、非空 error_message、进度与优先级）；auto-save
三触发点（add/remove/cleanup 周期，stop 排空后断言文件存在 +
重载校验）；set_state_file 动态启用；state_file 为空静默跳过；
on_task_status_changed/on_task_progress 经 EventDispatcher 派发
（RecordingListener 轮询断言 + 活动计数进出）。

**覆盖率（批次 H 收口）：task_manager.cpp gcov miss 75 → 13（行
97.31%）**，剩余定性：76-77 与 553/557-560/563-564 共 9 行为
gcc 15 行归属伪影（**铁证：同一顺序执行块内 561 proxy_password
覆盖而执行序更早的 560 proxy_username 不覆盖、78 行覆盖而更早的
76/77 不覆盖——直线代码部分行覆盖部分不覆盖在执行模型上不可能**
；round-trip 用例全字段断言通过即证明这些行在执行）；751-756
Impl::on_completed 4 行不可达——**全库零调用方**（唯一命中是
IEventListener 接口默认空实现；完成事件实际派发走
on_status_changed 内的 Completed 分支），接口完整性 override
保留不删。附带修正确认：TaskManager 不继承 IEventListener，
on_task_status_changed/on_task_progress 是给引擎适配层的显式注
入口。全包（批次 C 同款 gcovr 口径）：**行 79.6% / 函数 92.1% /
分支 43.2%**（批次 G 79.2/91.5/43.1；净涨 0.4/0.6/0.1 点）。
全量 ctest 1864（2 例 DownloadEngineTest 并行抖动串行复跑即
过，非回归——本次零生产代码改动）。

**后续批次（真实缺口）：** 下一批按 gcov 全包扫描重新定位（四批
已收敛 protocols 包三个大头与 core 最大头）。

### 批次 I 收口（2026-09-14）：websocket_rpc_client.cpp 99 → 9 miss

全包 gcov `#####` 真实计数排名（gcovr CSV 初筛虚高 2.5-2.7 倍不
可作依据）：websocket_rpc_client 99 / dht_node 94 / config_manager
82 / task_storage 81 / cloud_storage_plugin 78 / segment_downloader
77 / incremental_download 46 / kodo_browser 29 / cos_browser 20 /
upyun_browser 18 / oss_browser 11 / s3_browser 8 / ftp_browser 2。

**测试基建：** `websocket_rpc_client_test.cpp` 增量 18 用例（挂
falcon_daemon_rpc_client_tests，29 → 47），新增可编程原始 WS 服务
器 `RawWsServer`——握手剧本（Ok/半截头/非 101/错 Accept）+
`on_connected` 会话钩子注入服务器帧（ping/close/binary）+
`on_request` 请求应答脚本 + 客户端帧记录（opcode/payload 快照与
等待辅助）；随机端口 htonl(INADDR_ANY)+getsockname，停机
shutdown(listen) 唤醒 accept、shutdown 会话 fd 唤醒读线程。与
JsonRpcServer 回环互补：帧级行为完全由测试控制，覆盖客户端单侧
防御路径。

**六簇收口：** ① 便捷方法簇 547-638 整段零覆盖 →
ConvenienceMethodsFullSuite 批量一次收口：14 个转发方法逐一调用，
服务器侧断言每个 method 的 params 归一形状（addUri 的
[[uris],{options}]、changePriority 的 [gid,2]、tell* 的 [0,10000]、
无 secret 时无 token 前缀等 15 项），响应解包
as_gid/expect_ok 往返断言；外加两个防御变体（addUri result 非字
符串 → -32600、expect_ok 的非字符串/"NG" → -32600）。② call 失
败路径：466 非数组 params 归一（服务器回显 params==[] 证明）、
507-514 应答超时（timeout=1s，断言 ≥900ms 与错误消息精确）、530
无 result 无 error → -32600、435/437-442 fail_pending（call 挂
起中服务器 shutdown 会话 → 读线程收尾 → -32000 "connection
closed" 唤醒挂起调用）。③ 控制帧 351-360：服务器 ping → 客户端
回 pong（payload 原样断言）、close → 客户端回应 close 帧且
connected_ 复位、binary/pong 帧忽略后连接仍可用（且客户端不回
pong 不转发 binary）。④ 握手失败簇：252-253 半截头 EOF、263-264
非 101 状态行、285-286 错 Sec-WebSocket-Accept、112 getaddrinfo
失败（.invalid 保留域，connect 干净失败不悬挂）。⑤ set_url/
parse_url：165-169 运行期重定向（强断言：旧服务器下线后 call 仍
成功 ⇒ 必然连到了新端点）、191-195 IPv6 字面量（[::1]:1 连接
必败——服务器只听 127.0.0.1，换回后恢复）、186+204 裸主机默认
path/端口、自定义 path 原样进请求行（服务器侧断言）。⑥ ASan 树
首次纳入 daemon：build-asan `FALCON_BUILD_DAEMON` OFF→ON，
falcon_daemon_rpc_client_tests 47 用例 ASan+UBSan 零告警（WS 客
户端多线程 socket 代码首次内存检查）。

**剩余 9 miss 全部定性：** 238-239（握手请求 send 失败——TCP 连
接刚建立后首个 send 的失败窗口无注入点，服务器无法在客户端
connect 返回前精确 RST）；494-499（call 发送失败收口——需 fd 失
效而读线程尚未收尾的竞态窗口：读线程对任何连接死亡都立即 recv
出错收尾并置 fd_=-1，send 路径无可控注入时机）；520（response
非 object **不可达**——slot->response 两条赋值路径
dispatch_message 387 行 is_object 检查后 / fail_pending 431 行字
面量构造都保证 object，519 行检查恒真）。

**覆盖率（批次 I 收口，批次 C 同款 gcovr 口径）：行 79.9% / 函数
92.9% / 分支 43.6%**（批次 H 79.6/92.1/43.2；净涨 0.3/0.8/0.4
点）。全量 ctest 1884 零失败。

**批次 J 候选（##### 铁账）：** dht_node 94 / config_manager 82 /
task_storage 81 / cloud_storage_plugin 78 / segment_downloader 77
/ resource_search 47 / incremental_download 46。

### 批次 J 收口（2026-09-14）：dht_node.cpp 94 → 10 miss

**测试基建：** `dht_node_test.cpp` 增量 16 用例（挂
falcon_protocols_tests，9 → 25），复用既有本地 UDP mock DHT 网络
（MockDhtNode）。纯单元簇直接构造公开类型（DhtBucket K=8 /
DhtRoutingTable / DhtMessage / DhtUtils），DhtClient 簇沿用 mock
端到端。两个测试设计要点：① **bootstrap 候选 id 全零，距离排序退
化**——需要确定距离序的用例（α 上限验证）必须先跑一次热身查找，
让响应把带真实 id 的节点写入路由表，第二阶段查找的候选才来自路
由表的确定性排序（直接 getRoutingTable 注入不可行，返回 const）；
② 冻结迭代轮次用「响应门闩」：被查询节点的 responder 自旋等待
atomic 门闩，主线程观察到第 1 轮恰好 α=3 个查询后放行，响应驱
动第 2 轮查询第 4 近的候选——时序完全确定，无 sleep 依赖。

**六簇收口：** ① 纯单元：nodeIdFromString 大写 hex / 40 长度含非
hex 回退 / 非 40 长度短补零超长截断（140-162）、DhtNode::
distanceTo（193-194）、DhtBucket 全 API（桶满替换 15 分钟不活跃
最旧节点——lastSeen 直接回拨 16 分钟构造、桶满全员活跃拒绝进替
换缓存、removeNode / getNodes / getActiveNodeCount 含 inactive 不
计入、findClosestNodes 排序截断）、DhtRoutingTable 跨桶聚合
（getAllNodes / getTotalNodeCount / findClosestNodes / 全零 id 钳
位桶 159 不越界）、DhtMessage Error encode/decode 往返（含空错误
表回落 "Unknown error"）+ 非 dict 与垃圾输入返回默认消息不抛。
② 未 start 客户端：socket_==-1 → sendMessage 快速失败 → noteU
nreachable 链立即终结，findPeers/findNode 均不悬挂（700）。③
α=3 并发上限（783）：热身 + 门闩两阶段断言第 1 轮恰查最近 3 个、
第 4 近在放行后才被查。④ kMaxCandidates=64 吸收截断（896）：响应
携带 70 个新节点强制命中 break，剩余候选不可达经 1s 超时干净终
结。⑤ k=8 上报截断（965）：9 个响应者恰报最近 8 个、最远者不报
且无重复。⑥ 重复 id 去重（968）：两端点不同声称同一 id 的响应者
各查一次、只上报一次（idA + sharedId 共 2 个回调）。

**剩余 10 miss 全部定性（与批次 J 预定位清单逐一对应）：**
495-496（socket() 构造失败，需 OOM 注入）；542-543（维护线程 5
分钟周期，测试不可等待）；637-638（recvfrom 非 EAGAIN 错误，本
地 UDP 竞态/平台窗口）；771 / 850 / 876 / 938（四个并发防御行：
finalizeLookup 与 pendingRequests_ 在同一把锁内同步清理——迟到
响应/迟到的发送失败/双重终结仅在「超时线程 finalize 与回调派发
之间」的微窗口可达，非确定性注入点不存在；如 876 的迟到响应若
finalize 已发生则 pendingRequests_ 已清空，handleLookupResponse
根本不会被调用）。

**覆盖率（批次 J 收口，批次 C 同款 gcovr 口径）：行 80.3% / 函数
93.4% / 分支 43.8%**（批次 I 79.9/92.9/43.6；净涨 0.4/0.5/0.2
点）。全量 ctest 1900 零失败；DHT 25 用例 ASan+UBSan 零告警。
**口径警示（批次 J 实测）：** gcovr 显式位置参数 `.` 只扫 cov 树
（曾得 90.5% 虚高口径），留档命令无位置参数时搜索目录默认 root
（`..`）= 仓库根，同时吃进 build-cov 与 build-asan 两棵树——铁
账链为两树合并口径，跨批次对比必须用原命令。

**批次 K 候选（##### 铁账）：** config_manager 82 / task_storage
81 / cloud_storage_plugin 78 / segment_downloader 77 / resource_
search 47 / incremental_download 46。

### 批次 K 收口（2026-09-14）：config_manager.cpp 82 → 32 miss

**测试基建：** `config_manager_test.cpp` 增量 8 用例（挂
falcon_drives_tests，18 → 26；`FALCON_ENABLE_CONFIG_MANAGER` 未
开启时仍 SKIP——新增用例全部在既有 `#else` 分支内）。测试 target
经 falcon_drives 的 PUBLIC 链接可直接用 sqlite3/OpenSSL。两个新
构造手段：① `exec_sql` sqlite3 直连写库（绕过 ConfigManager 构
造防御/篡改数据场景）；② `mini_gcm_encrypt`（OpenSSL EVP
aes-256-gcm，IV12+ct+tag16 布局、key=SHA256(password)，与生产
AES256GCM::decrypt 对齐）+ "FALCONCFG1" magic 前缀构造任意语义
的导出 payload。

**八簇收口：** ① 未初始化 manager（db_=nullptr）全部操作拒绝——
verify/set_master_password false，save/get/delete/list/search/
update/export/import 全 false/空，一次性覆盖认证门的 312 提前
返回路径（401/466/489/514/592/667/713 七个门行同源）。注意不可
对未初始化 manager 调 set_master_password（276 先设成员再走
internal → sqlite3_prepare_v2(nullptr)）。② verify 全链：正确
true/错误 false/错误后正确恢复。③ master 表行被删（exec_sql 直
连）：verify false。④ set_master_password：弱密码拒、换密后旧
密码失效新密码认证。⑤ update 空 provider 拒绝（595）。⑥ 导出
导入边界：export/import 空密码（662/710）、import 不存在文件
（718）、短文件 <10 字节（721）、错 magic（724）。⑦ 篡改 payload
语义（mini_gcm 构造）：JSON 无 configs 键/非 array（741）、数组
条目缺 name 跳过且其余照常导入——部分导入语义验证（764）。⑧ 库
内密文截短（exec_sql UPDATE blob）：get 仍成功且解密失败字段空
串、非敏感字段完好（128 decrypt 短密文返空）。公共转发行
806-807/810-811 经 ② 覆盖。

**两个真实语义发现（记录不修，均为未接线 API 的设计缺口，修复
属特性开发）：** ① `set_master_password` 换密只更新 master 表
PBKDF2 哈希，不重加密已存配置——旧密文以旧密码密钥加密，换密后
access_key/secret_key 解密失败恒空串（数据废）。生产零调用方
（grep 实证），与 core PasswordManager 同语义。② verify 的
missing-row 路径（321-323）提前返回不触碰 authenticated_——
已认证 manager 删行后写入仍放行（认证门仅在 authenticated_ 已
失效时才复核）。两者均已在测试注释与断言中记录真实行为。

**ASan：** 新用例零新增告警。曝出 resource_search.cpp 既有泄漏
（WebCrawler::set_headers 的 headers_ slist 76 字节/4 处，经
GenericSearchProvider 构造 ← load_config；排除 ConfigManager
用例后泄漏依旧实证与本批无关）——留给 resource_search 批次。

**剩余 32 miss 全部定性：** EVP init/update/final 失败防御×12
（89-90/99-100/106-107/149-150/155-156/165-166，需 crypto 注
入）；sqlite prepare/exec/step 失败防御×15（318/376/412/472/
480/495/531/602/633/653 各方法 prepare+step、217-219 PRAGMA、
255-257 create table）；265/267 initialize 空密码路径不可达
（is_reasonable_master_password 拒空密码 → 200 入口已拒）；695
export encrypt 空返回（仅 OOM/内部错误）；768 import 中 save
失败不可达（有效数据+已认证+合法 SQL 下恒成功）。

**覆盖率（批次 K 收口，批次 C 同款 gcovr 口径）：行 80.5% / 函
数 93.7% / 分支 44.0%**（批次 J 80.3/93.4/43.8；净涨 0.2/0.3/
0.2 点）。全量 ctest 1908 零失败；drives 136 用例 cov 树全绿 +
ASan 排除既存泄漏后零新增告警。

**批次 L 候选（##### 铁账）：** task_storage 81 / cloud_storage_
plugin 78 / segment_downloader 76 / resource_search 47（顺带修
WebCrawler 泄漏）/ incremental_download 46。

### 批次 L 收口（2026-09-14）：task_storage.cpp 81 → 39 miss + initialize 死锁缺陷修复
- **修复 TaskStorage::initialize 死锁缺陷**（测试曝光，strace 栈
  铁证）：initialize() 入口持 `std::mutex`（不可重入），建表失
  败分支调 close()，close() 内部再次 lock 同一把锁 → 死锁。生产
  影响：**task db 损坏（非 SQLite 文件）时 daemon 启动永久挂死**
  而非优雅报错。修复：close() 去掉内部加锁（private 辅助，仅析
  构与已持锁的 initialize 流程两个调用点）。strace 证据链：sqlite
  判定 NOTADB 并解锁文件 → 进入 close → futex 死等
- task_storage.cpp gcov miss **81 → 39**：8 新用例挂
  `falcon_daemon_storage_tests`（24 → 32）——open 失败（不存在
  父目录）、坏库文件建表失败（256 字节垃圾 + 死锁修复后干净返回
  false）、completed_at 有值 create/update 往返、显式 id 重复插
  入 step 失败（UNIQUE 冲突）、list limit+offset 分页（created_
  at 显式错开保证 ORDER BY 次序确定——同毫秒并列时 DESC 次序未
  定义曾致断言抖动）、cleanup_completed_tasks 全语义（过期删除/
  新完成与 Pending 留存/幂等/未初始化实例 0）、move 构造与 move
  赋值（连接接管 + CRUD 复验）、get_last_error 错误面
- 剩余 39 miss 定性：sqlite prepare/step 失败防御×36（各方法成
  对 return 行，需 SQL 注入/句柄失败；cleanup 内两对同行）、gcc
  15 行归属伪影×3（664/666/670 parse_task_record 时间点构造多行
  表达式首行——同块尾行覆盖 + GetTask 字段断言通过双证在执行）
- **覆盖率（批次 L 收口，批次 C 同款 gcovr 口径）：行 80.7% /
  函数 94.0% / 分支 44.0%**（批次 K 80.5/93.7/44.0）；全量 ctest
  1916 零失败；storage 42 用例 cov + ASan 双绿（UBSan 零告警）
- 测试执行环境注意：storage 用例是死锁敏感户——批次 L 期间挂在
  futex 的进程用 strace 定位（沙盒无 gdb）；gcovr 在 cwd 漂离
  build-cov 根时报 TOTAL 0 行，回根重跑即可

**批次 M 候选（##### 铁账）：** cloud_storage_plugin 78 /
segment_downloader 76 / resource_search 47（顺带修 WebCrawler
headers_ slist 泄漏）/ incremental_download 46。

### 批次 M 收口（2026-09-14）：cloud_storage_plugin.cpp 78 → 75 miss + WebCrawler slist 泄漏修复
- **修复 WebCrawler 析构泄漏**（批次 L 预记录的既存缺陷，ASan
  实证 76 字节/4 处）：set_headers 保存 `curl_slist* headers_`
  且重复调用时释放旧链，但 `~WebCrawler()` 只 `curl_easy_
  cleanup` 从不释放 slist——每个 GenericSearchProvider 构造
  （load_config 每引擎一个）泄漏整条请求头链。修复：析构补
  `curl_slist_free_all(headers_)`（空指针安全）。resource_
  search.cpp:224 ← GenericSearchProvider 构造 ← load_config:698
  栈与本批 ASan 复验零告警
- cloud_storage_plugin.cpp gcov miss **78 → 75**：3 新用例挂
  `falcon_drives_tests`（139 用例）——①蓝奏云无效链接（detect
  `[\w]+` 接受下划线开头而 extract `[a-zA-Z0-9]+` 不接受，
  "https://www.lanzoux.com/_-"：识别到平台但拿不到文件 id，不
  发网络请求直接短路"无效的蓝奏云链接"）；②Google Drive docs
  形态无 file id（docs.google.com detect 必命中 `[^\s]+`，无
  /file/d/ 与 ?id= 时 extract 空 → 轻量基类无效链接分支 +
  platform_display_name）；③未知平台三循环路由全落空（"未找
  到对应的网盘插件"）
- 剩余 75 miss 全部定性：
  - **44-114（12 行）gcc 15 行归属伪影**：init_patterns 的 map
    initializer_list 赋值首行。铁证（执行序矛盾）：41 行
    `if (!url_patterns_.empty()) return` 命中 14045 次 ⇒ map
    非空 ⇒ 12 个赋值必然执行过；且 DetectCloudPlatform 等 12
    平台 detect 断言全通过（map 空则全 Unknown 必失败）
  - **435-450/545-574/648-671/741-764/813-832（55 行）死接口
    存根**：五类（Lanzou/Baidu/Aliyun/Quark/LightweightBase）
    的 get_download_url/authenticate/get_user_info/get_quota_
    info 四件套。grep 全库实证：CloudStorageManager 生产代码
    零调用方（纯库 API，仅测试消费）、register_plugin 生产零
    调用、get_download_url 包外零调用——manager 不暴露插件指
    针，四件套对内部类不可达，属 ICloudStoragePlugin 纯虚的占
    位实现
  - **879/894/909/954/969/984（6 行）display_name 死代码**：
    唯一调用点是轻量基类 file_id 空分支（784），而腾讯微云/
    115/PikPak/OneDrive/Dropbox/Yandex 的 detect 与 extract 正
    则字符类对齐（detect 命中 ⇒ extract 必命中），784 对它们
    结构性不可达（Mega/GoogleDrive 两平台可构造且已覆盖）
  - **1063-1064（2 行）死防御分支**：第二循环的
    `platform_type()==detected → continue`。detected!=Unknown
    时 platform_type==detected 的默认插件 can_handle ≡
    （detect==自身平台）≡ true，第一循环必命中——第二循环对
    已识别平台永不可达
- **覆盖率（批次 M 收口，批次 C 同款 gcovr 口径）：行 80.8% /
  函数 94.1% / 分支 44.2%**（批次 L 80.7/94.0/44.0）；ctest 清
  单 1919（1859 通过 + 60 既存 Skipped，零失败；out-of 分母
  1918 为 CTest 对 Skipped 的显示口径）；drives 139 用例 cov +
  ASan 双绿（零新增告警，既存泄漏清零）
- ctest 计数注意：gtest_discover_tests 清单（-N 1919）与运行
  分母（1918）差 1 来自 CTest Skipped 口径，账目闭合式
  = passed + skipped + failed；`Test  #917`（<1000 双空格对
  齐）与 `Test #1000`（单空格）会骗过单行格式 grep

**批次 N 候选（##### 铁账）：** segment_downloader 76 /
resource_search 47（泄漏已修，剩缺口待收）/ incremental_
download 46。

### 批次 N 收口（2026-09-14）：segment_downloader.cpp 76 → 11 miss + 死代码清理
- **删除 22 miss 行匿名命名空间死函数**（批次 E 死代码清理先
  例）：`generate_random_suffix`（自带 `[[maybe_unused]]`，零调
  用）与 `format_bytes`（唯一"引用"在 monitor_connections 的注
  释行里），连带 `<cmath>/<iomanip>/<random>/<sstream>` 四个仅
  其使用的 include
- segment_downloader.cpp gcov miss **76 → 11**：11 新用例挂
  `falcon_protocols_tests`（既有 27 用例 + 新 11）——①修复
  ZeroFileSize 占位测试（从未调 start() → 现断言 0 尺寸快速失
  败且无半成品）；②start 门禁两态（运行中重入拒绝：栅栏 mock
  保持运行态 + is_active 轮询；cancel 后启动拒绝）；③等分策略
  （adaptive_sizing=false + 非整除 10243 → 末段带走余数，全链
  从未跑过）；④续传完成态两态（恰好整段的既有段文件被标记完
  成后正常补齐；全部段已完成 → download_func 零调用直接合并 +
  成品按序逐字节校验）；⑤暂停三循环（worker 主循环 50ms 节拍
  + 监控线程 1s 节拍暂停分支（持续暂停 >1.15s）+ 段内重试循环
  100ms 节拍（失败后置暂停 300ms 再 resume，断点续上）），
  顺带断言暂停态 is_active=false 与只读查询可用；⑥返回 false
  但段文件恰好整段 → best-effort 记账识别完成态不浪费重试；
  ⑦merge 前逐段闸门（等 seg0 完整落盘 5120 后 trunc 篡改为
  100 字节——已完成段无重试自愈窗口，确定性触发拒绝）；
  ⑧输出路径是已存在目录 → rename(文件→目录) 失败干净收尾；
  ⑨传输中 cancel（栅栏 mock 响应取消立即退出，cancel 收割
  worker 与存活监控线程，start 收尾二次 join 跳过已收割线程）
- **MergeGate 用例的时序教训**：初版 `num_connections=1` 实际
  只产生 1 段（calculate_optimal_segments 返回 num_connections
  本身），篡改分支从未执行（假绿）；改 2 连接后并发交错下篡改
  可能落在写入中途 → 重试循环会把段自愈成精确尺寸反让闸门放
  行（且 retry_delay_ms 默认 1000 使用例耗时 7s）。确定性构造：
  等目标段完整落盘（文件尺寸==段长即已完成态）再篡改，已完成
  段绝无重试窗口
- 其余观察：start() 收尾 join 监控线程使其最后一秒 tick 跑满
  （多数用例 +1s 墙钟，既有行为非本批引入）；单跑失败先查段数
  是否等于 num_connections 再怀疑并发
- 剩余 11 miss 全部定性：184 死防御（calculate_optimal_segments
  恒 ≥1 → segments 不可能为空）；301-305 五行 30s worker 超时
  兜底（墙钟不可测）；345 防御（worker 无失败/取消退出时每段必
  已完成）；416-417 防御（resume 预标记使段入口完成检查不可
  达）；521 防御（merge 临时文件与段文件同目录，段已成功写）；
  545 权限依赖（ifstream 打开失败需 chmod 000，Windows 不兼容）
- **覆盖率（批次 N 收口，批次 C 同款 gcovr 口径）：行 81.0% /
  函数 94.3% / 分支 44.3%**（批次 M 80.8/94.1/44.2）；全量
  ctest 零失败（DownloadEngineTest.ResumeTask 并行抖动串行复跑
  即过，批次 H 同款既知条目）；新增 11 用例 cov + ASan 双绿
- **85% 路线全包扫描（本轮完成）**：18664/23103 → 85% 需
  ~974 行新覆盖；缺口分布（##### 铁账）：http_commands 146 /
  json_rpc_server 68 / resource_browser 系 153 / daemon config
  54 / incremental_download+file_hash 69（60 个 GTEST_SKIP 待
  查明）/ request_group 系 106 / resource_search 47 / 纯逻辑
  散矿 ~90；cloud_storage_plugin 剩余 75 行中 55 行四件套存根
  需 manager 只读插件访问器（方案 A）或 include-cpp 才可达

**批次 O 候选（##### 铁账）：** daemon/config.cpp 54（纯 JSON
解析最易）/ json_rpc_server 68（回环基建现成）/ resource_search
47 / incremental_download 46 + file_hash 23（先查明 GTEST_SKIP）。



### 批次 O 收口（2026-09-14）：incremental_download.cpp 46 → 11 + file_hash.cpp 23 → 12 + OpenSSL 宏 PUBLIC 化
- **修复公共头 ODR 隐患 + 测试 OpenSSL 路径长期被跳过**：
  `FALCON_USE_OPENSSL`/`FALCON_ENABLE_OPENSSL` 以 PRIVATE 挂在
  falcon_protocols，而 http_commands.hpp 等公共头按宏条件声明成
  员（TLS 会话指针等）——消费方 TU 看到不同类布局（ODR 错位风
  险）；测试 TU 看不到宏 → 13 个 FileHash 真实测试运行时
  GTEST_SKIP（"OpenSSL not available"，实际构建有 OpenSSL）。宏
  改 PUBLIC（与 OpenSSL::Crypto/SSL 的 PUBLIC 链接一致）后 13 个
  用例激活全过，**全量 ctest skip 60 → 11**（余量为设计内
  opt-in：10 个 DownloadIntegrationTest 环境变量门控 +
  HttpHandlerTest 两项既存占位）
- **删除死代码 mergeFile**（49 行）：private 且全库零生产调用
  方（downloadChanged 直写输出不经过它），唯一消费者是被整块注
  释的测试（"私有方法 - 暂时注释"）——.cpp 实现、.hpp 声明、注
  释测试块三处清出
- incremental_download.cpp gcov miss **46 → 11**：6 新用例挂
  `falcon_protocols_tests`——①CRLF+首尾空白哈希行裁剪（trim_
  line 尾部裁剪路径）；②零分块校验两态（元数据 `chunkSize: 0`
  被 v>0 守卫忽略按默认值合法解析 + defaultChunkSize=0 无覆盖
  时直达拒绝）；③未知哈希算法（EVP_get_digestbyname 失败 → 空
  串哈希，compare 优雅降级）；④无效远程哈希列表（算法元数据与
  请求不一致 → 解析空 → 回退全量下载建议）；⑤Range 短传服务器
  （Content-Length 与实际字节一致而尺寸不足，curl 判 CURLE_OK
  由调用方尺寸校验兜底）→ downloadChanged 干净失败且不产半成
  品；⑥目录读失败（POSIX fopen 目录成功而 read 必败——实测
  libstdc++ ifstream 对目录 open 即败走早返回，用例保底防回归）
- file_hash.cpp gcov miss **23 → 12**（宏激活 -7、新用例 -4）：
  2 新用例——未知算法枚举（calculate 的 switch 无 default →
  md_type 空指针）+ get_hash_length default 64
- **Windows CI 曝光生产缺陷并修复**：未知枚举用例在 Windows 上
  SEH 0xc0000005 崩溃（run 34892890668）——switch 无 default 使
  md_type=nullptr 直达 EVP_get_digestbyname；Linux OpenSSL 防
  null 安全返回（本地绿、掩盖了问题），Windows OpenSSL 解引用
  null 崩溃。修复：switch 补 `default: md_type = ""`，走既有
  EVP 获取失败防御路径（file_hash.cpp 85-87，两平台一致）。生
  产定性：损坏的持久化算法字段在 Windows 上会使 calculate 段错
  误；新用例在该路径上从"探测"升级为真回归守卫
- 剩余定性：incremental_download 11 = EVP DigestInit/Update/
  Final 失败 ×6（需 crypto 注入）+ 文件读取错误日志 ×2
  （ifstream 在本环境对目录/无权限文件 open 即败，读中失败无注
  入点）+ curl_easy_init OOM ×1 + downloadRange 零尺寸早返回
  ×1（唯一公开路径需经 downloadChanged 传零尺寸 chunk，将执行
  memcpy(dst, nullptr, 0) UB——记录为潜在健壮性缺陷，测试不覆
  盖 UB 路径）；file_hash 12 = >100MB 阈值日志 ×1（单测需
  100MB+ 内存文件，代价不成比例）+ EVP 五段失败防御 ×11（注入）
- **覆盖率（批次 O 收口，批次 C 同款 gcovr 口径）：行 81.2% /
  函数 94.5% / 分支 44.5%**（批次 N 81.0/94.3/44.3）；全量
  ctest **1938 = 1927 通过 + 11 设计内 skip，零失败**；新增 8
  用例 cov + ASan 双绿（ASan 全量重建后 protocols 696 + http 47
  + daemon rpc_client 47 + daemon storage 42 零告警）
- Windows CI 修复（c7feabc 单独提交）：http_handler_edges_test.
  cpp 补 `#ifdef _WIN32` 适配块（批次 G 漏 guard，MSVC C1083
  netinet/in.h——CI run 34883115118 的 Build 失败根因）
- 测量口径备忘：多 target 编译水分——同一 .cpp 编进多个 target
  时单份 gcov 的 miss 虚高（daemon/config.cpp 单份 54、两 target
  gcda 各跑 gcov 后 OR 合并实为 13）；矿点表使用前需按
  `paste a/x.gcov b/x.gcov | awk 两列同 #####` 口径复核

**批次 P 候选（##### 铁账，需按多 target OR 合并口径复核）：**
json_rpc_server 68（回环基建现成）/ resource_browser 系 153 /
request_group 系 106 / resource_search 47（WebCrawler 泄漏已
修）/ cloud_storage_plugin 55 行存根（方案 A：manager 只读插件
访问器）/ http_commands 176（TLS 防御与 OOM 为主）。

### 批次 P 收口（2026-09-14）：json_rpc_server.cpp 68 → 15 miss（RPC 分发边界 + WebSocket 协议路径收敛）
- **21 新用例三文件**（cov + ASan 双绿，全部复用既有基建）：
  `json_rpc_server_coverage_test.cpp` +11（HangingHandler 阻塞
  handler 基建）——changePriority/changeGlobalOption/tellStatus/
  getFiles 族/pause 族/removeDownloadResult 的参数形状与"合法 gid
  无任务"变体、整数值 max-concurrent-downloads、getOption 自定义
  header 回显、removeDownloadResult 终态移除与活动任务拒绝
  （code 1 "Task cannot be removed while active"）、forceShutdown
  无 handler 仅告警、bind 占口 start 失败（`#ifndef _WIN32`——
  Windows SO_REUSEADDR 允许双绑定）、半截 HTTP 头写端关闭连接；
  `websocket_test.cpp` +8——WS 升级 path 白名单 404、握手 CORS
  头回显、ping→pong 且 pong 被忽略、坏操作码 1002 close、慢分发
  +RST 双失败路径（广播命中死 fd + dispatch 返回后应答发送失败，
  set_shutdown_handler 睡 400ms 滞留会话线程 + SO_LINGER{1,0}
  RST 关闭；POSIX 须 SIGPIPE SIG_IGN）、Preparing→Downloading
  通知、进度节流窗口到期恢复推送、引擎移除后通知退化为仅 gid
  （TwoStageHandler 两阶段门 + set_status 无终态守卫 + remove_
  finished_tasks）；`json_rpc_storage_test.cpp` +2——unpauseAll
  的 Paused 收集/恢复落库链、tellWaiting storage 回落并集
- **gid 长度陷阱（既有测试名字与实际路径不符）**：
  gid_to_task_id 拒绝 >16 字符——既有用例的
  "00000000000ffffffc/d"（17-18 字符）命中的是非法 gid 路径而非
  "合法 gid 无任务"；后者须用 16 字符合法 gid
  "00000000000000ff"（=TaskId 255）
- **引擎语义记录**：task_manager worker 层在调 handler->download
  **之前**已置 Downloading（task_manager.cpp:831）——handler 内
  再 set_status(Preparing) 即产生 Preparing→Downloading 通知序
  列；372 行 ##### 为 gcc 行归属伪影（`||` 链指令归属 371 行，
  探针实证 status 2→1→2 通知发出 + 371/374 计数相等执行序矛盾
  铁证），与 987 同类
- 剩余 15 行全部定性：gcc 行归属伪影 ×2（372/987）、OOM 与发
  送失败注入 ×6（519-520 socket、559-562 listen、816 握手 send、
  854 PONG send 失败——PING 处理与会话读循环同线程无慢分发等
  价手段、636 accept EINTR 竞态）、时序竞态 ×2（1435/1460——
  pause_task/cancel_task 仅在任务消失瞬间可 false）、结构不可
  达 ×1（1321——add_task 失败恒抛异常走 -32603）、全枚举兜底
  ×1（153）、多 target 编译水分已排除（json_rpc_server.cpp 仅
  编进 falcon_daemon_rpc 单 target）
- **覆盖率（批次 P 收口，批次 C 同款 gcovr 口径）：行 81.4% /
  函数 94.4% / 分支 45.1%**（批次 O 81.2/94.5/44.5；函数 -0.1
  为边缘函数计数翻转，行 +0.2/分支 +0.6 为本批净贡献）；全量
  ctest **1958 = 1947 通过 + 11 设计内 skip，零失败**；新增 21
  用例 cov + ASan 双绿（daemon RPC 三套件 44 用例 ASan 零告警）

### 批次 Q 收口（2026-09-14）：resource_browser 系 75 → 9 + resource_search.cpp 47 → 9（格式化器/路径工具边界 + detail 提升重构）
- **24 新用例双树绿**（cov + ASan）：storage 侧 `resource_browser_
  edges_test.cpp` 17 用例三套件——BrowserFactoryEdges 4（默认工厂
  全建（s3 直断言 + crypto 五协议运行时 is_supported 守卫，PRIVATE
  宏对测试 TU 不可见）、available 有序且描述非空、create_from_url
  空 scheme/无 scheme nullptr、注册边界（空 protocol 拒/null 工厂
  拒/工厂产出 nullptr 仍注册成功））、BrowserFormatterEdges 5
  （format_tree 首行 base_path + 树枝字节转义常量（MSVC 源码编码
  防御）+ 收尾分支、max_depth>0 首层仍打印、format_table ls 风格
  d/-/l 类型标识、format_custom 五列 + 15 字符分隔线 + modified
  截 15 字符、symlink→other/未知列 "-"/空列表仅表头）、
  BrowserUtilsEdges 8（is_valid_path 控制字符（tab 放行、\x1f 拒
  ，DEL 0x7f 不在 <32 范围）、normalize_path 盘符与相对 ".." 保留
  （实现语义：所有组件前置 "/"，"a/b/.."→"/a"、../x→"/../x"、
  a/..→"." 特例）、join_path 空侧/绝对名/盘符根无额外分隔符、
  get_parent_path 尾斜杠/裸名 "."/根、get_filename 尾斜杠剥离）
- drives 侧 `resource_search_coverage_test.cpp` +7 + **detail 提升
  重构**（仓库既有 detail 模式）：validate_url/url_decode/
  parse_magnet_link 提升到 detail 命名空间（头文件补声明，含此前
  漏声明的 parse_size——测试前向声明块删除）；**删除 NoCrawlerTag
  孤儿构造函数**（零引用死代码）；**删除 set_headers 死防御 if**
  （headers_ 单调用点，curl_slist_free_all 对 NULL 安全）。用例：
  validate_url 前缀白名单（magnet/http/https/ftp 放行 + ed2k/
  thunder/"httpsfake" 拒绝）、parse_magnet_link 40 位 btih 哈希 +
  dn%20 解码 + tr 不入标题 + 非 40 hex 不匹配、url_decode 非法转
  义与截断 % 原样保留、provider 层 filter size 降序 + limit 截断
  （经 search_providers 直达 provider 输出，区别于 manager 全局排
  序）、sort_by 兜底键走 confidence 分支、空 base_url+search_path
  在发起请求前提前返回（离线安全）
- 剩余定性（三文件合计 18 行）：
  - resource_search 9：curl_easy_init OOM throw ×1；接口零调用方
    ×8（validate_url 转发 2 + get_details 主体 6——ISearchProvider
    方法生产全库零调用方，grep core/cli/daemon/desktop/drives 实
    证，Manager 不转发，结构不可达）
  - resource_browser 7：gcc 行归属伪影 ×7（50/54/57/60/63/66 六行
    register_browser 的 info 字面量实参行 + 218 lambda 赋值行——
    并列实参计数矛盾铁证：调用行命中 8 + lambda 行命中 28 + info
    行 #####；lambda 体 219-234 全命中且 print_tree 调用成功无
    bad_function_call ⇒ 赋值必执行）
  - resource_browser_utils 2：join_path 162 死代码（147 执行后
    back 恒 ∈ {'/','\\',':'} ⇒ 161 条件恒假）+ get_parent_path
    197 不可达（end==2 ⇒ 最后分隔符在 index 1 ⇒ path[1] 是分隔
    符，与 ==':' 矛盾）
- 多 target 编译水分已排除（三文件各仅一份库 target gcda；ASan
  树无 gcda）
- **覆盖率（批次 Q 收口，批次 C 同款 gcovr 口径）：行 81.9% /
  函数 94.9% / 分支 45.5%**（批次 P 81.4/94.4/45.1 → +0.5/+0.5/
  +0.4）；全量 ctest **1983 零失败**（1 例 WsRpcClientEdge 并行
  抖动串行复跑过）；ASan storage 359 + drives 146 全绿（1 例
  PerformanceLargeBatch 墙钟断言并行抖动 5.8s→串行 3.5s，非回归）

### 批次 R 收口（2026-09-15）：request_group 31 → 2 + cloud_storage_plugin 75 → 20 + http_commands 145 → 135（三文件 94 行收敛 + WS 测试挂死缺陷修复）
- **27 新用例三树绿**（cov 2010 + ASan 三套件 894 用例零告警）：
  protocols 侧 `request_group_test.cpp` +13（URL→文件名推导边界
  （query 剥离/空段回退 download）、自定义目录拼接、socks5 代理
  init 拒绝、resume 控制文件三态校验失败放弃断点（url 不一致/
  临时文件小于记录进度）、控制文件写失败容忍（内存追踪照常）、
  路径辅助守卫、Manager add 守卫（null/重复 id）+ 调度丢弃与
  resume 孤儿补插队往返）；`http_commands_edges_test.cpp` +8
  （零字节下载、>1MB 响应头终止、拆分计划 1 段回退单连接、
  chunked 大小行四变体帧错误（CR-pending+非 LF / CR-pending+LF
  补齐后无效 hex / 同缓冲 CR+非 LF / 同缓冲完整 CRLF+无效 hex）、
  **32MB 突发-静默-再突发 NEED_RETRY 旗舰**——execute 尾部
  update_progress + 回队 + 速度计算分支全链）；drives 侧
  `cloud_storage_coverage_test.cpp` +6（经新增 plugins() 只读访
  问器：注册序视图 13 插件、Lanzou/Baidu/Aliyun/Quark 四件套存
  根离线直调、轻量基类经子类指针驱动、6 平台 display_name 经
  无效链接错误路径 surfaced）
- **NEED_RETRY 可测化方法论**：阻塞 send 的剧本服务器会把发送
  节奏同步到客户端排水速度（背压恒小，64×64KB 轮上限永不触发）
  ——**深灌 + 客户端直写**才构成确定性触发：服务器每段 16MB 快
  速预灌（快于客户端 recv+落盘排水）、`enable_disk_cache=false`
  拖慢客户端，内核积压单调增长 → 单次 execute 必然读满 64 轮让
  出；静默 1200ms 后第二段再让出时距命令构造 >1s，顺带覆盖
  update_progress 速度计算分支
- **生产 API**：CloudStorageManager 新增 `plugins()` 只读访问器
  （指针生命周期归管理器，可驱动接口方法不可 delete）——批次 M
  遗留的「管理器不暴露插件指针 ⇒ 四件套存根 55 行不可达」经此
  打通离线直调面
- **修复 WsServerTest 断言失败即挂死缺陷**（全量 ctest 实证：
  负载下进度通知 5s 未到 → ASSERT 提前返回 → release() 漏调 →
  BlockingHandler 无界 cv_.wait → 析构 join 卡死 1:59:16）：
  cv_.wait 改 wait_for(30s)（TwoStageHandler 同款防御），一次
  修复覆盖全部四处 release-after-ASSERT 用法；flake 仍会红但
  不再无限挂
- 剩余定性（三文件 157 行）：request_group 2（create_initial_
  command 内层兜底——init() 三条失败路径全自带 set_status
  (FAILED)，结构性不可达）；cloud_storage 20（init_patterns
  map 初始化伪影 ×12 + quota 返回字面量伪影 ×6（断言通过即执
  行铁证）+ 第二循环已识别平台防御 ×2）；http_commands 135
  （TLS/SSL 防御 ×12、OOM ×12、WANT_WRITE 时序 ×4、send 失败
  ×4、磁盘写失败防御 ×6（2289 同类）、结构不可达防御与 private
  可见性 ×~30、2065-2076 第二完成收口（download_complete_ 全部
  置位点伴随 receive_data 返回 OK，2091 同轮收口先行，入口检查
  恒假）、多行语句伪影 ×若干）
- **多 target 编译水分已排除**（三文件各仅一份库 target gcda）
- **覆盖率（批次 C 同款 gcovr 口径）：行 82.4% / 函数 96.3% /
  分支 45.9%**（批次 Q 81.9/94.9/45.5 → +0.5/+1.4/+0.4）；全量
  ctest **2010 清单仅 2 例并行抖动失败（DownloadEngineTest.
  ResumeTask / TaskManagerPriorityTest.PriorityDequeueOrder，
  串行复跑即过）**；ASan protocols 717 +
  drives 152 + daemon_rpc 25 全绿

### 批次 S 收口（2026-09-15）：六小文件 64 行真矿清账（config/dispatcher 归零，resume/password/poll/epoll 收敛至不可测定性线）
- **31 新用例三树绿**（cov 全量 ctest **2043 清单 100% 通过
  零抖动** + ASan 三套件 421/734/27 零告警）：
  - daemon `config_test.cpp` +10：非 rpc 节的「节非 object」
    （daemon=5 / storage="x" / download=[]）与各节键类型错误
    （rpc.allow_origin_all / daemon.pid_file / daemon.working_
    dir / daemon.log_file / storage.task_db_path / download.
    max_overall_speed_limit / download.http_engine）——config.cpp
    13 → **0**
  - core `event_dispatcher_test.cpp` +4：dispatch_sync（未 start
    也直接派发、不经队列）/ get_listener_count（add/remove/clear
    计数）/ clear_listeners（清除后不再投递）/ is_running（生命
    周期三态）——四个公开方法此前**全库零调用**（既有
    DispatchSyncDoesNotQueue 名字误导：实测的是关异步后
    dispatch() 的同步路径）——20 → **0**
  - core `password_manager_test.cpp` +3：HOME 为空时
    password_hash_path 返回相对路径 ".falcon/.password_hash"
    （ScopedChdir 沙箱 + ScopedEnvVar("HOME","") 断言哈希落
    cwd）；无回调 prompt_password 控制台分支（istringstream 替
    换 cin.rdbuf，POSIX termios 块无条件执行全量覆盖）；
    generate_password(1, false, true) 第二轮 required_sets 入口
    命中长度 break——16 → 2
  - protocols `request_group_test.cpp` +14（ResumeControlCovS）：
    save 空 path / save 父目录缺失（ofstream tmp 创建失败）/
    load garbage 行（split 无 '='）/ total 空值 / total=12abc
    （consumed != size）/ total=abc / segments=abc / seg 三字段
    / 缺 total / total=0（187 三条件）/ segments=2 实际 1 段
    （190）/ body CRLF 裁剪成功 + 全 CRLF 魔数拒绝对照 /
    remove 空 path no-op——16 → 2
  - protocols `event_poll_test.cpp` +1 / `event_poll_poll_
    test.cpp` +2：epoll add 成功后关 fd 再 modify（EPOLL_CTL_
    MOD 得 EBADF）；poll 对已关正整数 fd 的 fcntl(F_GETFL) 探
    测失败（区别于 -1 的 EINVAL 路径）；双注册单就绪时静默 fd
    的 revents==0 跳过（poll 返回恰 1、回调恰 1 次）——
    epoll 15→14 / poll 11→9
- **测量级发现**：resume 控制文件的 \r 裁剪（load 132 行）只作
  用于 body 行——魔数比较（122 行）先于裁剪，完整 CRLF 文件被
  魔数直接拒绝；这是严格语义而非缺陷（save 端 binary 模式恒写
  LF），测试以成功/拒绝对照对固化
- 剩余 29 行全部定性（不可测）：password 2（RAND_bytes 失败，
  无注入点）/ resume 2（save 写中途失败）/ epoll 14（create1
  失败 ×2、epoll_fd_<0 结构分支 ×4、EINTR ×2、等待失败 ×2、
  remove-wait 间未知 fd 竞态 ×3）/ poll 9（nfds_t 超量需 20 亿
  fd ×2、EINTR ×2、poll 失败 ×2、未知 fd ×3）
- **多 target 编译水分排除**：daemon/config.cpp 双 target（tests
  与 main_integration）OR 合并口径复核 0
- **覆盖率（批次 C 同款 gcovr 口径）：行 82.7% / 函数 96.7% /
  分支 46.1%**（批次 R 82.4/96.3/45.9 → +0.3/+0.4/+0.2）

**批次 T 候选（##### 铁账行号已在手，待细读定性）：**
- `download_engine_v2.cpp` 23（57, 69, 186, 195, 205, 275, 368,
  381, 498-499, 546, 578, 585, 769, 772, 791-792, 840-841,
  896-897, 901-902）
- `json_rpc_client.cpp` 23（57, 73, 109, 119, 130, 148-149,
  157-158, 201-233 连串）
- `download_engine.cpp` 17（37, 66, 110, 115, 119, 125, 153,
  164, 167, 179, 365-367, 369-370, 372, 381）
- `v2_http_download_adapter.cpp` 16（67-70, 77, 81, 102, 107,
  118, 120, 124-126, 129-131）
- `daemon/daemon.cpp` 16（48, 66, 123-125, 129, 134-136,
  142-144, 147, 680-681, 688）
- daemon `src/main.cpp` 16（188-189, 245, 255-256, 273, 341,
  352-353, 385, 452, 471, 480, 486, 490, 497；dump 匹配须用
  `src/main.cpp`——路径为 packages/falcon-daemon/src/main.cpp，
  子串 "daemon/main.cpp" 不命中）
- 行号来自批次 S 全量 ctest 后的新 gcda（2043 清单采集），
  直接可信；初判：多为网络注入 / 线程时序 / 平台分支

### 2026-09-18 - aria2 功能对齐差距分析（立项路线）

**背景：** 以 aria2 为参照做功能覆盖盘点（基于 grep 全库实证，
非文档宣称）。结论：主下载路径已对齐或以自有方式覆盖，缺口
集中在 Metalink / SFTP / BT 做种策略与若干 HTTP 细节。

**已对齐（含等价实现）：**
- ✅ HTTP(S)/FTP(S)：V1 curl + V2 裸 socket 双引擎；分段下载、
  断点续传（.falcon.ctrl 控制文件 + If-Range 内容变更防护，
  语义对齐 aria2 -c 且更严）
- ✅ 限速：全局 + 任务级（V2 滑动窗口 / V1 curl 通道热应用）
  ≈ aria2 --max-overall-download-limit + --max-download-limit
- ✅ 重试：max_retries + retry_delay_seconds + 指数退避
  ≈ --max-tries + --retry-wait
- ✅ 代理：HTTP 明文（absolute-form）+ CONNECT 隧道（V2）；
  V1 curl 另有 socks/HTTPS 代理。≈ --all-proxy
- ✅ BT/Magnet：DHT（Kademlia 迭代查找）/ PEX / tracker；
  ≈ aria2 的 DHT+PEX+utp 基础面
- ✅ RPC：JSON-RPC over HTTP+WS 28 方法 aria2 兼容（AriaNg 可
  直连）+ 事件通知 + 会话持久化（SQLite + auto-save + 重启恢复，
  ≈ --save-session/--input-file 语义）
- ✅ cookie 引擎、HTTP Basic 认证（401 挑战重放）、UA/自定义头；
  Digest/NTLM 由 V1 CURLAUTH_ANY 覆盖（http_handler.cpp:246）
- ✅ 已存在文件保护：V2 overwrite 门禁（明确报错或显式授权覆盖）
  ≈ --allow-overwrite=false 语义

**Falcon 独有（aria2 无）：** 包装协议 thunder/flashget/qqlink、
ED2K、HLS/DASH；云存储浏览（S3/OSS/COS/Kodo/Upyun，endpoint
path-style）；网盘分享链识别（12 平台）+ 资源搜索；GUI 桌面。

**缺口（按价值排序，附实证）：**
1. **Metalink（aria2 语义）**：现 metalink 插件是 2025-12 旧接口
   占位（getSupportedSchemes/canHandle 旧 API、自造 "metalink:"
   scheme、独立 CMake 子项目未编入主库；builtin 注册表仅 8 个
   handler 无 metalink）。缺 aria2 的真语义：.metalink4/.meta4
   文件输入 → 多源镜像并行 + piece 级哈希校验。**立项首选**——
   可复用 V2 分段骨架（多源=多段来源）+ file_hash 分块哈希
2. **SFTP**：同上形态（plugins/sftp 449 行旧接口未接入注册表）；
   aria2 原生支持。可经 V1 curl 的 SCP/SFTP 能力低成本接入
   （需 libssh2 构建项）
3. **BT 做种策略**：seed-ratio/seed-time 全库零命中——下载完
   即停，无法做种保活（PT 站核心需求）。需要 BT handler 增加
   完成后保留会话 + 上传计量 + 条件退出
4. ~~**gzip/deflate content-encoding**~~（2026-09-19 收口，方案与本
   条原始设想不同）：原条目前提「V1 curl 自动解压」有误——libcurl
   不设 CURLOPT_ACCEPT_ENCODING 就不协商也不解压，全库 grep 零命中，
   即 V1/V2 现状一致（原样落盘）。真缺口是「服务器无视协商强制压缩
   时客户端毫无告警」。落地为 aria2/wget 同语义三件事：① V2 GET 恒
   带 `Accept-Encoding: identity`（wget 同语义，正常服务器不再压缩）；
   ② 2xx 响应带非 identity content-encoding 时记 INFO 日志（原样
   落盘不再静默）；③ 测试钉住「强制 gzip → 字节原样落盘」不变式
   （含 chunked 组合）。**不做透明解码**：输出文件与 URL 响应体逐
   字节一致是下载器不变式（分段拼接/断点续传/metalink 哈希校验依
   赖它），解码需 V1/V2 同步改且偏离 aria2——真需要解码时应是显式
   特性（如未来「--compressed」选项）而非引擎隐式行为
5. ~~**auto-file-renaming**~~（2026-09-19 已落地）：CLI 有
   --auto-file-renaming 参数（arg_parser.cpp:256）但
   DownloadOptions 零消费——引擎侧是"拒绝或覆盖"二值，无 .1/.2
   自动改名（与限速当年的"零消费端"同型）
6. ~~**conditional-get**~~（2026-09-19 已落地）：If-Modified-Since/
   If-Match 零命中（aria2 --conditional-get，配合镜像同步场景）
7. **V2 IPv6**：resolve_host 已 AF_UNSPEC，数据面 socket
   AF_INET-only——IPv6 目标在 V2 判失败（ip6-localhost 有干净
   失败分支，批次 D 测试钉住）；生产默认 V1 不受影响，V2 灰度
   面收窄项
8. **file-allocation**：连 CLI 参数都没有——V2 稀疏临时文件 /
   V1 curl 直写；大文件预分配（falloc）对机械盘碎片与空间预留
   有意义，优先级低
9. **客户端 TLS 证书**：V1/V2 均无（CURLOPT_SSLCERT 未接线）
   ——aria2 --certificate/--private-key，双向 TLS 场景需要
10. ~~**RustFS / MinIO 系 S3 兼容服务的真鉴权**~~（2026-09-19 已落地）：
    MinIO 社区版闭源化后 RustFS（S3 兼容、Apache-2.0）成为自建替代
    主流。**真缺口是 s3_browser 无 SigV4**——perform_s3_request 原本
    只发 Date/Host 匿名请求，对强制鉴权服务必 403 AccessDenied。落地
    三件：① SigV4 头签名接入 perform_s3_request（有凭据即签：host +
    x-amz-date + x-amz-content-sha256 + Authorization，线上头与参与签
    名头一一对应；**计划修正——「复用 s3_plugin.cpp 的 S3Authenticator」
    前提为假**，nm 实证该文件从未接入任何构建目标、库符号零
    S3Authenticator、生产零消费方，按 websocket_server/http_plugin_v2
    先例整体删除死文件，提取为活代码 s3_authenticator.{hpp,cpp} 并
    扩展：sign_request 增 query_params 参签（ListObjectsV2 带查询串必
    须进规范请求）、sha256 公有化；OpenSSL 缺席构建回落匿名模式，
    FALCON_ENABLE_CRYPTO_STORAGE_BROWSERS 门控）；② mock 测试加带请
    求头的 RawHandler 形态 + s3_browser_auth_test 3 用例（**测试侧
    OpenSSL 独立重导签名全程**与线上 Authorization 精确比对——不经
    S3Authenticator，避免同 bug 自我印证；HEAD 对象无查询签名相等 /
    ListObjectsV2 查询规范化——prefix=docs/ 线上 docs%2F 签名前解码
    还原再规范重编码排序，二次编码 %252F 或乱序即红 / 无凭据匿名路
    径保持）；③ README 双语 MinIO → MinIO/RustFS（标注 SigV4 签名）
    + 桌面云盘页核对（endpoint/access_key/secret_key 已透传 options，
    库层签名自动生效，零改动）

**记录不修的假缺口：** HTTP Digest（V1 CURLAUTH_ANY 已含，V2
经适配层 401 回退 V1 兜底）、socks/HTTPS 代理（V2 判 Unsupported
回退 V1）、Referer（同回退）——均为"V2 原生化"项而非生产缺口，
随 V2 引擎成熟度逐步收编。

### 2026-09-18 - P2SP 共享网络路线讨论（立项备忘）

**背景：** 产品方向讨论——"只用 Falcon 能否形成一张共享网络
（DHT 或之外）？迅雷就是这么做的？"。结论：能做，且**不需要
中央服务端**；迅雷选中央索引是商业选择（缓存服务器、内容审核、
付费加速），不是技术必需。真正的门槛是冷启动（网络效应）与
客户端的对等服务能力。

**参照系（迅雷 P2SP 模式解剖）：**

- 下载时同时拉三类源：原始 HTTP 服务器 + 其他迅雷用户（含半截
  持有者）+ 迅雷自建缓存服务器
- 靠**中央索引服务**（文件 hash → 用户列表）发现源；客户端默认
  开启共享（下载完自动给别的用户上传）——"越用越快"的来源，
  也是早年"偷跑上传"争议的来源
- 同类玩家：电驴 eD2k（服务器目录 + KAD DHT）、快播 QVOD、
  BitTorrent 生态（公共 DHT）、WebTorrent/Resilio

**Falcon 现状盘点（离这张网已有多远）：**

- ✅ 已接入公共 BitTorrent DHT（`dht_node.cpp`，BEP-5，引导
  router.bittorrent.com）+ PEX——BT 任务时与全球 BT 客户端同处
  一张路由表；但这不是 Falcon 自己的网络
- ✅ 内容寻址地基：Metalink 落地的 `FileHasher::
  calculate_streaming/verify_streaming`（流式哈希，GB 级安全），
  "两个 HTTP 下载是否同一文件"现在可判定
- ✅ 引擎底座在路上：V2 原生多源分段（= Metalink 阶段2，即
  上文差距分析 #1 的后半）——"同一文件多源并发拉取 + 逐段校验"
  正是 P2SP 的引擎核心
- ❌ 缺：入站监听（现为纯客户端，只出站）、索引公告机制、
  客户端间数据协议（HTTP 文件不会）、上传策略

**三条路线（服务端需求对照）：**

| 路线 | 中央 server | 说明 |
|---|---|---|
| A. 公共 BT DHT 桥接 | ❌ | HTTP 下载按 sha1 派生合成 infohash（**加魔数前缀防与真实种子碰撞**）公告进公共 DHT；零基建，复用现有 DHT 代码，Falcon 之间互相发现 |
| B. Falcon 私有 DHT | ❌ | 同一套 Kademlia 代码换引导节点/魔数成 Falcon 专属表；引导节点=几个硬编码普通客户端即可；冷启动难（没用户=空表） |
| C. 中央索引（迅雷正解） | ✅ | 效果最好但需要服务器基建 + 运营 + 内容责任的法务承担（迅雷/电驴均被版权方起诉过）；已超出写代码范畴 |

**关键认知：要补的不是"server 程序"，是客户端的对等服务能力
（三件事）：**

1. **入站监听**：TCP 数据服务（对端来拉分片）+ UDP（DHT 节点
   通信——这半边已有，`DhtClient::start()` 本就绑 UDP 端口）
2. **索引公告**：完成（或下载中）把「持有 sha256=X，地址
   ip:port」公告进 DHT
3. **上传策略**：给不给/给多快/是否互惠（BT tit-for-tat）——
   策略问题，非基建

其中数据服务最省的实现不需要新协议：**内嵌一个只读、按内容
寻址的 HTTP 服务**（复用现有代码很薄），对端发现后直接
`GET /by-sha256/...` 拉分片。闭环示例：

```
Falcon A 下载完 hello.iso（流式哈希校验过）
  → 公告 (hash → A 的 ip:port) 进 DHT
Falcon B 从慢速源下载同一文件
  → DHT 查到 A → 并发从 A + 原始服务器同时拉（P2SP）
```

**路线 B 的替代方案（更彻底）**：把 HTTP 下载做成合成 torrent
做 seed（libtorrent 已有），WebTorrent 式桥接——代价是引入
libtorrent 强依赖（当前 FALCON_ENABLE_BITTORRENT=OFF 为默认
配置之一）。

**务实顺序（本文件立项路线的延伸）：**

1. 先做 **Metalink 阶段2**（V2 原生多源分段）——纯引擎能力，
   无论将来接哪条路线都是底座；与差距分析 #1 合并推进
2. 发现层从**路线 A** 起步（零基建、最快验证「Falcon 之间互相
   借力」核心体验）：合成 infohash 公告 + DHT 查询复用
3. 数据面加**入站监听 + 内嵌只读内容寻址 HTTP 服务**
4. 上传策略与共享开关（默认关闭、用户显式开启——吸取迅雷
   默认共享的舆论教训）
5. 路线 C（中央索引）仅在产品验证后另行评估，涉及运营与合规，
   不在本仓库工程范畴

**风险与约束（如实记录）：**

- 冷启动是产品问题不是工程问题：第一个用户查 DHT 是空表，
  加 server 也解决不了（迅雷索引的价值同样来自用户量）
- 法律面：任何"分享网络"形态都有内容分发责任（参考迅雷、
  电驴被诉史）；默认上传的伦理争议真实存在——共享必须默认关
- NAT/防火墙穿透（公网可达性）未在上文展开，路线 A 落地前需
  补一轮评估（STUN/打洞或 UPnP 映射；纯 DHT 公告对 NAT 后
  节点不可达）
