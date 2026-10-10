# Falcon 节点互发现与资源共享 Rendezvous Service 设计(P2SP 共享网络)

> [!NOTE]
> 本文档为设计材料,全部内容尚未进入当前公开 API。本文承接 `todo.md` 中
> 「P2SP 共享网络路线讨论(立项备忘)」的路线论证,将其推进为可指导实现的设计。

> **状态**:阶段 0 已实现(2026-09-25,`falcon-swarmd` 包,见该包 CLAUDE.md);
> **阶段 1 已实现**(2026-10-09,§16,daemon 公告/查询/RPC/e2e 铁律);
> **阶段 2 已实现**(2026-10-10,§17,数据服务 + 查询注入镜像池 + NAT 过滤);
> 阶段 3 未开工。

## 1. 问题陈述与目标

Falcon 今天已有的能力(2026-09 现状):

| 能力 | 现状 | 与本设计的关联 |
|------|------|---------------|
| V2 引擎多源分段(P2SP) | metalink 阶段 2 落地,同一文件多镜像段级换源 | **数据面已具备"从多个来源拉同一文件"的全部机制** |
| daemon aria2 兼容 RPC | 28 方法,HTTP + WebSocket 同端口,自实现零新依赖 | Rendezvous 与节点控制面的技术底座候选 |
| metalink | RFC 5854/Metalink3 双兼容,整文件哈希校验后才发布成品 | "哈希带外已知"的下载形态,天然支持下载中共享 |
| BT/DHT | libtorrent 数据面 + 自行开发 DhtClient(Kademlia 迭代查找) | 检验过"对等网络"的全部工程约束(见 §4 选型) |
| 任务持久化 | SQLite,停机恢复 | 节点侧公告状态机的持久化惯例参考 |

**缺的是最后一环**:多源分段目前只能从 metalink 文档或单一下载任务的 URL 得到"同一内容的多路来源"。用户的真实场景是拥有一群 falcon 节点——家庭 NAS、桌面机、云机——它们各自下载文件,却互不知道对方手里有什么:

- 桌面机已下载完成的 4GB 文件,NAS 再要时只能从头从原始镜像下载;
- 多台机器各自维护自己的"这个 URL 哪里能下"的知识,无法互通;
- 云机有公网带宽,NAS 在局域网,二者不能互为镜像源加速。

**目标**:设计一个自托管的 Rendezvous Service(俗称 tracker,索引协调面),让用户自己的 falcon 节点互相发现、互为下载来源,并把"已完成文件 / 可用镜像 / 任务元数据"三类资源共享给圈内节点。数据面(文件字节)始终节点直连,**Rendezvous 不中继内容**。

**非目标**(论证见 §3、§13):公共基础设施、全局内容搜索、匿名参与、任何形式的积分激励。

---

## 2. 决策总览

本文以 D1-D10 编号决策贯穿,每处给出"为什么"。汇总表见 §15。

| 编号 | 决策 |
|------|------|
| D1 | 网络形态 = 自托管私有 swarm:自架 Rendezvous(索引面)+ 节点直连(数据面) |
| D2 | 节点身份 = 持久化 Ed25519 密钥对,节点 ID = 公钥指纹 |
| D3 | 发现机制 = 中心 Rendezvous Service 为权威面;mDNS 局域网发现延后;否决 DHT 路线 |
| D4 | NAT 穿透 = 阶段一不做,以部署形态约束 + "拉取方主动连接"模型覆盖 |
| D5 | 共享面 = 已完成文件 / 镜像 URL 列表 / 任务元数据 三类;分片级共享延后 |
| D6 | 内容寻址与完整性 = SHA-256 全文件哈希,下载后 `verify_streaming` 校验 |
| D7 | Rendezvous 形态 = 独立单二进制,JSON-RPC over HTTP+WS,索引即离场 |
| D8 | 线协议 = JSON-RPC 2.0,`falcon.swarm.*` 方法族 + WS 事件通知 |
| D9 | 安全 = TLS + 指纹 pinning、挑战-签名认证、限频/配额/吊销、共享默认关 |
| D10 | 节点集成挂点 = TaskManager 层完成事件(非引擎层),CLI/config/daemon/RPC 四面配置 |

---

## 3. 总体形态(D1):自托管私有 swarm

### 3.1 形态定义

```
                    ┌─────────────────────┐
                    │  falcon-swarmd      │  用户自架(云机/NAS 任一)
                    │ Rendezvous Service  │  索引面:注册/目录/公告/通知
                    └───────┬─────────────┘
              注册/心跳/公告/查询(TLS)
           ┌────────────┼────────────────┐
           ▼            ▼                ▼
      ┌─────────┐  ┌─────────┐      ┌─────────┐
      │ NAS     │  │ 桌面机   │      │ 云机     │
      │ falcon  │  │ falcon  │      │ falcon  │
      └────┬────┘  └────┬────┘      └────┬────┘
           │            │                │
           └───── 数据面:HTTP 直连拉取 ────┘
                 (V2 P2SP 分段语义,Rendezvous 不经手)
```

- **索引面**:节点向 Rendezvous 注册身份、公告资源、查询资源、订阅变更通知。
- **数据面**:节点 A 需要文件 X(sha256 已知)→ 从 Rendezvous 查得"节点 B、C 持有 X"→ A 直接向 B、C 的内嵌数据服务发起 HTTP Range 拉取,V2 引擎按既有 P2SP 语义分段、换源、校验。

### 3.2 为什么是"自托管私有",而不是公共网络

todo.md 立项备忘已对路线 C(中央索引)定性:需要服务器基建 + 运营 + 内容责任法务,"超出写代码范畴"。这个定性针对的是**公共**中央索引——面向不特定公众的内容分发索引,迅雷与电驴的被诉史说明这是产品生死级风险。自托管把三个问题同时消解:

1. **运营主体**:Rendezvous 是用户自己的一台机器(一次性二进制,类似 `aria2c` 的使用门槛),没有平台运营方。
2. **内容责任**:共享圈是自己的机器群(家人 NAS + 自己的云机),节点间直接分享自有文件;Rendezvous 只存"谁的哪个 sha256 在哪个地址"的索引元数据,不存内容,不提供搜索。
3. **冷启动**:公共网络的冷启动是产品问题(没人就没速度,备忘原文);私有网络**天然没有冷启动**——节点就是自己的机器,第一天就有完整拓扑。

同时,"默认关"原则(§9.5)保证:不开共享的 falcon 用户,行为与今天逐字节一致;共享面即使开启,也只在用户明确配置的 Rendezvous 与群组内可见。

### 3.3 为什么数据面不走 Rendezvous 中继

Rendezvous 中继内容(TURN 式)会把自托管 Rendezvous 的带宽变成全网瓶颈,并把 Rendezvous 从"索引服务"升级成"内容分发服务"——恰好退回 3.2 要避开的法务形态。索引即离场:Rendezvous 的响应里只有地址,字节只在节点之间流动。中继的例外出口见 §13(延后项)。

---

## 4. 发现机制选型(D3)

候选四路,todo.md 备忘的路线 A/B/C 对应其中三路。本节用仓库现状实证各路的真实成本。

### 4.1 方案对比

#### 方案一:中心 Rendezvous Service(选定)

**优点**:
- 确定性:查询即应答,无迭代收敛延迟;节点数 <20 的私有圈子里,DHT 的 O(log N) 扩展性优势不存在,一个 HTTPS 请求直达优于多跳 UDP。
- 可管理性:认证、限频、配额、吊销都在 Rendezvous 一处落地(§9);DHT 里做不到"踢掉一个节点"。
- 离线语义自然:节点下线后公告可带 TTL 存活;上线通知经 WS 推送即时到达。
- 与 daemon 技术栈同源:daemon 已有自实现 HTTP+WS JSON-RPC 服务器(`packages/falcon-daemon/src/rpc/json_rpc_server.*`,含 WS 帧协议、token 认证、getsockname 端口回读先例),Rendezvous 直接复用同一族栈,**零新依赖**。

**缺点**:
- 单点:Rendezvous 挂了共享面失效(§12 降级语义:下载不受任何影响)。
- 需要用户架一个服务(门槛与自建 aria2 RPC 相当)。

#### 方案二:公共 BT DHT 桥接(备忘路线 A)——否决

合成 infohash(成品 SHA-1 加魔数前缀派生)公告进公共 DHT,零基建。**否决,四个理由,前两个有仓库实证**:

