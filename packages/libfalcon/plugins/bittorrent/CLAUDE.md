[根目录](../../../../CLAUDE.md) > [packages](../../../) > [libfalcon](../../CLAUDE.md) > [plugins](../) > **bittorrent**

---

# BitTorrent Plugin

## 变更记录 (Changelog)

### 2025-12-21 - 初始化插件架构
- 创建 BitTorrent 插件目录结构

---

## 插件职责

BitTorrent/Magnet 协议插件，基于 **libtorrent-rasterbar** 实现，支持：

- .torrent 文件下载
- Magnet 链接下载
- DHT、PEX、LSD
- 文件选择（多文件 torrent）
- 速度限制

---

## 依赖

- **libtorrent-rasterbar** 2.0+

---

## 下一步

1. 集成 libtorrent-rasterbar
2. 实现 `BitTorrentPlugin` 类
3. 支持 Magnet 链接解析
4. 编写集成测试（需测试 torrent 文件）

---

**文档维护**：实现完成后更新本文档。
