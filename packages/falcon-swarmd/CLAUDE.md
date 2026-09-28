[根目录](../../CLAUDE.md) > [packages](../) > **falcon-swarmd**

---

# falcon-swarmd

P2SP 共享网络的 **Rendezvous Service**（会合/发现服务，俗称 tracker）与节点侧客户端（设计文档 `docs/p2sp_network_design.md` §12 阶段 0）。单一包内三个静态库 + 一个可执行：服务端与 client 共享 `swarm_protocol` 单一事实源，回环测试一处覆盖两端。

## 变更记录 (Changelog)

### 2026-09-28 - 传输层边界收口（swarm_rpc_server miss 96 → 22，21 新用例）+ 注入点零消费更正
- **21 新用例三套件**挂 `falcon_swarm_loopback_tests`（29 → 50，全量 ctest 绿）：
  - **SwarmRpcTransport 10**（裸 socket `raw_exchange`，drop 剧本必须半关写端 `SHUT_WR`/`SD_SEND`——否则与服务器 recv 互等只能靠 SO_RCVTIMEO 兜底）：HTTP 解析失败族（缺 CL → -32700 / 头区超限 / 半截头 / 垃圾请求行 / body 超限 / 短 body 全部静默断连 + 头值 trim 无语义）/ Bearer 边界（"Bearer" 恰等前缀长、Basic 非 Bearer scheme → 401）/ 信封非 object → -32600
  - **SwarmRpcLifecycle 5**：start() 重入幂等 / `SwarmRdvSocket`·`SwarmRdvListen` 注入 → false + last_error 精确串 + 解除后同实例重试成功 / 非法 bind host / 端口冲突
  - **SwarmRpcWs 6**：非 /jsonrpc 升级 404 / **TEXT·BINARY JSON-RPC 请求-响应（WS 请求路径此前从未被测**）/ CLOSE 回显断开 / 坏操作码 CLOSE 1002 / PONG 忽略后会话存活
- **更正**：下表曾称「注入点（socket/listen/EVP 四点）已测」——`SwarmRdvSocket`/`SwarmRdvListen` 实为全测试目录零消费（真被测的只有 swarm_crypto EVP 四点），本批为前两者首个消费方
- **定性跳过 15 行 `#####`（证据在测试文件头）**：RST 微秒窗竞速 3（222/564/722）+ accept 竞速 1（428）+ 结构不可达 5（633 表查无 / 749 恒成功 / 252-253 default）+ IPv6 分支 757-764（服务器 AF_INET-only）+ 行归属伪影 2（498/557 带计数）；`=====` 7 行 = 多实例共享行部分执行伪影
- gcov 双口径：单对象 ##### 75 → 15 恰为跳过集合；gcovr XML 单文件 96 → 22

### 2026-09-27 - 服务更名 Rendezvous Service（用户拍板术语统一）
- 文档/目标/文件/符号统一：CMake target `falcon_swarm_server` → `falcon_swarm_rdv`；`src/server/` → `src/rdv/`；`SwarmServerState/SwarmServerOptions/SwarmServerHarness/SwarmRpcServer` → `SwarmRendezvousState/SwarmRendezvousOptions/SwarmRendezvousHarness/SwarmRendezvousServer`；`swarm_server_state.*` → `swarm_rdv_state.*`、`swarm_server_loopback_test.cpp` → `swarm_rdv_loopback_test.cpp`、`swarm_server_harness.hpp` → `swarm_rdv_harness.hpp`；注入点 `SwarmServerSocket/Listen` → `SwarmRdvSocket/Listen`（libfalcon-core）
- 保留项（技术事实/线协议契约，不为改而改）：`server_token`（config 键 + CLI `--server-token`）、`kFieldServerTime`/"server_time"（线字段）、`swarm_rpc_server.{hpp,cpp}` 文件名（传输层命名，沿 daemon `json_rpc_server` 形制）、`SwarmRendezvousServer` 类名中的 Server（HTTP/WS 服务器角色语义）、中文注释里的"服务器"角色词