1. **实现成本被现状推翻**。备忘预估"最省数据面",但对自行开发 `DhtClient` 的现状调查(2026-09)表明:`announce_peer` 是零实现(仅 `DhtQueryType::AnnouncePeer=3` 枚举与字符串映射占位,`dht_node.cpp` 中 token 零命中——不捕获、不存储、不回传);无入站查询应答(`handleMessage` 只处理 Response,`y=q` 一律忽略,即没有 infohash→peers 存储表与服务端能力);BEP-5 线格式三处偏差(id 写在顶层 dict 而非 `a`/`r` 内、decode 的 `r` 只吸收字符串值导致真实 `values` 列表被整体丢弃、`arguments` 的 `map<string,string>` 装不下 `implied_port` 整数)。对接公共 DHT = 先补齐公告发送 + token 流程 + 入站应答 + wire 修正四大块新建面,并逐一通过 MockDhtNode 与真实节点互通验证。
2. **公网暴露面**。合成 infohash 的魔数前缀防的是意外碰撞,防不了主动扫描:任何第三方都能枚举 DHT 收集"谁持有哪个 sha256",把私有下载行为公告进公共可观测空间。
3. **NAT 失效**。NAT 后节点公告的 ip:port 对公网不可达,公共 DHT 无法辅助打洞(§4.2)——公告了个寂寞,这是结构性缺陷不是调优问题。
4. **公共 DHT 的治理缺位**。无认证(任何人可就任意 infohash 应答假地址)、无吊销、无限频,污染与滥用防护只能靠客户端侧校验兜底。

#### 方案三:Falcon 私有 DHT(备忘路线 B)——否决

同一套 Kademlia 换引导节点。复用方案二否决理由 1 的全部新建面(私有 DHT 同样需要 announce/token/入站应答),再加:

- **冷启动在私有形态下无解**:私有圈节点数个位数,DHT 查找的多跳迭代纯增延迟(bootstrap → get_peers 迭代 → 收敛,秒级;HTTPS 查询毫秒级)。
- 引导节点就是另一个要维护的常驻服务——复杂度不降反升。
- 自行开发 DHT 的维护线程每 5 分钟随机 find_node(既有行为),私有网络里是无意义流量。

**DHT 能力不浪费**:自行开发 DhtClient 服务于 BT 任务(libtorrent 模式之外的基础设施),与本设计正交。若未来进入公共 P2P 形态,DHT 路线可重新评估。

#### 方案四:mDNS(局域网自动发现)——延后,列为可选加速层

- 仅局域网内有效,跨网段/跨网(NAS 在家、云机在公网)失效,不能作为权威面。
- 跨平台实现差异大(Avahi/Bonjour/自实现组播),与"三平台自包含测试基建"的仓库惯例冲突(同类教训:SFTP 因无法回环 mock 被阻塞)。
- **延后触发条件**(§13):用户实测局域网场景下"Rendezvous 不可达但同网段"的发现需求真实存在,再以自实现组播(mDNS 文本协议,无 Avahi 依赖)评估。

### 4.2 选型结论

**中心 Rendezvous Service 为唯一权威发现面**;mDNS 作为局域网加速层延后;DHT 路线(公共/私有)双双否决。发现的产出是"资源 → 持有节点集合(含可达地址与 NAT 标记)",供 §6 的共享模型与 §10 的引擎集成消费。

---

## 5. NAT 穿透(D4):阶段一明确不做

### 5.1 现状约束

仓库现状(2026-09 调查):全库无 getifaddrs / SIOCGIFADDR / GetAdaptersAddresses——**本机对外 IP 枚举没有任何现成设施**;UPnP/NAT-PMP 仅在 libtorrent 分支被显式关闭;可达地址目前只有配置注入一条路(daemon `--rpc-listen-host`,默认 127.0.0.1 不可对外)。STUN/打洞为零基础。

### 5.2 阶段一模型:"拉取方主动连接" + 部署形态约束

不做穿透,不等于 NAT 后节点完全不可用。关键观察:**数据面连接方向是自由的**——A 拉取 B 不需要 B 能主动连 A,只需要 A 能发起一条到 B 的 TCP。由此阶段一的可达性矩阵:

| 部署形态 | 作为数据源 | 作为拉取方 | 说明 |
|---------|-----------|-----------|------|
| 同网段(NAS+桌面) | 可 | 可 | 私网地址互通,无 NAT 隔离 |
| 公网机器(云机) | 可 | 可 | 公知地址直连 |
| NAT 后(家用宽带) | 默认不可 | 可 | 默认只发起不接收 |
| NAT 后 + 用户做端口映射/配置公知地址 | 可 | 可 | 显式配置 `advertise_address` |

落地规则:

1. 节点注册/心跳经出站 TLS 到 Rendezvous,**Rendezvous 从连接源地址观察到节点出口 IP**,但源地址对数据面**默认不可信**(NAT 共享出口)。
2. 节点公告的数据服务地址 = 显式配置的 `advertise_address`(ip:port),未配置则公告 `direct=false`(仅可发起、不可被拉取)。
3. 查询方对 `direct=false` 的节点**不作为 P2SP 数据源**,仍可作为"镜像 URL 池"的成员(镜像拉取方向由节点自己发起,天然可穿 NAT)。
4. Rendezvous 对 `direct=false` 节点打 NAT 标记,供查询方过滤(§8 消息字段)。

这个模型覆盖家庭场景的两个主要角色:NAS(通常端口映射或同网段访问)与桌面机(拉取方)。**覆盖不了的**:两个都在对称 NAT 后且都不愿做映射——它们互相不作为数据源,仍共享镜像 URL 与元数据(价值保留大半)。

### 5.3 延后项与触发条件

| 技术 | 解决什么 | 不现在做的理由 | 触发条件 |
|------|---------|---------------|---------|
| UPnP-PCP / NAT-PMP | NAT 后节点自动获得公网端口 | 路由器兼容性长尾;需本机地址枚举设施先行 | 用户实测端口映射摩擦显著 |
| STUN + UDP/TCP 打洞 | 对称 NAT 之外的穿透 | Rendezvous 协调打洞是新协议面;成功率取决于 NAT 类型,无法承诺 | UPnP 落地后仍有强需求 |
| TURN 中继 | 兜底一切不可达 | 违背"索引即离场"(§3.3),Rendezvous 带宽成本回到平台形态 | 仅评估,不承诺 |

---

## 6. 资源共享模型(D5/D6)

### 6.1 共享面界定:三类资源

**共享什么**由 P2SP 数据面的需求反推——多源分段需要"同一内容的多个来源",来源分三等:

| 资源类 | 内容 | 消费方式 | 完整性保障 |
|--------|------|---------|-----------|
| **R1 成品文件** | 节点已下载完成的文件:sha256、文件名、尺寸、持有节点(含数据服务端口) | 作为 P2SP 数据源(§10.3 内嵌 HTTP 服务) | 下载后 SHA-256 校验(§6.3) |
| **R2 镜像 URL** | 节点下载成功时记录的原始/最终 URL(URL、ETag、Last-Modified) | 注入 metalink 镜像池语义的候选源 | 由 URL 侧语义决定(metalink 哈希/既有校验链) |
| **R3 任务元数据** | 文件名、尺寸、ETag、Last-Modified、Accept-Ranges 能力 | 辅助:If-Range 续传对接、下载前元信息预览、同名去重 | 信息性,不参与校验 |

**不共享什么**(隐私与法律边界,论证见 §9.4):

- 路径:只共享文件名,**不共享目录结构**(本地路径是隐私,也泄漏目录布局)。
- 下载历史:只公告"当前持有的成品",不公告"曾经下载过什么"(历史由节点本地 SQLite 独享)。
- 未完成任务:分片级共享(下载中互传)延后(§13)——普通 HTTP 下载中无法承诺内容完整性(哈希未知,§6.4),metalink 任务例外但第一阶段不为它特化分片协议。

### 6.2 内容寻址

R1 资源以 **SHA-256 hex** 为主键。选 SHA-256 而非 SHA-1:

- 与仓库主流一致:metalink 整文件校验、增量下载、S3 SigV4 载荷哈希均以 SHA-256 为主;
- `FileHasher::calculate_streaming(path, HashAlgorithm::SHA256)`(EVP 分块 256KB,`packages/libfalcon-protocols/src/file_hash.cpp:65`)现成,GB 级文件流式不读全内存;
- SHA-1 仅保留给 BT infohash 兼容面,不引入新的依赖 SHA-1 的面。

