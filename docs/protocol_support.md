# Falcon 下载器协议支持文档

> [!NOTE]
> 本文档属于历史总览材料；当前协议支持状态请以 `docs/src/protocols/README.md` 为准。

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

#### 网盘支持矩阵（原生 WebDAV / Alist 聚合）✅
- **文件位置**: `packages/libfalcon-storage/plugins/webdav/webdav_browser.cpp`（浏览/元数据）、`include/falcon/storage/storage_presets.hpp`（连接预设）
- **支持格式**: `webdav://[user:pass@]host[:port]/path`（`dav://`、`davs://`、`webdavs://` 同义）
- **功能特性**:
  - PROPFIND 浏览（Depth 0/1，207 multistatus 解析，命名空间前缀容忍）
  - MKCOL / DELETE（递归 Depth: infinity + 逐项兜底）/ MOVE / COPY（Destination）
  - 鉴权由 curl 401 挑战协商（Basic/Digest）
  - RFC 4331 配额（服务器不支持时返回空）
  - href 百分号解码、路径逐段编码、递归扁平化列表
- **网盘清单**:

| 网盘 | 承载方式 | 预设 id | 说明 |
|------|----------|---------|------|
| Google Drive | Alist 聚合 | `google-drive` | OAuth 由 Alist 侧完成 |
| OneDrive | Alist 聚合 | `onedrive` | 个人/商业版 |
| Dropbox | Alist 聚合 | `dropbox` | |
| pCloud | Alist 聚合 | `pcloud` | |
| MEGA | Alist 聚合 | `mega` | 凭据配在 Alist 存储驱动 |
| 坚果云 | 原生 WebDAV | `jianguoyun` | `https://dav.jianguoyun.com/dav`，应用密码 |
| 阿里云盘 | Alist 聚合 | `aliyundrive` | 国内既有矩阵 |
| 百度网盘 | Alist 聚合 | `baidu` | 国内既有矩阵 |
| 天翼云盘 | Alist 聚合 | `tianyi` | 国内既有矩阵 |
| 115 网盘 | Alist 聚合 | `115` | 国内既有矩阵 |
| 通用 WebDAV | 原生 | `webdav` | NAS/InfiniCloud/自建 |
| Alist 端点 | 原生 | `alist` | 任意 Alist 实例 |

- **数据面边界**: 网盘文件的实际下载（数据面）由协议层 WebDAV 插件承接，
  URL 形如 `dav(s)://host[:port]/dav/path`；浏览、建删、重命名、移动、
  配额面均已由本浏览器落地。
- **验证边界**: mock HTTP 测试覆盖全部请求/解析路径（401 Basic 挑战、
  命名空间大小写、绝对 URL href、递归兜底删除等 25 例）；坚果云线上
  端点、Alist 容器实测见下方「真实连通走查」。

### 4. 对象存储

#### S3 兼容对象存储矩阵 ✅
- **文件位置**: `packages/libfalcon-storage/plugins/s3/s3_browser.cpp`、`include/falcon/storage/storage_presets.hpp`
- **协议面**: ListObjectsV2 XML（`list-type=2`）——真 S3/MinIO/R2/B2/Wasabi/GCS
  一律应答 XML `ListBucketResult`，`xml_scan.hpp` 手工扫描器解析（零新增依赖，
  命名空间容忍 + 五预定义实体解码）；历史 JSON 形态保留为兼容路径
- **厂商矩阵**:

| 厂商 | 预设 id | endpoint | 鉴权 | 备注 |
|------|---------|----------|------|------|
| Amazon S3 | `aws-s3` | 按 bucket+region 自动拼 | SigV4 | `region` 默认 us-east-1 |
| Cloudflare R2 | `cloudflare-r2` | `https://<account_id>.r2.cloudflarestorage.com` | SigV4 | 控台 R2 页取账户地址 |
| Backblaze B2 | `backblaze-b2` | `https://s3.<region>.backblazeb2.com` | SigV4 | region 如 us-west-004 |
| Wasabi | `wasabi` | `https://s3.<region>.wasabisys.com` | SigV4 | |
| Google Cloud Storage | `google-cloud-storage` | `https://storage.googleapis.com` | SigV4 + HMAC | HMAC 密钥在互操作性设置创建 |
| MinIO / 自建 S3 兼容 | `minio` | 用户自填（含端口） | SigV4 | MinIO/RustFS/SeaweedFS/Ceph RGW |
| Azure Blob | `azure-blob` | 经 Alist | Alist 账户 | Azure 无 S3 兼容协议，走网盘载体 |

- **国内云厂商（既有矩阵，预设沿用）**: Alibaba OSS（`oss`）、Tencent COS
  （`cos`）、Qiniu Kodo（`kodo`）、Upyun（`upyun`）
- **验证边界**: 本批已修复并实测 S3 浏览面 XML 解析与 delimiter 目录
  语义（RustFS 真栈 12 项全过，此前 XML 解析缺失时 list 恒空）；
  OSS/COS/Kodo/Upyun 四家浏览器仍为 JSON 解析路径，与真面 XML 形态
  不符（同款缺口，本批未修，连通性走查未做）——国内四家以协议兼容面
  实现 + mock 测试为界。

#### 真实连通走查（2026-10-02，docker 真栈）
- **RustFS**（docker 容器，S3 兼容真面，SigV4 强校验）：connect 探活 /
  delimiter 目录语义（嵌套对象不再泄漏进根层）/ 文件大小与元数据 /
  建目录 / 目录重命名（copy 回退尾斜杠标记）/ 删除标记清理——12 项全过。
  走查中发现并修复两处真面缺陷：① 非 list 未带 delimiter 时嵌套对象
  平铺泄漏为根级文件；② PUT 复制目录标记 404（NoSuchKey），DELETE 静默
  漏删尾斜杠标记。备注：MinIO 官方已撤下 Docker Hub 镜像（quay 又被
  本机网络挡），选 RustFS 作为 S3 兼容真栈——同协议面，SigV4 校验比
  MinIO 更严格。
- **Alist**（docker 容器，WebDAV 端点，Local 驱动挂载）：PROPFIND 探活/
  浏览 / MKCOL / MOVE / 递归 DELETE 全过；**COPY（集合与文件）Alist 均
  回 500**——其 WebDAV COPY 实现限制（与本端实现无关），已如实登记；
  配额字段不返回（RFC 4331 缺席 → 空 map，符合约定）。
- **rclone serve webdav**（docker 容器，合规 WebDAV 真面）：PROPFIND /
  文件 COPY（201）/ DELETE 全过——COPY 语义在合规服务器上验证通过。
- **坚果云 / AWS / R2 / B2 / Wasabi / GCS**：无线上账号，按协议兼容面
  实现 + mock 测试桩覆盖（XML 形态与各厂商文档一致），未做线上实测
  ——边界如实标注。

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

**最后更新**: 2026-10-02
**版本**: 0.2.0
