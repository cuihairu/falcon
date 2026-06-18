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