公告主键与查询主键都是 sha256;文件名只是展示与落盘建议(同一 sha256 在不同节点可不同名)。

### 6.3 完整性校验:消费端强制

从 swarm 源拉取的字节**与从任何镜像拉取的字节同等对待**,进入既有校验链,零特权:

- metalink 任务:成品过既有 `verify_or_discard` 语义(metalink_handler.cpp 的哈希校验→rename→Completed 规约),失败回落串行镜像(既有行为);
- 普通任务 + 旁车哈希(§6.4):下载完成后 `FileHasher::verify_streaming` 校验,失败按任务既有失败语义收口;
- 无哈希可校验的普通任务:R1 源**不参与**(§6.4 的哈希来源规则保证"swarm 源必有 sha256 可校验"——公告本身就是哈希驱动的)。

这条规则的价值:swarm 不引入新的信任面。坏节点给错字节的下场与坏镜像给错字节完全一致——校验失败、源被剔除、回落,成品永远经过校验才发布(既有"完成=可信"不变式)。

### 6.4 哈希来源(D6 的另一半):什么时候"查得到"

核心张力:**普通 HTTP 下载在完成前不知道成品哈希**(无带外哈希),所以"下载中查询 swarm"对普通任务天然不成立。三轨并存:

1. **完成后公告(主轨,覆盖一切任务)**:任务 Completed → 后台对成品 `calculate_streaming(SHA256)` → 公告 R1。哈希派生是后台动作,不阻塞完成回调(GB 级文件数十秒的哈希耗时只推迟公告,不影响下载);
2. **旁车探测(增强轨,覆盖"下载前")**:URL 旁存在 `.sha256`/`.sha1` 旁车文件(社区惯例,`file.bin` ↔ `file.bin.sha256`)时,任务启动前探测旁车取得哈希 → 全程可查、可下载中注入 swarm 源;
3. **metalink(天然轨)**:文档自带整文件哈希,任务全程可查——**第一阶段唯一支持"下载中查询"的形态**。

规则表:

| 任务形态 | 下载中查询注入 | 完成后公告 |
|---------|---------------|-----------|
| metalink | 可(哈希带外已知) | 可(校验已有哈希,无需重算) |
| 普通 + 旁车存在 | 可 | 可 |
| 普通,无旁车 | 不可(只能从 Rendezvous 之外的传统源下载) | 可(完成后派生并公告) |

### 6.5 权限与隐私边界

- **群组制**:Rendezvous 上按群组(至少一个,如 `family`)组织节点;公告/查询都携带群组令牌(§9.3),跨群组不可见。多群组支持(一个节点入多个圈子)延后。
- **文件名可见性**:默认共享文件名;提供 `share.hash_only` 配置,R1 公告隐去文件名(消费方以 sha256 落盘,文件名自定)。
- **共享开关层级**:全局 `--p2sp-share`(默认 **off**)→ 群组级(Rendezvous 侧吊销节点即退出)→ 无单文件开关(阶段一;单文件粒度留待真实需求)。
- **默认关**:与 todo.md 备忘"上传策略默认关闭(吸取迅雷舆论教训)"同一决策。共享开启 = 用户显式动作,且开关同时控制"公告"与"被拉取"两侧(不存在"只上传不下公告"的中间态,语义简单可审计)。

---

## 7. Rendezvous Service 职责与 API(D7)

### 7.1 形态:独立单二进制 `falcon-swarmd`(暂名)

- 单个可执行文件,配置文件 `swarm.json`(对齐 daemon.json 惯例:CLI > 文件 > 默认值),一键自托管;
- 技术底座复用 daemon 的自实现栈:HTTP + WebSocket 同端口、JSON-RPC 2.0 分发、`token:` 认证、WS 帧协议(`websocket_frame.{hpp,cpp}`)、SQLite 可选落库——**零新依赖**;
- 为什么不复用 falcon-daemon 进程:职责不同(daemon 是"我的下载",swarmd 是"圈子的目录"),生命周期不同(daemon 重启不应晃动全群目录),部署位置不同(swarmd 常驻云机,daemon 在每台机器)。二者只共享协议族与库代码。

### 7.2 职责清单(五件,索引即离场)

1. **身份注册**:新节点注册(公钥 + 签名证明),登记节点指纹与群组成员关系;
2. **Peer 目录**:维护"节点 → 可达性(direct/NAT 标记/last_seen/版本)"活性表;
3. **资源索引**:R1/R2 资源公告的存储与 TTL 过期;按 sha256 前缀/文件名查询;
4. **活性心跳**:心跳超时摘除节点,连带摘除其公告(持有者不在,公告无意义);
5. **变更通知**:WS 推送资源新增/过期、节点上下线(消费方即时刷新镜像池)。

**明确不做**(对齐 §3.3):字节中继、内容缓存、搜索排名、社交图谱。

### 7.3 API 面(JSON-RPC 2.0,HTTP 与 WS 双承载)

与 daemon 的 aria2.* 方法族并列同族,Rendezvous 侧方法前缀 `falcon.swarm.`:

```
POST /jsonrpc            # 与 daemon 同路径惯例;WS 升级亦在 /jsonrpc
Authorization: Bearer <server_token>     # 传输层准入(区别于节点身份层)
```

| 方法 | 方向 | 作用 |
|------|------|------|
| `falcon.swarm.register` | 节点→Rendezvous | 注册/重注册(密钥身份 + 群组令牌 + 可达性) |
| `falcon.swarm.challenge` | Rendezvous→节点 | 注册时的签名挑战(§9.2) |
| `falcon.swarm.heartbeat` | 节点→Rendezvous | 活性续租(60s 间隔,180s 超时) |
| `falcon.swarm.announce` | 节点→Rendezvous | 批量公告 R1/R2 资源(带 TTL) |
| `falcon.swarm.retract` | 节点→Rendezvous | 显式撤回资源(文件删除时) |
| `falcon.swarm.query` | 节点→Rendezvous | 按 sha256 / 文件名查询持有节点 |
| `falcon.swarm.unsubscribe` | 节点→Rendezvous | 优雅注销(清目录,通知在线节点) |

WS 通知(Rendezvous→节点,JSON-RPC notification 形态):

| 通知 | 触发 |
|------|------|
| `falcon.swarm.onResourceAdded` | 圈内新公告(增量,查询免轮询) |
| `falcon.swarm.onResourceExpired` | TTL 到期/持有者下线 |
| `falcon.swarm.onPeerJoined` / `onPeerLeft` | 节点上下线 |

HTTP GET 健康面:`GET /v1/health`(与桌面 IPC 的 `/v1/health` 惯例一致,无副作用探测)。

### 7.4 Rendezvous 存储模型

- 内存为主:节点表 + 资源表都是小圈规模(百级节点 × 千级资源),进程内 map 足够;
- 可选 SQLite 落库(对齐 daemon TaskStorage 惦例):Rendezvous 重启后目录保留,省去全群重公告风暴;资源表带 TTL 字段,加载时过期清扫;
- 无持久化配置时纯内存运行(重启后节点凭重注册机制自愈,§12)。

---

## 8. 线协议与消息格式(D8)

### 8.1 设计原则

- **JSON-RPC 2.0 全家桶**:请求/响应/notification 三形态与 daemon 完全同构,客户端(`WebSocketRpcClient` / JSON-RPC HTTP 客户端)与 Rendezvous 的分发层双侧都有现成代码;
- **签名覆盖规则**:对 `payload = method + "\n" + nonce + "\n" + sha256_hex(params_json)` 计算 Ed25519 签名,`params_json` 为紧凑 JSON(键序 = 本文档各示例的字段序,实现以 canonical 序列化函数固化)——简单、无歧义、易于两端对拍;
- 时间戳全部 UTC RFC 3339;哈希全部小写 hex;节点指纹 = SHA-256(公钥 DER) 前十六字节 hex。

### 8.2 注册与挑战(证明身份)

