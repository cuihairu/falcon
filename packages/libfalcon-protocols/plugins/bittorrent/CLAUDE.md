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

### 2026-09-13 - 覆盖率专项（PEX/bencode/BT 解析测试补全 + PEX 缺陷修复）
- 修复 `pex_protocol.cpp` 既有缺陷：handlePexMessage Add 分支对
  已存在 peer 重复触发发现回调——仅新插入候选集才触发
- 新增 `tests/unit/pex_protocol_test.cpp` 25 用例：消息编解码
  往返、Add/Drop 字节布局、握手/无握手分发、候选去重、回调触发、
  异常输入
- 新增 `tests/unit/bencode_edge_test.cpp` ~12 边界用例：深层嵌套、
  空容器、键序校验、非最小整数编码、截断输入、大整数
- `tests/unit/bittorrent_parse_test.cpp` 补全 ~10 用例（can_handle
  /get_file_info 单多文件/错误路径）；手写 bencode 测试数据的
  长度前缀错误（`3:aa` 声明 3 字符实际 2 字符）用 Python 迷你
  解析器逐字符验证最可靠
- ASan 构建 BT 86 用例零告警（`FALCON_ENABLE_BITTORRENT=ON` 下
  纯 C++ 模式，无需 libtorrent）

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
