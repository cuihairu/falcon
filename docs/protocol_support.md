# Falcon 下载器协议支持文档

## 已实现的协议和功能

### 1. 基础协议

#### HTTP/HTTPS ✅
- **文件位置**: `plugins/http/http_plugin.cpp`
- **功能特性**:
  - 基础GET/POST请求
  - 断点续传（Range请求）
  - 多线程分块下载
  - 速度控制
  - 自定义请求头
  - 重定向跟随
  - 代理支持（HTTP/SOCKS4/SOCKS5）
  - SSL证书验证控制

#### FTP ✅
- **文件位置**: `plugins/ftp/ftp_plugin.cpp`
- **功能特性**:
  - 被动/主动模式
  - 匿名登录和认证登录
  - 断点续传（REST命令）
  - 目录列表
  - 文件信息获取

### 2. 私有协议和磁力链

#### Thunder（迅雷）✅
- **文件位置**: `plugins/thunder/thunder_plugin.cpp`
- **支持格式**:
  - `thunder://` (标准格式)
  - `thunderxl://` (迅雷极速版)
- **实现方式**: Base64解码还原为HTTP链接

#### QQDL（QQ旋风）✅
- **文件位置**: `plugins/qqdl/qqdl_plugin.cpp`
- **支持格式**: `qqlink://`
- **实现方式**: GID验证 + Base64解码

#### FlashGet（快车）✅
- **文件位置**: `plugins/flashget/flashget_plugin.cpp`
- **支持格式**: `flashget://`
- **实现方式**: URL解码 + 参数提取

#### ED2K（电驴）✅
- **文件位置**: `plugins/ed2k/ed2k_plugin.cpp`
- **支持格式**: `ed2k://`
- **实现方式**: MD4哈希解析 + eD2k链接处理

#### BitTorrent/Magnet ✅
- **文件位置**: `plugins/bittorrent/bittorrent_plugin.cpp`
- **支持格式**:
  - 磁力链接 `magnet:?xt=urn:btih:`
  - 种子文件 `.torrent`
- **功能特性**: DHT网络支持、Tracker支持、Piece校验

#### HLS/DASH ✅
- **文件位置**: `plugins/hls/hls_plugin.cpp`
- **支持格式**:
  - M3U8播放列表（HLS）
  - MPD清单文件（MPEG-DASH）
- **功能特性**: 自适应码流、多分辨率选择

### 3. 网盘和云存储

#### 蓝奏云 ✅
- **文件位置**: `src/cloud_storage_plugin.cpp`
- **支持格式**: `lanzouy.com`、`lanzoux.com`等
- **功能特性**:
  - 分享链接解析
  - 密码提取支持
  - 文件信息获取
  - 直链下载

#### 其他网盘支持框架 🚧
- **支持的网盘平台**:
  - 百度网盘
  - 阿里云盘
  - 腾讯微云
  - 115网盘
  - 夸克网盘
  - PikPak
  - MEGA
  - Google Drive
  - OneDrive
  - Dropbox
  - Yandex Disk

### 4. 对象存储

#### Amazon S3 ✅
- **文件位置**: `plugins/s3/s3_plugin.cpp`
- **支持格式**:
  - `s3://bucket/key`
  - `https://bucket.s3.region.amazonaws.com/key`
  - `https://s3.region.amazonaws.com/bucket/key`
- **功能特性**:
  - AWS V4签名认证
  - 预签名URL生成
  - 多部分上传/下载
  - 存储类别支持
  - 区域配置
  - MinIO等兼容支持

### 5. 资源搜索功能

#### 搜索引擎集成 ✅
- **文件位置**: `src/resource_search.cpp`
- **支持的搜索引擎**:
  - TorrentGalaxy
  - 1337x
  - ThePirateBay
  - EZTV
  - MagnetDL
- **功能特性**:
  - 多引擎并发搜索
  - 结果去重和排序
  - 智能筛选（大小、种子数等）
  - 网盘资源搜索支持
  - JSON配置文件支持

### 6. NAS协议支持计划 📋

#### Samba/CIFS 🔄
- **协议说明**: Windows网络共享协议
- **实现计划**:
  - 使用libsmbclient库
  - 支持SMB 2.0/3.0
  - 认证支持（用户名/密码、Kerberos）
  - 文件浏览和下载
- **预计实现时间**: Q1 2024

#### NFS 🔄
- **协议说明**: 网络文件系统协议
- **实现计划**:
  - 使用libnfs库
  - NFSv3/NFSv4支持
  - 自动挂载功能
  - 大文件传输优化
- **预计实现时间**: Q1 2024

#### WebDAV 🔄
- **协议说明**: 基于HTTP的分布式文件系统
- **实现计划**:
  - HTTP/WebDAV客户端实现
  - 支持认证（Basic/Digest）
  - 属性获取和管理
  - 集成到现有HTTP插件
- **预计实现时间**: Q2 2024

#### FTPS/SFTP 🔄
- **协议说明**: 安全文件传输协议
- **实现计划**:
  - SFTP: 使用libssh2
  - FTPS: OpenSSL集成
  - 主机密钥验证
  - 公钥/私钥认证
- **预计实现时间**: Q1 2024

### 7. 高级功能

#### 代理支持 ✅
- **支持类型**:
  - HTTP代理
  - SOCKS4代理
  - SOCKS5代理
  - 代理认证（用户名/密码）
- **配置方式**: 命令行参数、配置文件

#### 资源搜索与网盘集成 🚀
- **创新功能**:
  - 搜索结果包含网盘分享链接
  - 自动识别和解析网盘链接
  - 统一的下载体验

### 使用示例

```bash
# HTTP下载
falcon-cli https://example.com/file.zip

# 磁力链接下载
falcon-cli "magnet:?xt=urn:btih:..."

# S3对象下载
falcon-cli s3://my-bucket/path/to/file

# 搜索资源
falcon-cli --search "Ubuntu 22.04" --min-seeds 10 --download 1

# 使用代理下载
falcon-cli https://example.com/large.iso --proxy socks5://127.0.0.1:1080

# 网盘下载（需要密码）
falcon-cli https://www.lanzoux.com/iabcdefg --password "123"
```

## 架构设计

### 插件化架构
- 所有协议通过统一的`IProtocolHandler`接口实现
- 动态加载和卸载插件
- 配置化的功能开关

### 核心组件
1. **DownloadEngine**: 下载引擎核心
2. **TaskManager**: 任务管理和调度
3. **PluginManager**: 插件管理和路由
4. **ResourceSearchManager**: 资源搜索管理
5. **CloudStorageManager**: 网盘存储管理

### 依赖库
- **必需**: libcurl（HTTP请求）
- **可选**:
  - nlohmann/json（JSON解析）
  - spdlog（日志）
  - libtorrent（BitTorrent）
  - OpenSSL（S3签名）
  - libssh2（SFTP，计划中）
  - libsmbclient（Samba，计划中）

## 未来规划

### 短期目标（3个月）
1. 完善NAS协议支持（Samba、NFS、WebDAV）
2. 优化搜索性能和准确性
3. 增加更多网盘平台支持
4. 完善错误处理和重试机制

### 中期目标（6个月）
1. 实现P2P网络加速
2. 添加视频流直接播放支持
3. 实现任务调度和优先级管理
4. 开发GUI界面

### 长期目标（1年）
1. 支持更多专业存储系统
2. 实现分布式下载网络
3. 添加AI辅助资源推荐
4. 企业级功能和管理界面

---

**最后更新**: 2025-12-21
**版本**: 0.1.0