### 2026-09-25 - P2SP 阶段 0 落地（三批提交：基建拆库 8d5e366 / server 4c351a9 / client+e2e 50b42bd）
- 实现设计文档 §12 阶段 0 全范围：`falcon-swarmd` 单二进制（两步注册挑战/心跳/查询/WS 通知 + 双限频器 + Bearer 准入）+ 节点侧 `SwarmClient`（注册两步状态机/心跳自愈/查询/退订，无公告——用户裁决「空表往返」）
- 70 用例四 target 全绿；ASan 70/70 零告警
### 2026-09-25 - 批 4 收尾（补矿 + MSVC 编译面收口 + 三份文档 + 铁账）
- **swarmd_config 补矿**：config 编译单元从可执行移入 `falcon_swarm_server`（+PRIVATE `falcon_daemon_core` 链接——`get_default_config_dir` 符号依赖），新增 `tests/swarm_config_test.cpp` 14 用例挂 unit_tests（全字段往返/缺键保留默认/六类错误路径 can't-open·parse·root·两节非 object·两键类型错/未知节与未知键告警/路径键 ~/ 展开/expand_home_path 直测含 "~"、"~user" 非语义形态原样返回）——对位 daemon config 10 用例矩阵，配置解析纯逻辑不留 e2e 覆盖
- **MSVC 编译面两处收口**（批 3 run 36076043736 Qt6 Windows job 曝光；NOMINMAX 修复已生效，C2589 未再现）：① `swarm_client.cpp` chrono rep（MSVC long long）brace-init 到 Options::timeout_ms(long) 窄化 C2397 → 显式 static_cast（macOS tv_usec 教训的 MSVC 版）；② `swarm_ws_subscriber.cpp` POSIX `::poll` C3861 → Windows 分支 `#define poll WSAPoll`（winsock2.h 同语义，pollfd/POLLOUT 已在用）
- 用例总账 70 → **84**（unit 28→42；`ctest -R Swarm` 子串匹配只命中 71——13 个新用例名不含 "Swarm" 子串，全集按 84 记）

## Target 与依赖

| Target | 内容 | 链接 |
|---|---|---|
| `falcon_swarm_common` | 线协议常量/canonical JSON/签名 payload/Ed25519/SHA-256 | core + nlohmann + OpenSSL::Crypto + falcon_ws_protocol |
| `falcon_swarm_rdv` | SwarmRendezvousState/RpcHandlers/RateLimiter/传输层（swarm_rpc_server）+ swarmd_config | common + ws_protocol + daemon_core(PRIVATE) |
| `falcon_swarm_client` | SwarmHttpClient/SwarmWsSubscriber/SwarmKeyStore/SwarmClient | common + ws_protocol + CURL |
| `falcon-swarmd`（可执行） | main（daemonize/信号停机） | rendezvous（falcon_swarm_rdv）+ falcon_daemon_core |

包级守卫链：`falcon_ws_protocol`/`falcon_daemon_core`（daemon 包拆出）缺失 → WARNING + return()；OpenSSL 缺 `OpenSSL::Crypto` → 同上（编译期整体跳过，对齐 storage 云浏览器先例）；CURL 缺失只跳 client（rendezvous 面单独可用）。`FALCON_BUILD_SWARMD` 默认 ON。

## 线协议（JSON-RPC 2.0，HTTP POST /jsonrpc + WS 通知）

方法：`falcon.swarm.register`（两步：身份 params → 同 params + challenge_sig）/ `heartbeat` / `query` / `unsubscribe`。`challenge`/`announce`/`retract` 常量已定义，阶段 0 无服务端实现（announce 为阶段 1 写入路径）。

通知（WS 帧，**params 为 object**，与 daemon 的数组式不同）：`falcon.swarm.onPeerJoined`（首次出现 node_id 才广播）/ `falcon.swarm.onPeerLeft`（心跳超时 sweep 摘除）。

错误码：`-32001` Bearer 准入（HTTP 401）/ `-32002` 限频（HTTP 429 同发）/ `-32003` 未知或过期 session / `-32004` 群组令牌 / `-32005` 签名/指纹/黑名单拒绝。

注册绑定（§9.2）：step1 快照 `canonical_json(params)`（键序排序 + 紧凑 dump），step2 要求去 `challenge_sig` 后 canonical 逐字节一致（nonce/node_id 绑定 + 防参数偷换）；challenge 单次有效 + TTL。签名 payload = `method + "\n" + nonce + "\n" + sha256_hex(params_json)`，Ed25519 一步式 EVP_DigestSign/Verify（md=NULL）。

## 编码定案

