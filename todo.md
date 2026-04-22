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
- ⏳ CloudPage 与 libfalcon-storage 集成
- ⏳ DiscoveryPage 搜索 API 集成

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
- ⏳ 完整的多线程分段下载
- ⏳ BitTorrent 插件与 DHT/PEX 集成

**待实现：**
- ⏳ CloudPage 与 libfalcon-storage 的完整 C++ 绑定
- ⏳ DiscoveryPage 与真实搜索 API 集成
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