```jsonc
// ① 节点 → Rendezvous:falcon.swarm.register(请求)
{
  "jsonrpc": "2.0", "id": 1,
  "method": "falcon.swarm.register",
  "params": {
    "node_id": "9f86d081884c7d65",          // 节点指纹(16 字节 hex,32 字符)
    "pubkey": "MIIBIjANBg...",              // Ed25519 公钥(SPF base64 或 hex,实现定其一)
    "group_token": "fam-7f3a...",           // 群组准入令牌(Rendezvous 配置侧发放)
    "advertise": {                          // 数据服务可达性;不可达则整个字段缺省
      "addr": "192.168.1.10:7800",
      "direct": true
    },
    "agent": "falcon/0.2.0",
    "nonce": "b3d1a2..."
  }
}

// ② Rendezvous → 节点:挑战(要求证明持有私钥)
{ "jsonrpc": "2.0", "id": 1,
  "result": { "status": "challenge", "challenge": "5f2c9e..." } }

// ③ 节点 → Rendezvous:签名回执(重发 register,附 challenge 签名;params 同①)
{ "jsonrpc": "2.0", "id": 2,
  "method": "falcon.swarm.register",
  "params": { "challenge_sig": "3041bd..." } }

// ④ Rendezvous → 节点:注册结果
{ "jsonrpc": "2.0", "id": 2,
  "result": {
    "status": "ok",
    "session": "s-71cc...",                 // 后续心跳/公告凭此(Rendezvous 签发的会话凭据)
    "heartbeat_interval_s": 60,
    "server_time": "2026-09-22T12:00:00Z"
  } }
```

挑战-签名的意义(为什么不是"注册即信任"):注册报文里的 `node_id` 是自报的,挑战回执把"持有该指纹对应私钥"变成 Rendezvous 侧验证事实,杜绝 ID 抢注(§9.2)。

### 8.3 公告(批量,带 TTL)

```jsonc
// 节点 → Rendezvous:falcon.swarm.announce
{ "jsonrpc": "2.0", "id": 3,
  "method": "falcon.swarm.announce",
  "params": {
    "session": "s-71cc...",
    "sig": "9d0e71...",                     // 对 payload 签名(§8.1)
    "resources": [
      { "kind": "file",                     // R1 成品文件
        "sha256": "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "name": "ubuntu-24.04.iso",         // hash_only 模式下缺省
        "size": 5360366592,
        "ttl_s": 86400 },
      { "kind": "mirror",                   // R2 镜像 URL
        "url": "https://mirror.example.com/ubuntu-24.04.iso",
        "sha256": "e3b0c442...",            // 与 R1 同哈希即归并为同资源的多路来源
        "etag": "\"abc123\"", "last_modified": "2026-08-30T10:00:00Z",
        "accept_ranges": true,
        "ttl_s": 86400 }
    ]
  }
}

// Rendezvous → 节点
{ "jsonrpc": "2.0", "id": 3,
  "result": { "accepted": 2, "rejected": 0, "expires_at": "2026-09-23T12:00:00Z" } }
```

`kind: "mirror"` 携带同一 sha256 时,Rendezvous 端归并:资源 `e3b0c442...` = {持有节点 NAS(direct), 镜像 URL(...)}——查询方一次拿到 R1+R2 全部来源,正是 P2SP"多源"的语义。

### 8.4 查询(返回来源集合)

```jsonc
// 节点 → Rendezvous:falcon.swarm.query(按 sha256;文件名查询同理,fuzzy=false)
{ "jsonrpc": "2.0", "id": 4,
  "method": "falcon.swarm.query",
  "params": { "session": "s-71cc...", "sha256": "e3b0c442..." } }

// Rendezvous → 节点
{ "jsonrpc": "2.0", "id": 4,
  "result": {
    "sha256": "e3b0c442...", "name": "ubuntu-24.04.iso", "size": 5360366592,
    "sources": [
      { "type": "node",
        "node_id": "aa11...", "agent": "falcon/0.2.0",
        "addr": "192.168.1.10:7800", "direct": true,
        "last_seen": "2026-09-22T11:58:41Z" },
      { "type": "url",
        "url": "https://mirror.example.com/ubuntu-24.04.iso",
        "etag": "\"abc123\"", "last_modified": "2026-08-30T10:00:00Z" }
    ]
  }
}
```

`direct:false` 的 node 源由查询方过滤(§5.2 规则 3),协议层不替客户端做决策。

### 8.5 WS 通知(增量)

```jsonc
// Rendezvous → 节点(notification,无 id)
{ "jsonrpc": "2.0",
  "method": "falcon.swarm.onResourceAdded",
  "params": {
    "sha256": "bb97e3c9...", "name": "big-model.bin", "size": 12884901888,
    "by": "aa11...",                        // 公告者指纹
    "sources": [ { "type": "node", "node_id": "aa11...",
                   "addr": "192.168.1.10:7800", "direct": true } ] } }
```

---

## 9. 安全(D9)

### 9.1 威胁模型

私有圈子的对手画像,按现实优先级:

1. **圈外窃听/篡改**(网络路径上的第三方)——传输加密解决;
2. **身份冒充**(圈外节点伪装成圈内节点入库)——挑战-签名 + 群组令牌解决;
3. **圈内恶意/失陷节点**(假数据、资源耗尽)——完整性校验(§6.3)+ 限频配额 + 人工吊销解决;
4. **Rendezvous 失陷**(最坏面:目录全泄漏)——缓解:目录里只有 sha256/文件名/地址,无内容无路径;TLS pinning 防中间人替身 Rendezvous;
5. **明确不防**:圈内向信节点故意扩散自己拿到的资源(技术无解,信任模型边界)。

### 9.2 认证:挑战-签名(Ed25519)

- **为什么非对称**:pre-shared 对称令牌只能证明"是圈内成员",无法区分节点——不能按节点审计、限频、吊销,一个节点失陷 = 全群凭据失陷。Ed25519 私钥不出本机,Rendezvous 只存公钥;
- **为什么 Ed25519**:OpenSSL 1.1.0+ 的 EVP API 原生支持(`EVP_PKEY_ED25519`,项目依赖表 OpenSSL 1.1+ 成立);签名短(64B)验签快,适合心跳/公告的高频签;
- **挑战流程**:§8.2 的四步。nonce 由节点生成(防重放),challenge 由 Rendezvous 生成(防预计算),签名对象绑定 node_id 与 nonce;
- **ID 抢注防护**:指纹 = 公钥哈希,同一指纹必然同一密钥对;抢占已有 node_id 的注册因挑战签不出来而失败。

### 9.3 传输与会话

- **TLS 强制**:自托管场景无 CA,采用**自签证书 + 指纹 pinning**——节点配置 `server_fingerprint`(首次连接时人工确认录入,SSH known_hosts 同款信任模型);拒绝无 TLS 或指纹不符的 Rendezvous;
- **会话凭据**:注册成功签发 `session`(随机 128b+),心跳/公告/查询凭 session 免重复签名;session 与节点指纹绑定、TTL 180s 随心跳续;
- **群组令牌**:注册时验 `group_token`(Rendezvous 配置发放,静态串即可——圈子准入是一次性人工动作,不需要轮换协议)。

### 9.4 内容责任防线(产品设计级,重于一切密码学)

- 共享默认关(§6.5);开启是显式配置动作;
- **无搜索**:Rendezvous 只提供"按 sha256/精确文件名"查询,不做全文检索、不做浏览列表、不做热门推荐——目录天然不可被当作内容分发入口;
- 圈子隔离:群组间资源互不可见;Rendezvous 无"公共区"概念;
- Rendezvous 侧审计日志(谁在何时公告/查询了什么)默认开,自托管者可查。

### 9.5 防滥用(Rendezvous 侧行为约束)

| 手段 | 参数(默认) | 说明 |
|------|-------------|------|
| 注册限频 | 每 IP 5 次/分钟 | 防注册风暴 |
| 公告限频 | 每节点 60 请求/分钟、单次 ≤256 条 | 心跳期公告批量走 |
| 查询限频 | 每节点 120 请求/分钟 | 消费方缓存 + WS 增量后查询频次低 |
| 资源配额 | 每节点公告 ≤10k 条 | 防目录灌爆 |
| 吊销 | Rendezvous 配置 blacklist(node_id) | 立即摘节点与其全部公告,通知在线节点 `onPeerLeft` |

超限行为:HTTP 429 + JSON-RPC error(code -32002),节点侧指数退避(§12)。

### 9.6 节点侧数据服务加固(阶段二,预置约束)

内嵌 HTTP 数据服务(§10.3)只读、只认 sha256 路径(`GET /by-sha256/<hex>`),不暴露文件系统路径;请求校验群组成员性时以 Rendezvous 会话外带(简化:首阶段同网段/私网信任,资源路径不可枚举——sha256 路径本身就是 256b 能量;加固阶段支持 per-request token)。

---

