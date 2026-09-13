[根目录](../../../../CLAUDE.md) > [packages](../../../) > [libfalcon](../../CLAUDE.md) > [plugins](../) > **bittorrent**

---

# BitTorrent Plugin

## 变更记录 (Changelog)

### 2026-09-13 - DHT Kademlia 迭代查找落地
- `dht_node.{hpp,cpp}` 补齐异步迭代查找：`LookupContext` 按 lookupId
  管理候选集/待响应集，α=3 并发、k=8 上报，响应中发现新候选继续
  迭代，收敛/超时（默认 30s 可配）终结并触发回调（回调一律锁外
  执行）；已查询/待响应以端点（ip:port）为键（引导节点 ID 未知，
  不得因响应带回真实 ID 而重复查询）
- 修复三既有缺陷：公网域名引导节点从不解析 DNS、发往 0.0.0.0 且
  拖住查找永不收敛（`sendMessage` 返回 bool + 发送失败快速终结；
  新增 `clear_bootstrap_nodes()`）；`nodeIdFromString` 原为 memcpy
  截断而非 hex 解码（encode/decode 不对称）；get_peers 的
  `info_hash` 从 40 字符 hex 文本改为 20 字节原始值（BEP-005）
- 测试：`tests/unit/dht_node_test.cpp` 9 用例（本地 UDP mock 网络
  端到端：两跳迭代、距离序上报、超时/无效端点快速终结等）

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
