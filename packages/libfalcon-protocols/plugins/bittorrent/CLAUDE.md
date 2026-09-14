[根目录](../../../../CLAUDE.md) > [packages](../../../) > [libfalcon](../../CLAUDE.md) > [plugins](../) > **bittorrent**

---

# BitTorrent Plugin

## 变更记录 (Changelog)

### 2026-09-14 - 覆盖率批次 E：bittorrent_plugin.cpp 187 → 7 miss + 四缺陷修复
- **magnet infoHash off-by-one**：`"xt=urn:btih:"` 是 12 字符，旧代
  码 `pos + 11` 截取——magnet 任务的 infoHash 恒带前导冒号
  （`can_handle` 用 `kXtPrefix.size()` 正确而 `download()` 错误，
  两处不一致即证据）。提取/归一化收口为公开 static
  `extract_info_hash`/`info_hash_to_hex`（hex 小写归一；32 位
  Base32 经 `base32Decode` 解 20 字节转 hex）
- **Base32 magnet 不解码**：`can_handle` 接受 32 位 base32，但
  `download()` 把 base32 文本原样传 `findPeers`——
  `nodeIdFromString` 只认合法 40 位 hex 否则字节截断，必然查询错
  误 info_hash；`base32Decode` 存在却从未被调用（死代码激活）。
  `base32Decode` 查表改大小写不敏感（RFC 4648）
- **parseBencode 宽松解析**：截断输入（`"i42"` 缺 `'e'`）静默返
  回假值、`stoll` 宽松接受空白/`+`、越界抛裸 `out_of_range`——
  `get_file_info` 纯模式 .torrent 路径真实使用这套内嵌解析器。
  三处严格化：容器/整数截断显式 throw、整数内容校验（可选负号 +
  全数字）、`stoll` 包 try/catch 转 runtime_error
- **DHT 僵尸客户端**：`DhtClient::start()` bind 失败只记日志不抛
  异常（`running_=false`、`socket_=-1`），`startDht` 却照常持有
  客户端——`isDhtRunning()` 撒谎、`findPeers` 的查找无人驱动。
  `startDht` 现检查 `isRunning()`，失败即 reset + 错误日志；
  `dht_node.hpp` 补 `isRunning()` 访问器
- 死代码删除：`bencodeToString`/`sha1`/`getTrackers`/
  `generateNodeId`/`urlDecode` 全库零引用
- 测试：`bittorrent_plugin_test.cpp` +32 用例（提取 6/归一化 7/
  parse 严格化 8/validateTorrent 契约钉死 3/DHT 生命周期与端口冲
  突 2/下载生命周期 6；Base32 向量经 Python `base64.b32encode`
  独立生成；DHT 用 `DhtClient(0)` 随机端口 + `HeldUdpPort` 占口
  测冲突）
- 覆盖率：bittorrent_plugin.cpp gcov miss **187 → 7**（行
  95.72%）；剩余 7 行为 resume 路径 DHT 重启组合分支、findPeers
  回调推进 peers（需真实 P2P 网络）、parseBencode 入口防御 throw
- **测量教训**：改源码后必须全量重建（daemon/CLI 等未重建二进制
  内嵌旧 checksum 对象，全量 ctest 中对 gcda 执行 checksum 覆盖
  = 整体替换非合并，已测行为覆盖数据被清掉——表现为全绿测试但
  can_handle 显示未覆盖）；修复 = 全量重建 → 清全部 gcda → 重跑

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

BitTorrent/Magnet 协议插件，**纯 C++ 实现**（`FALCON_USE_LIBTORRENT`
存在但生产未启用），支持：

- .torrent 文件解析（内嵌 B 编码解析器，`get_file_info` 纯模式路径）
- Magnet 链接（hex / Base32 info-hash 归一化，`extract_info_hash`/
  `info_hash_to_hex` 公开 static 回归挂点）
- DHT 客户端（Kademlia 迭代查找，见 dht_node）
- PEX 扩展协议（见 pex_protocol）
- 下载生命周期（pause/resume/cancel，DHT 随启停）

**测试要点**：DHT 用 `DhtClient(0)` 随机端口绕开 6881 争用，
`clearDhtBootstrapNodes()` 清公网引导节点后空网络查找立即收敛；
手写 Base32 向量用 Python `base64.b32encode` 独立验证。

---

## 依赖

- 无强制外部依赖（纯 C++ 模式）；libtorrent-rasterbar 2.0+ 可选

---

**文档维护**：实现完成后更新本文档。