## 10. 与现有 daemon/引擎的集成点(D10)

节点侧新增能力拆成四个模块,逐一指明挂点(2026-09 代码调查实证):

### 10.1 配置面(对齐 file-allocation / seed-ratio 既有全链惯例)

| 面 | 挂点 | 字段 |
|----|------|------|
| CLI | `arg_parser` + `main.cpp` help + `config_loader` 三件套(JSON 读/写/合并) | `--p2sp-share`、`--swarm-server`、`--swarm-fingerprint`、`--p2sp-advertise` |
| daemon | `daemon/config.hpp` `DownloadConfig` 节 + `config.cpp` 解析 + `main.cpp` apply + SIGHUP 对比重放 | `p2sp.share` / `p2sp.Rendezvous` 等;SIGHUP 可热更公告开关,不可热更监听端口(restart required,对齐既有语义) |
| RPC | `json_rpc_server.cpp` addUri 键映射(aria2 兼容键位惯例) | per-download `p2sp-share` 覆盖;`falcon.swarm.status`/`falcon.swarm.setShare` 扩展方法(桌面/浏览器扩展经既有 RPC 面控制共享,**UI 零新增协议**) |
| 密钥 | `~/.config/falcon/swarm_key.pem`(权限 0600) | 首次开启共享时生成,持久化保身份稳定 |

### 10.2 完成钩子:TaskManager 层,而非引擎层

**决策**:公告触发挂 `IEventListener`(TaskManager 层),不挂 V2 引擎收口函数。论证:

- TaskManager 层的完成分支(`task_manager.cpp` `on_status_changed` 的 Completed 路径)已派发 `dispatch_completed(task_id, output_path, total_bytes, elapsed)`——**公告所需的成品路径与总长在事件里现成可得**;
- 覆盖面:V1 curl(daemon/CLI **默认**数据面,`http_engine` 默认 "v1")+ V2 + metalink + 其他协议一次全覆盖;引擎层收口(`http_commands.cpp` 的 `complete_group_if_all_segments_done` 与 304 分支两个 COMPLETED 点)只覆盖 V2 HTTP,且 `RequestGroup::set_status` 是纯赋值无钩子可挂;
- 订阅样板现成:daemon `TaskStorageListener`(注册于 daemon `main.cpp:358-361`)三件套同款;裸指针 + `ListenerDetacher` RAII 摘除规约照搬;
- 新监听者 `SwarmAnnouncer`:Completed → 入公告队列(后台线程)→ `calculate_streaming(SHA256)` → announce。哈希与网络全部后台化,**完成回调路径零新增延迟**;
- BT 做种任务在 seeding 结束才置 Completed(既有语义),按 URL/options 分支过滤出 HTTP/metalink 任务公告(BT 资源共享走 BT 网络本身,不重复公告)。

### 10.3 入站数据服务(阶段二):内嵌只读 HTTP

- 形态:进程内只读 HTTP 端点 `GET /by-sha256/<hex>`,支持 Range(V2 P2SP 拉取方的分段请求);
- 监听端口沿用 `json_rpc_server.cpp:555-561` 的 getsockname 回读先例(`port=0` 随机端口,`port()` 访问器对外);绑定地址默认私网可达(配置化);
- 数据出口:成品文件直接文件流(sendfile 语义);**阶段二不做分片级服务**——只服务"完整持有"的资源,半成品不公告(§6.1);
- sha256 已知的成品在公告前算好(10.2 后台链),服务启动时无需全盘扫描哈希(公告状态持久化在本地 SQLite,重启加载)。

### 10.4 查询注入:swarm 作为镜像候选源

- 注入点:P2SP 源发现层。sha256 已知的任务(§6.4 三轨)在启动时 `query` → `sources` 里 `type:"node"` 转 V2 多镜像 URL 形态(`http://<addr>/by-sha256/<hex>`)与 `type:"url"` 直接进镜像池,与 metalink 镜像同池竞争、同规则换源;
- 与 metalink 阶段 2 的关系:swarm 源是镜像池的**新来源**,不是新协议——metalink 桥接的门禁(整文件哈希 + OpenSSL)天然满足(swarm 源必有 sha256);
- 段级失败换源、超时清理、If-Range 全部复用既有 P2SP 机制,swarm 源与任何镜像源无行为差异(§6.3 的零特权原则在集成层的体现)。

### 10.5 元数据消费(增值,非阻塞)

R3 元数据用于:同名任务去重提示("圈内已有此文件,是否直接拉取")、下载前尺寸预览。经 daemon RPC 扩展方法(`falcon.swarm.lookup`)暴露给桌面/扩展,阶段三做。

---

## 11. 故障降级

**总纲:swarm 是增强,不是依赖。** 每一条降级路径的终态都是"回到今天的下载行为":

| 故障 | 行为 | 恢复 |
|------|------|------|
| Rendezvous 不可达 | 节点下载照常;公告/查询静默失败,指数退避(1s 起,×2,上限 5min,加抖动);WS 断线按既有 `WebSocketRpcClient` 重连节奏 | Rendezvous 回来后 re-register(身份密钥稳定,session 重建)+ 全量重公告(本地公告状态在,增量补差) |
| Rendezvous 重启(无落库) | 同上;节点重注册风暴由注册限频(§9.5) + 客户端退避抖动吸收 | 全群在退避窗口内自愈 |
| 查询无结果 | 正常路径:走传统源(metalink 文档/URL 本身),无 swarm 参与 | 后台 WS `onResourceAdded` 到达时**不**自动重试已启动任务(阶段一语义简单;用户手动 refresh 触发重新查询) |
| swarm 源拉取失败/超时/校验失败 | 段级换源到下一来源(既有 P2SP 语义);多次失败的节点地址进本地黑名单(会话级) | 会话结束即忘(公告 TTL 会自然刷新持有状态) |
| 心跳超时被摘 | Rendezvous 摘节点连带摘其公告;WS `onPeerLeft` 通知圈内 | 节点侧退避重注册 |
| 公告 TTL 过期未续(节点退出) | 目录自然收敛 | — |
| 本机共享开关关闭 | 停止公告与数据服务;**已公告资源走 `retract` 优雅撤回**;下载/查询行为保留(可配置 `share.one_way`:只消费不提供) | — |

设计上的取舍:查询结果不自动注入已运行任务(阶段一),因为"下载中新源加入"涉及段重分配的复杂度,而既有 P2SP 已有"跨段连接"机制可承接——留作阶段三评估项(§13),不在第一阶段把降级矩阵复杂化。

---

## 12. 分阶段落地里程碑与验收边界

> 每阶段独立可验收、可独立合入;验收形态对齐仓库测试惯例(回环集成测试,三平台自包含,无外部网络依赖)。

### 阶段 0:Rendezvous 原型 + 身份面

**范围**:`falcon-swarmd` 单二进制(register/challenge/heartbeat/query/WS 通知 + 限频);节点侧密钥生成与 `SwarmClient`(注册/心跳/查询,无公告)。

**验收**:
- 回环 e2e:两节点 + 一 Rendezvous(test 固定端口/随机端口),注册挑战往返、心跳超时摘除、查询返回正确来源、WS 通知到达;
- 注入面:注册失败(错令牌/签名错/限频触发)的干净错误路径(对齐仓库 `injection.hpp` 惯例);
- 线协议对拍:canonical 序列化与签名的往返单测(对齐 BT Base32 向量"独立生成"惯例)。

### 阶段 1:公告/查询 + 完成钩子(只读增强,数据面零改动)

**范围**:`SwarmAnnouncer`(TaskManager 层监听 + 后台哈希 + 公告)、配置面全链(CLI/config/daemon/RPC)、`retract`、TTL 续租。

**验收**:
- e2e:节点 A 完成下载 → Rendezvous 目录可见 → 节点 B 查询命中(元数据一致);A 删除文件 → retract → B 查询落空;
- **降级铁律验收**:Rendezvous 进程杀死,A/B 下载任务(含进行中)全程无感,成品逐字节一致;
- 默认关验收:全量既有 ctest 套件零变化(共享默认 off = 引擎/事件路径零侵入的回归证明)。

### 阶段 2:入站数据服务 + P2SP 拉取(数据面打通)

**范围**:内嵌只读 HTTP(§10.3)、查询注入镜像池(§10.4)、NAT 标记过滤。