- pubkey = DER(SPKI) 小写 hex（88 字符）；签名 = hex（128 字符）；nonce = hex（16 字符）
- challenge / session = RAND_bytes(16) → hex（32 字符，session 带 `s-` 前缀）
- 节点指纹（node_id）= sha256_hex(DER) 前 32 字符
- 私钥落盘 = PEM（PKCS#8 未加密），POSIX 收权 0600；文件已存在但解析失败报错返回（绝不静默覆盖——私钥即节点身份）

## 关键设计

- **SwarmRendezvousState 单锁**：peer/session/challenge/资源表一把 mutex；通知一律锁外（锁内摘除 + 攒列表）；`now` 全部显式入参（状态类零时钟依赖——测试推进虚拟时钟确定性收口）
- **限频器**：per-key 时间戳窗口（`map<string, deque<time_point>>`），`allow(key, now)` now 入参；`max_events==0` = 不限；实例化 register-per-IP 与 query-per-IP 两个
- **传输层独立瘦实现**（方案 B）：零 daemon 源改动，socket/WS 会话/广播/停机模式沿 `json_rpc_server.cpp` 形态换命名空间；WS 帧协议复用 `falcon_ws_protocol`（第三消费方）
- **准入分叉**：swarmd 用 `Authorization: Bearer`（daemon JSON-RPC 用 `token:` 首参——两者不兼容，测试钉死）
- **start() 内 POSIX `signal(SIGPIPE, SIG_IGN)`** + Windows call_once Winsock；WS 服务器停机先 shutdown 全部 fd 再 join（daemon 先例）
- **SwarmClient 心跳自愈**：心跳线程对 `-32003` 自动重注册换新 session（Rendezvous sweep 摘除后节点无感恢复）；`detach()` = 停心跳保注册（模拟进程崩溃，e2e 心跳超时用例的客户端侧入口）
- 阶段 0 **无 announce 面**：资源表无写入路径恒空；query 合法 session → sha256 回显 + 空 sources（「空表往返」用户裁决）；`SwarmResource` 结构 + 表 + 读取逻辑在位（query 真实消费），阶段 1 加 upsert 即通

## 测试（105 用例四 target）

| Target | 数 | 覆盖 |
|---|---|---|
| `falcon_swarm_unit_tests` | 42 | canonical JSON/签名 payload 对拍（RFC 8032 TEST1/TEST2 向量 + 指纹推导）/限频器虚拟时钟/状态过期注入 now/密钥往返/**swarm.json 配置 14 用例**（往返/错误路径六类/告警/~/默认保留） |
| `falcon_swarm_loopback_tests` | 50 | 真 socket 回环：HTTP/WS 握手/通知帧形制/错误路径全集（-32001..-32005、-32600/1/2、429）/限频/注入点（socket/listen 两点 2026-09-28 起真测，EVP 四点在 crypto 测试）/**传输层边界 21 用例**（HTTP 解析失败族/Bearer 边界/start() 生命周期/WS 帧协议面） |
| `falcon_swarm_client_tests` | 10 | SwarmClient × 回环 Rendezvous：注册往返/心跳存活/退订摘除/会话过期自愈/空表查询/传输失败/错令牌双路径/密钥往返/0600 |
| `falcon_swarm_e2e_tests` | 3 | 真二进制 fork+execv（POSIX-only）：全流程含通知到达、心跳超时摘除、坏配置非零退出 + SIGTERM exit 0（gcda 铁律） |

防「同一 bug 自我印证」：回环测试侧用 SwarmCrypto 独立重导 canonical→payload→verify（s3_browser_auth_test 惯例）；RFC 向量独立生成。

## 配置（swarm.json `swarm` 节 + CLI）

host/port（7800，0=随机）/server_token（空=不鉴权+启动告警）/group_token（空=不校验）/heartbeat_interval_s（60）/heartbeat_timeout_s（180）/challenge_ttl_s（60）/sweep_interval_ms（1000）/rate_register_per_min（5）/rate_query_per_min（120）/blacklist。`daemon` 节四键复用 `falcon_daemon_core`。CLI `--swarm-host/--swarm-port/--server-token/--group-token/--heartbeat-*-s/--challenge-ttl-s/--sweep-interval-ms/--rate-*-per-min` + daemonize 三键。优先级 CLI > 文件 > 默认；类型错误 daemonize 前报错退出；阶段 0 无 SIGHUP 热更。