**验收**:
- e2e:metalink 任务(或旁车任务)从"swarm 节点源 + 原始镜像"混合分段拉取,成品逐字节一致、整文件哈希校验通过、无临时残留;
- 坏源剔除:伪造成 swarming 源返回错字节的 mock 服务器 → 校验失败 → 换源 → 成品仍正确(§6.3 零特权的直接检验);
- `direct:false` 过滤:NAT 标记节点不作为数据源但镜像 URL 仍参与;
- ASan 全套件零告警(网络 + 线程新代码,对齐仓库惯例)。

### 阶段 3(可选,按需求触发)

mDNS 局域网发现、R3 元数据消费(daemon 扩展方法 + 桌面去重提示)、下载中动态加入源、多群组。

**触发判定(2026-10-10 巡检,依用户授权自行裁定)**:维持未触发。依据:本节明文「可选,按需求触发」+ §13 延后表逐项触发条件(动态加入源=下载中新源出现频次证明价值;mDNS=Rendezvous 不可达但同网段场景真实存在;R3=按桌面端需求评估;多群组=多圈子需求出现)+ 仓库至今无对应产品需求记录;阶段 1/2 已交付完整发现-共享-消费链,无需求信号不开工。

---

## 13. 明确不做与延后

### 不做(设计定案,非资源问题)

| 项 | 理由 |
|----|------|
| 公共基础设施/公共索引 | §3.2:内容责任法务是产品生死级风险(迅雷/电驴被诉史);运营主体错位 |
| 全局内容搜索/浏览/热门 | §9.4:目录不可被当作分发入口;这是设计红线不是功能缺口 |
| 匿名参与 | 与身份认证(D2)根本冲突;私有圈子无匿名需求 |
| 积分/激励/信誉体系 | 私有小圈子无需博弈机制;激励体系是公共网络的产物 |
| Rendezvous 字节中继(TURN) | §3.3:把 Rendezvous 推回内容分发形态 |
| 端到端内容加密(消费方无法校验的加密共享) | §6.3 的校验链以明文哈希为锚;E2E 加密另立设计(需求未现) |
| 移动端节点 | 移动网络 NAT 深度不可达(§5),共享价值趋零 |

### 延后(各附触发条件)

| 项 | 触发条件 |
|----|---------|
| NAT 打洞族(UPnP→STUN→中继评估,§5.3) | 用户实测 NAT 后节点作为数据源的缺失显著影响体验 |
| 分片级共享(部分文件互传) | 大文件(GB+)在圈内"多机都要但没人下完"场景真实高频 |
| 下载中动态加入源 | 长尾大文件下载中 swarm 新源出现的频次证明其价值 |
| mDNS 局域网发现 | "Rendezvous 不可达但同网段"场景真实存在 |
| 多群组成员关系 | 单用户多圈子需求出现 |
| R3 元数据消费 | 阶段 1/2 落地后按桌面端需求评估 |

---

## 14. 风险与开放问题

| 风险 | 缓解 |
|------|------|
| Ed25519 在极旧 OpenSSL(<1.1.0)不可用 | 与 metalink 哈希同门禁:无能力时共享开关置灰,下载不受影响 |
| canonical JSON 序列化两端不一致导致验签失败 | 单测往返对拍 + 线协议示例固化字段序(§8.1) |
| swarmd 单点成为群内"必须在线"组件 | 降级矩阵(§11)把 Rendezvous 故障的影响面钉死在共享增强层 |
| 公告哈希计算与用户 I/O 竞争 | 后台线程 + 低优先级顺序读(流式 256KB 分块本身顺序);可选 `share.hash_after_minutes` 错峰 |
| 内嵌数据服务被局域网外扫描 | 默认绑定私网/指定接口;sha256 路径不可枚举;加固阶段 per-request token(§9.6) |

**开放问题**(实现前需决断,不阻塞设计):
1. R2 镜像公告的 URL 隐私:镜像 URL 可能含私有 token(如带签名的对象存储 URL)——首版由用户配置排除规则(默认不过滤,文档警示),还是默认正则过滤?倾向后者,待阶段 1 实现时定。
2. session 凭据的持久化:Rendezvous 重启后 session 失效全群重注册 vs session 落库续用——倾向前者(重注册协议本身就是幂等自愈路径,简单性优先)。

---

## 15. 结论

**形态**:自托管私有 swarm——用户自架 `falcon-swarmd`(索引面,复用 daemon 自实现 HTTP+WS JSON-RPC 栈,零新依赖),节点间数据面直连(复用 V2 P2SP 全部机制),Rendezvous 索引即离场。

**十条决策**:

| # | 决策 | 核心论据 |
|---|------|---------|
| D1 | 自托管私有 swarm | 内容责任/运营/冷启动三题同解(§3) |
| D2 | Ed25519 持久身份,指纹即 ID | 节点级审计/吊销;防抢注(§9.2) |
| D3 | 中心 Rendezvous 权威;DHT 双路线否决 | 小圈子无 DHT 收益;仓库实证 DHT 公告面零基础且有公网暴露/NAT 失效结构缺陷(§4) |
| D4 | 阶段一不做穿透;拉取方主动连接 + direct 标记 | 部署形态覆盖主场景;本机地址设施零基础(§5) |
| D5 | 共享面 = 成品/镜像/元数据 三类 | 由 P2SP 数据面需求反推;路径与历史不共享(§6.1) |
| D6 | SHA-256 内容寻址 + 消费端强制校验,零特权 | 复用 `verify_streaming`;坏源下场与坏镜像一致(§6.3) |
| D7 | 独立单二进制,索引即离场 | 职责/生命周期/部署三重分离(§7) |
| D8 | JSON-RPC 2.0 + `falcon.swarm.*` + WS 通知 | 与 daemon 协议族同构,双端代码现成(§8) |
| D9 | TLS pinning + 挑战-签名 + 限频/配额/吊销 + 共享默认关 | 私有圈威胁模型全覆盖;默认关是产品红线(§9) |
| D10 | TaskManager 层完成事件挂公告;查询注入镜像池 | 覆盖默认 V1 引擎,`dispatch_completed` 现成携带路径与总长(§10.2) |

**路线图**:阶段 0(Rendezvous + 身份)→ 阶段 1(公告/查询 + 完成钩子,下载面零改动)→ 阶段 2(数据面打通,混合源分段拉取)→ 阶段 3(按需)。每阶段独立验收,阶段 1 的"Rendezvous 杀死下载无感"与"默认 off 全量测试零变化"是本设计两条不可妥协的验收铁律。

与 todo.md 备忘的对账:路线 C 的"中央索引"以**自托管私有**形态落地(避开其法务与运营定性的适用前提);备忘三件事中"索引公告"与"上传策略(默认关)"在本文完整设计,"入站监听"细化为阶段 2 的内嵌只读数据服务;备忘的 NAT 穿透评估(§5)与冷启动定性(§3.2)分别给出结论。

---

## 16. 阶段 1 实现设计(2026-10-08 收口)

本节把 §12 阶段 1 范围(`SwarmAnnouncer` + 配置面全链 + `retract` + TTL 续租)落成可指导实现的设计,并裁决 §14 两个开放问题。实现按增量推进,每增量全绿后提交。

### 16.1 开放问题裁决(§14 → 定案)

1. **R2 镜像 URL 隐私 = query/fragment 一刀切过滤**。镜像 URL 含签名 token 时 token 几乎总在 query(S3 预签名/OSS 签名/Kodo token 均如此)——公告侧对 R2 URL 做**最小卫生检查:URL 含 `?` 或 `#` 即丢弃该 R2 条目(计入 rejected),只保留 R1**;scheme 非 http/https 同样丢弃。不做正则白名单(维护成本高、误杀难排查)。局域网地址(RFC 1918)不过滤——P2SP 主场景就是局域网/小圈子,内网可达地址恰是合法公告内容。`share.announce_mirrors: false` 可整体关闭 R2 公告。
2. **session 凭据不持久化**。重注册协议本身是幂等自愈路径(Rendezvous 重启 → 全群 -32003 → 重注册),公告状态机以**本地意图表**(内存)为准:重注册成功后全量重公告补差。密钥文件即身份,session 只是租约。

### 16.2 服务端(falcon-swarmd):announce/retract 语义

**签名形态(与 register 挑战式的分叉)**:announce/retract 高频(60/min)且副作用幂等(upsert),不值得每请求一次挑战往返。签名 payload 沿 §8.1 公式,**nonce 语义位填 session**:

```
payload = method + "\n" + session + "\n" + sha256_hex(canonical_params_without_sig)
```

参数完整性由 sha256(params) 绑定(防偷换——与 register step2 同一机制),身份由 Ed25519 保证;重放窗口 = 重复 upsert 同一资源,无安全后果(幂等)。

**announce(params = {session, sig, resources:[...]})**:
- 鉴权链:Bearer(传输层)→ session 有效性(-32003)→ 验签(-32005)。验签失败同 register 惯例。
- params 校验:resources 必须为数组且 ≤256 条(§9.5),超限 -32602;每条严格 schema(未知字段拒绝):`kind` ∈ {`file`,`mirror`};file 必带 sha256(64 hex 小写);mirror 必带 url(http/https)+sha256;`ttl_s` 缺省 86400,钳 [3600, 604800](1h..7d)。
- **归并语义**(§8.3):同 sha256 → 同资源。file 条目 upsert node 源(node_id = session 所属节点,direct/addr/agent 取注册态);mirror 条目 upsert url 源(etag/last_modified/accept_ranges 可选)。**url 源的归属者记为公告者**(SwarmResourceSource.node_id 复用为归属者字段)——retract 与节点摘除联动靠它。
- name/size:资源级单值,**非空值后写胜出**(确定性;hash_only 公告 name 缺省不覆盖已有 name)。
- expires_at:**max(现值, now + ttl_s)**——多公告者共存时短 ttl 公告者不得缩短他人续租;单公告者自续租语义不变。
- result = `{accepted, rejected, expires_at}`(资源级 expires_at,max(各命中资源),**值为剩余整秒数**(数值型;空 resources 数组合法,accepted=0 且 expires_at=0)。单条资源非法(哈希形态错/URL 被隐私过滤)计入 rejected 不失败整请求;params 级错误才 -32602。
- **onResourceAdded 触发:资源首次出现才广播**(params object,沿 §8.5);已有资源追加来源不广播(增量语义 = 新资源;来源变更由查询面覆盖)。

**retract(params = {session, sig, sha256s:[64hex,...]})**:
- 语义:摘除本节点在该资源上的**全部来源**(node 源 + 本节点公告的 url 源)。
- 资源 sources 清空 → 删资源 + 广播 `onResourceExpired`(§7.3 触发表:TTL 到期/持有者下线——显式 retract 清空同属此语义);sources 未空(他节点仍持有)→ 不广播。未知 sha256 计入 unknown 不报错。
- result = `{removed, unknown}`。

**TTL 到期与节点摘除联动(更正阶段 0 裁决)**:sweep 扫 `resources_`,`expires_at <= now` → 删资源 + onResourceExpired(阶段 0 的「过期资源无通知」是表恒空占位,阶段 1 按 §7.3 触发表生效)。sweep/unsubscribe 摘节点 → 连带摘其全部来源;资源 sources 空 → 删资源 + onResourceExpired。

**限频/配额**:RateLimiter 第三实例(announce 60/min per **node_id**——与 register/query 的 per-IP 键不同,公告配额按身份计);每节点活跃公告 ≤10k 条(按来源归属计数),超配额整请求拒绝 -32002(429 同发,沿既有形态)。

### 16.3 节点侧(falcon-swarmd client):SwarmClient 扩展

- `SwarmError announce(const std::vector<AnnounceResource>&)` / `SwarmError retract(const std::vector<std::string>& sha256s)`;`AnnounceResource{kind, sha256, name, size, url, etag, last_modified, accept_ranges, ttl_s}`。
- 签名 nonce 位填当前 session;非 0 result 的 rejected/unknown 交调用方解读,网络/协议失败走 SwarmError。

### 16.4 SwarmAnnouncer(daemon 侧新组件)

新文件 `packages/falcon-daemon/src/daemon/swarm_announcer.{hpp,cpp}`,**编进 falcon-daemon 可执行 target**(依赖 falcon_swarm_client→CURL;falcon_daemon_core 不链 CURL,不进该库)。生命周期走 main.cpp 监听者注册区既有形态(`TaskStorageListener` + `ListenerDetacher` RAII 同款,main.cpp:374-389 先例)。

**完成钩子(D10)**:实现 `IEventListener::on_completed(task_id, output_path, total)` → 入后台哈希队列;哈希(`FileHasher::calculate_streaming`,SHA-256)与网络全部在 announcer 工作线程,**完成回调路径零新增延迟**。可选 `share.hash_delay_s` 错峰。

**过滤链(顺序)**:
1. `share.enabled` 为假或 `one_way`(只消费)为真 → 不公告;
2. per-download 覆盖:options `p2sp_share`(三态 ""/"true"/"false","" = 跟全局);
3. `magnet:` 前缀任务不公告(BT 资源走 BT 网络自身发现面;`.torrent` 文件任务按普通文件公告语义自洽——他人可经 P2SP 拿到种子文件,不误伤);
4. 哈希失败(文件消失/读错误)→ 放弃本条,不重试不告警升级(WARN 一次)。

**公告内容**:R1(file, sha256, name/size 按 mode)+ 任务 URL 为 http(s) 且通过隐私过滤(16.1)时附 R2(mirror, url=任务 URL;etag/last_modified 任务层无,缺省;accept_ranges 缺省不公告)。`share.mode: "standard"` 公告 name+size,`"hash_only"` 连 size 一起省(size 侧信道可猜内容)。

**TTL 续租循环(announcer 线程)**:周期 = min(5min, ttl/3)。每轮对每条活跃公告:
- 文件存在且 (mtime,size) 与公告时一致 → announce 重发(续租);
- 文件消失 → retract + 移出活跃表(e2e 验收铁律 (a) 的 retract 入口);
- (mtime,size) 变化 → retract 旧哈希 + 重哈希公告新哈希(旧资源不 dangling);
- `SwarmClient::session()` 变化(检测于续租循环)→ **全量重公告**(§16.1 裁决 2 的补差路径);
- announce 失败 → 指数退避(1s 起 ×2,上限 5min,加抖动),-32003 无需特殊处理(SwarmClient 心跳线程自动重注册,自愈后 announce 自然成功)。

**停机语义**:stop() 停哈希队列与续租循环,**不 retract**——停机不等于删文件,重注册后全量重公告补差;异常停机泄漏的公告由 TTL 上界(7d)自愈。`share.enabled` 热更关 → 全量 retract 后停(§11「优雅撤回」)。

**私钥**:`~/.config/falcon/swarm_key.pem` 0600,`SwarmKeyStore` 既有 load_or_create;同机 daemon+CLI 共用密钥会互踢 session(各自心跳 -32003 往复),自愈无损害,文档披露为已知行为。

### 16.5 配置面全链

**daemon(daemon.json `p2sp` 节,默认整体缺省 = 共享关)**:

```jsonc
{
  "p2sp": {
    "share": { "enabled": false, "mode": "standard", "one_way": false,
               "ttl_s": 86400, "hash_delay_s": 0, "announce_mirrors": true },
    "rendezvous": { "host": "127.0.0.1", "port": 7800,
                    "server_token": "", "group_token": "",
                    "advertise_addr": "", "advertise_direct": true }
  }
}
```

- 类型错误报错退出(daemonize 前),未知键告警——沿 config 既有分型;`mode` 非法值告警保留 standard(对齐 http_engine 先例)。
- SIGHUP:`share.*` 全部热更(enabled 变 false → 全量 retract);`rendezvous.*` 变化 → "restart required" 告警。

**RPC(阶段 1 增量 4)**:`falcon.swarm.status`(announcer 状态快照:enabled/registered/node_id/session/announced_count/queue_depth)+ `falcon.swarm.setShare`(params {enabled},运行时开关同热更语义);addUri per-download 选项 `p2sp-share`("true"/"false" 字符串,aria2 风格),消费点 = announcer 过滤链,持久化随 TaskStorage options JSON 尾字段。

**CLI(阶段 1 增量 4)**:`--p2sp-share` / `--swarm-server host:port` / `--p2sp-advertise addr` 接 config_loader 三件套;`--swarm-fingerprint` 解析但启动 WARN 忽略(阶段 0 client 无 TLS,前向声明);CLI 单发形态**无续租无数据服务**——下载完成后同步哈希(阻塞,CLI 无后台宿主)→ announce 一次 → 退出,announce 失败 WARN 不失败下载;资源可用窗口 = min(TTL, 退出前),如实披露。daemon 是共享的一等形态,CLI 公告属尽力而为。

### 16.6 测试设计(防同一 bug 自我印证)

- **服务端纯单元**(虚拟时钟):announce upsert 归并/node-url 源归属/expires_at max 语义/name 后写胜出/ttl 钳制/rejected vs -32602 分型/retract 部分摘除与清空删资源/摘节点联动/TTL sweep 通知/限频 per-node/配额 10k/签名失败 -32005/nonce=session payload 对拍。
- **回环线协议**:真 socket announce→query 命中元数据一致(铁律 a 前半)/retract→落空/announce 通知真到达 WS/onResourceExpired 两触发面/429 与 -32002 同发。
- **client**:SwarmClient announce/retract × 回环 Rendezvous(签名往返真实验签)。
- **announcer**:哈希队列(伪 Hasher 注入)/过滤链全分支/mtime+size 变化三分支(续租/retract/重公告)/session 变化全量补差/退避(虚拟时钟或短间隔)/停机不 retract。
- **e2e 验收(铁律,增量 4 收口)**:(a) A 完成 → B query 命中(元数据一致)→ A 删文件 → retract → B 落空;(b) Rendezvous 进程杀死,A/B 下载任务(含进行中)全程无感,成品逐字节一致;(c) 默认 off 全量既有 ctest 套件零变化。

### 16.7 增量切分

| 增量 | 内容 | 门禁 |
|---|---|---|
| 1 | 本节设计落文档(已完成)+ swarmd 服务端 announce/retract/sweep 联动/通知/限频/配额 | swarmd 单元+回环全绿(✅ 已完成 1b0a13f) |
| 2 | SwarmClient announce/retract + 测试 | swarmd 四 target 全绿(✅ 已完成) |
| 3 | SwarmAnnouncer + daemon p2sp 配置节 + main 接线 + SIGHUP | daemon 全套件 + swarmd 全绿（✅ 已完成 2026-10-09） |
| 4 | RPC falcon.swarm.status/setShare + addUri p2sp-share + CLI 参数链 + e2e 验收四条 + 文档对账(daemon/swarmd CLAUDE.md、README) | 全仓 ctest 全绿 + e2e 铁律（✅ 已完成 2026-10-09：e2e 三铁律 3/3 + 10/10 压测，target `falcon_daemon_swarm_e2e_tests`） |

---

## 17. 阶段 2 实现收口(2026-10-10)

阶段 2(入站数据服务 + P2SP 拉取)已实现,六批提交:`8c7ee56`(数据服务)、
`e3bb66a`(查询注入镜像池)、`d141b95`(坏源拒收钉子)、`64f5097`(winsock2
编译面修复)、`102d4e1`(查询面独立于共享开关)、`ca09aa3`(e2e 混合拉取铁律)。

### 17.1 落地形态与设计条目对账

| 设计条目 | 落地 | 对账/偏差 |
|---|---|---|
| §10.3 入站只读 HTTP `GET /by-sha256/<hex>` + Range | `swarm_data_service.{hpp,cpp}`(daemon):accept 线程 + detached 连接线程台账(沿 2026-10-01 生命周期纪律);注册表经 `ISwarmDataRegistry` 由 announcer 的「已完成 → 已哈希 → 已公告」链填充,半成品永不注册;文件名只以 sha256 对外,不暴露本地路径 | 方法白名单 GET/HEAD(HEAD 为实现增益);一连接一请求(`Connection: close`,无 keep-alive);404(未注册/路径不符)/400(非法 hex)/405 幂等语义全测 |
| §10.3 绑定地址 | `make_data_service_config`:`p2sp.rendezvous.advertise_addr` 形如 "ip:port" → 按公告端口绑定(公告与实听一致,e2e 直探);空/解析失败 → 0.0.0.0:0 随机端口 + `port()` 回读 | 绑定失败仅告警不阻断(数据面是增量能力,对端连拒由段级换源吸收) |
| §10.4 查询注入镜像池 | protocols 公共缝 `mirror_source.hpp`(函数回调,protocols 不反向依赖 daemon,宿主未注册即零查询面);daemon 侧 `SwarmSourceProvider`:node 源 → `http://<addr>/by-sha256/<hex>`、url 源(http/https)原样透传;metalink V2 桥接镜像池 = 文档镜像在前(If-Range 归属者) + swarm 源去重后追加 | **增强语义:文档镜像池 ≥2 时不查询**(§10.4 只说注入,未说何时查——补源只在池不足时发生,零冗余查询);恢复路径忽略 swarm 源漂移(断点不作废);段级失败换源/超时清理/If-Range 全复用既有 P2SP 机制,swarm 源零特权 |
| §10.5 NAT 标记过滤 | provider `node_source_usable`:advertise 对象缺席 / addr 空 / 不含 ':' / `direct==false` 一律跳过;本机自身 node_id 跳过 | url 源不经 NAT 过滤(镜像 URL 仍参与,与设计一致) |
| §11 降级 | 查询连续失败指数退避 30s×2^n 封顶 300s,抑制期内查询不发往网络;任一次成功即清除;查询失败面零外泄(异常/形状不符全收口为空表,发现面故障绝不阻断下载) | **查询面独立于共享开关**(102d4e1):进程级共享 SwarmClient 惰性创建 + start 幂等(start_mutex_);share off 时公告停/数据服务停,查询仍活——§11 one_way 语义的落地;停机回调摘缝先于排水(在途查询持 shared_ptr 拷贝延寿,无悬垂) |
| §12 验收四条 | 见 §17.2 | 全过 |

### 17.2 测试与验收对账

- **门禁与日志**:文档镜像与 swarm 源合计 ≥2 才走 V2 多源,否则回落串行委托(与阶段 2 前单镜像行为一致);跳过原因分级日志(池不足/接缝未注册/查询异常)。
- **provider 纯单元 12 用例**(`swarm_source_provider_test`):parse_sources 形状容错全分支(node/url/未知 type/非 object/去重/自身排除)/`node_source_usable` direct 矩阵/退避状态机(失败抑制 → 成功清除,抑制期查询不发网络)。
- **数据服务 12 用例**(`swarm_data_service_test`,POSIX 回环):随机端口全量/Range 精确切片/HEAD 无体/未注册 404/非法 hex 400/路径 404/方法 405/注销与清空/成品文件缺失 404/幂等 stop/并发客户端。
- **metalink 桥接 +7 用例**(`metalink_handler_test`):swarm 源扩展池启用多源/与文档镜像重复不收录/provider 抛异常回落串行/非 http 源过滤/空结果/池足够不查询/**坏源拒收**(d141b95:`SwarmSourceBadBytesRejectedByWholeFileHash`——swarm 源发错字节 → 整文件哈希校验失败 → 回落文档镜像串行 → 成品仍正确,§6.3 零特权的直接检验)。
- **e2e 混合拉取铁律**(ca09aa3,`SwarmDaemonE2E.MetalinkV2BridgePullsSwarmSourceIntoMirrorPool`):A(share on + advertise_addr)从 legacy 镜像完成后公告节点源;B(share off + http_engine=v2)拿到单文档镜像且该镜像拒绝 Range(416)——串行路径结构性必败的判别器;B 经 swarm 查询注入节点源 → V2 多源分段混合拉取;断言文档镜像上 plain GET ≥1 且 Range GET ≥1(Range 只在 V2 多段轮转下出现,串行回落形态不可能)+ 成品逐字节一致 + 无 `.falcon.*` 残留 + 三进程干净退出。注册限频 per-IP 计 RPC 调用(A 公告 + 探针 + B 查询 = 3 客户端),e2e 显式调高。
- **ASan**:swarm 全套件 + metalink 桥接 198/198、daemon/RPC 233/233 零告警(含真二进制 e2e 4/4)。
- **CI**:8 job 全绿(ca09aa3,run 37996089515)。

### 17.3 已知边界(如实披露)

- 数据服务为最小手写 HTTP/1.1(状态行 + Content-Length/Content-Range),无 keep-alive;只服务完整成品,不分片级服务(§10.3 定案);
- §11 的「多次失败节点进会话级黑名单」未做——坏源由整文件哈希兜底拒收,重复坏源仅浪费重试预算,留阶段 3 评估;
- 查询注入只发生在下载启动(首次/回落)时刻,运行中任务不加源(§11 明文阶段一语义,动态加源留阶段三);
- e2e 为真三进程拓扑(2 daemon + rdv,fork 依赖),POSIX-only(与阶段 1 同姿态);Windows 由 provider/data-service/bridge 的纯单元与回环用例覆盖。
