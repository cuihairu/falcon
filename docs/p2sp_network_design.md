[中文](p2sp_network_design.zh.md) | **English**

# Falcon Node Mutual Discovery and Resource Sharing: Rendezvous Service Design (P2SP Sharing Network)

> [!NOTE]
> This document is design material; none of its content has yet entered the public API. It carries forward the roadmap discussion of the "P2SP Sharing Network Route Discussion (Project Memo)" in `todo.md`, advancing it into a design that can guide implementation.

> **Status**: Stage 0 implemented (2026-09-25, `falcon-swarmd` package; see that package's CLAUDE.md);
> **Stage 1 implemented** (2026-10-09, §16, daemon announce/query/RPC/e2e iron rules);
> **Stage 2 implemented** (2026-10-10, §17, data service + query-injected mirror pool + NAT filtering);
> Stage 3 not started.

## 1. Problem Statement and Goals

Capabilities falcon has today (2026-09 state):

| Capability | Current State | Relation to This Design |
|------|------|---------------|
| V2 engine multi-source segmentation (P2SP) | metalink Stage 2 landed: the same file across multiple mirrors with segment-level source switching | **The data plane already has all the mechanics of "pulling the same file from multiple sources"** |
| daemon aria2-compatible RPC | 28 methods, HTTP + WebSocket on the same port, self-implemented with zero new dependencies | Candidate technical base for the Rendezvous and node control plane |
| metalink | RFC 5854/Metalink3 dual compatibility; publish only after whole-file hash verification | A download form where the "hash is known out-of-band", naturally enabling sharing during download |
| BT/DHT | libtorrent data plane + self-developed DhtClient (Kademlia iterative lookup) | Has validated all the engineering constraints of a peer network (see §4 selection) |
| Task persistence | SQLite, recovery after shutdown | Reference for the persistence conventions of the node-side announce state machine |

**What is missing is the last link**: multi-source segmentation today can only obtain "multiple sources of the same content" from a metalink document or the URLs of a single download task. The real user scenario is a fleet of falcon nodes — home NAS, desktop machines, cloud machines — each downloading files yet none aware of what the others hold:

- A 4GB file already downloaded on the desktop can only be fetched from scratch from the origin mirror when the NAS needs it;
- Each machine maintains its own knowledge of "where this URL can be downloaded", with no way to share;
- A cloud machine has public bandwidth while the NAS sits on a LAN; the two cannot serve as mirror sources for each other.

**Goal**: design a self-hosted Rendezvous Service (commonly called a tracker; the index coordination plane) so that a user's own falcon nodes discover each other, serve as download sources for each other, and share three classes of resources — "completed files / usable mirrors / task metadata" — within the circle. The data plane (file bytes) is always node-to-node direct; **the Rendezvous never relays content**.

**Non-goals** (justification in §3 and §13): public infrastructure, global content search, anonymous participation, any form of credit incentives.

---

## 2. Decision Overview

This document threads decisions D1–D10, each with its "why". The summary table is in §15.

| ID | Decision |
|------|------|
| D1 | Network form = self-hosted private swarm: self-hosted Rendezvous (index plane) + node direct connections (data plane) |
| D2 | Node identity = persistent Ed25519 key pair; node ID = public key fingerprint |
| D3 | Discovery mechanism = central Rendezvous Service as the authoritative plane; mDNS LAN discovery deferred; DHT route rejected |
| D4 | NAT traversal = not done in Stage 1; covered by deployment-form constraints + "puller connects actively" model |
| D5 | Sharing plane = three classes: completed files / mirror URL list / task metadata; piece-level sharing deferred |
| D6 | Content addressing and integrity = SHA-256 whole-file hash, verified after download with `verify_streaming` |
| D7 | Rendezvous form = independent single binary, JSON-RPC over HTTP+WS, index-and-leave |
| D8 | Wire protocol = JSON-RPC 2.0, `falcon.swarm.*` method family + WS event notifications |
| D9 | Security = TLS + fingerprint pinning, challenge-signature authentication, rate limiting/quota/revocation, sharing off by default |
| D10 | Node integration point = TaskManager layer completion event (not the engine layer); CLI/config/daemon/RPC four-plane configuration |

---

## 3. Overall Form (D1): Self-Hosted Private Swarm

### 3.1 Form Definition

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

- **Index plane**: nodes register identity, announce resources, query resources, and subscribe to change notifications with the Rendezvous.
- **Data plane**: node A needs file X (sha256 known) → queries the Rendezvous to learn "nodes B and C hold X" → A directly issues HTTP Range pulls against B's and C's embedded data services; the V2 engine applies the existing P2SP semantics for segmentation, source switching, and verification.

### 3.2 Why "Self-Hosted Private" Rather Than a Public Network

The project memo in todo.md already characterized route C (central index): it requires server infrastructure, operations, and content-liability legal exposure — "beyond the scope of writing code". That characterization targets a **public** central index — a content distribution index open to an indeterminate public. The lawsuit history of Thunder (Xunlei) and eDonkey shows this is an existential, product-level risk. Self-hosting dissolves three problems at once:

1. **Operating entity**: the Rendezvous is one of the user's own machines (a one-shot binary, with a usage threshold like `aria2c`); there is no platform operator.
2. **Content liability**: the sharing circle is the user's own machine fleet (family NAS + own cloud machines); nodes directly share files they own. The Rendezvous stores only index metadata — "whose which sha256 is at which address" — no content, no search.
3. **Cold start**: the cold start of a public network is a product problem (no users means no speed, per the memo's original wording); a private network **has no cold start by construction** — the nodes are the user's own machines, and the full topology exists from day one.

Meanwhile, the "off by default" principle (§9.5) guarantees: falcon users who do not enable sharing behave byte-for-byte identically to today; and even when sharing is enabled, it is visible only within the Rendezvous and groups the user explicitly configured.

### 3.3 Why the Data Plane Does Not Relay Through the Rendezvous

Relaying content through the Rendezvous (TURN-style) would turn the self-hosted Rendezvous's bandwidth into a fleet-wide bottleneck and upgrade the Rendezvous from an "index service" to a "content distribution service" — exactly the legal form §3.2 seeks to avoid. Index-and-leave: the Rendezvous's response contains only addresses; bytes flow only between nodes. The exception outlet for relaying is deferred to §13.

---

## 4. Discovery Mechanism Selection (D3)

Four candidates; routes A/B/C in the todo.md memo correspond to three of them. This section uses the current state of the repository to demonstrate the true cost of each route.

### 4.1 Comparison of Options

#### Option 1: Central Rendezvous Service (Selected)

**Advantages**:
- Determinism: a query gets an answer immediately, with no iterative convergence delay; in a private circle of fewer than 20 nodes, DHT's O(log N) scalability advantage does not exist — a single HTTPS request beats multi-hop UDP.
- Manageability: authentication, rate limiting, quotas, and revocation all land in one place at the Rendezvous (§9); in a DHT you cannot "kick out a node".
- Natural offline semantics: after a node goes offline, its announcements survive with a TTL; online notifications arrive instantly via WS push.
- Same technical stack as daemon: daemon already has a self-implemented HTTP+WS JSON-RPC server (`packages/falcon-daemon/src/rpc/json_rpc_server.*`, including the WS frame protocol, token authentication, and the getsockname port-readback precedent); the Rendezvous directly reuses the same family of stack, **zero new dependencies**.

**Disadvantages**:
- Single point: if the Rendezvous goes down, the sharing plane fails (§12 degradation semantics: downloads are unaffected).
- Requires the user to run a service (a threshold comparable to self-hosting an aria2 RPC).

#### Option 2: Public BT DHT Bridging (Memo Route A) — Rejected

Announce a synthesized infohash (derived from the product's SHA-1 plus a magic prefix) into the public DHT, zero infrastructure. **Rejected — four reasons, the first two backed by repository evidence**:

1. **Implementation cost overturned by current state**. The memo estimated "cheapest data plane", but the investigation of the self-developed `DhtClient` (2026-09) shows: `announce_peer` is zero-implemented (only the `DhtQueryType::AnnouncePeer=3` enum and string-mapping placeholder; token has zero hits in `dht_node.cpp` — not captured, not stored, not returned); there is no inbound query answering (`handleMessage` only handles Response, `y=q` is always ignored, i.e., there is no infohash→peers storage table or server capability); the BEP-5 wire format has three deviations (id written at the top-level dict rather than inside `a`/`r`; decode's `r` only absorbs string values, causing the real `values` list to be dropped wholesale; `arguments`' `map<string,string>` cannot hold the integer `implied_port`). Connecting to the public DHT = first building four new surfaces — announce sending, token flow, inbound answering, and wire fixes — and verifying each against MockDhtNode and real nodes.
2. **Public exposure**. The magic prefix of the synthesized infohash guards against accidental collisions, not active scanning: any third party can enumerate the DHT to collect "who holds which sha256", publishing private download behavior into publicly observable space.
3. **NAT failure**. A node behind NAT announces an ip:port that is unreachable from the public internet; the public DHT cannot assist with hole punching (§4.2) — the announcement goes nowhere. This is a structural defect, not a tuning problem.
4. **Absence of governance in the public DHT**. No authentication (anyone can answer with fake addresses for any infohash), no revocation, no rate limiting; pollution and abuse protection can only rely on client-side verification.

#### Option 3: Falcon Private DHT (Memo Route B) — Rejected

The same Kademlia with different bootstrap nodes. Reuses all the new surfaces from rejection reason 1 of Option 2 (a private DHT equally needs announce/token/inbound answering), plus:

- **Cold start is unsolvable in private form**: a private circle has single-digit node counts; DHT lookup's multi-hop iteration adds pure latency (bootstrap → get_peers iteration → convergence, seconds; an HTTPS query is milliseconds).
- The bootstrap nodes are yet another resident service to maintain — complexity rises instead of falls.
- The self-developed DHT's maintenance thread does a random find_node every 5 minutes (existing behavior) — meaningless traffic in a private network.

**DHT capability is not wasted**: the self-developed DhtClient serves BT tasks (infrastructure beyond the libtorrent mode), orthogonal to this design. If a public P2P form is entered in the future, the DHT route can be re-evaluated.

#### Option 4: mDNS (LAN Auto-Discovery) — Deferred, Listed as an Optional Acceleration Layer

- Only valid within a LAN; fails across subnets/networks (NAS at home, cloud machine on the public internet); cannot serve as the authoritative plane.
- Cross-platform implementation differences are large (Avahi/Bonjour/self-implemented multicast), conflicting with the repository's convention of "self-contained test infrastructure on three platforms" (similar lesson: SFTP was blocked because it could not be loopback-mocked).
- **Deferral trigger conditions** (§13): once users demonstrate a real need for "Rendezvous unreachable but same subnet" discovery in LAN scenarios, evaluate self-implemented multicast (mDNS text protocol, no Avahi dependency).

### 4.2 Selection Conclusion

**The central Rendezvous Service is the only authoritative discovery plane**; mDNS is deferred as a LAN acceleration layer; both DHT routes (public/private) are rejected. The product of discovery is "resource → set of holding nodes (with reachability addresses and NAT flags)", consumed by the sharing model of §6 and the engine integration of §10.

---

## 5. NAT Traversal (D4): Explicitly Not Done in Stage 1

### 5.1 Current-State Constraints

Repository state (2026-09 investigation): no getifaddrs / SIOCGIFADDR / GetAdaptersAddresses anywhere — **there is no existing facility for enumerating the machine's external-facing IPs**; UPnP/NAT-PMP is explicitly disabled only in the libtorrent branch; reachable addresses currently have only the configuration-injection path (daemon `--rpc-listen-host`, default 127.0.0.1, not externally reachable). STUN/hole punching has zero foundation.

### 5.2 Stage 1 Model: "Puller Connects Actively" + Deployment-Form Constraints

Not doing traversal does not mean NAT'd nodes are completely unusable. Key observation: **the data plane's connection direction is free** — A pulling from B does not require B to be able to actively connect to A, only that A can initiate one TCP to B. Hence the Stage 1 reachability matrix:

| Deployment Form | As Data Source | As Puller | Notes |
|---------|-----------|-----------|------|
| Same subnet (NAS + desktop) | Yes | Yes | Private addresses interwork, no NAT isolation |
| Public machine (cloud machine) | Yes | Yes | Publicly known address, direct connection |
| Behind NAT (home broadband) | No by default | Yes | Only initiates, does not receive |
| Behind NAT + user configures port mapping / public address | Yes | Yes | Explicit `advertise_address` configuration |

Implementation rules:

1. Node registration/heartbeat goes over outbound TLS to the Rendezvous; **the Rendezvous observes the node's egress IP from the connection's source address**, but the source address is **untrusted by default** for the data plane (NAT-shared egress).
2. The node's announced data service address = the explicitly configured `advertise_address` (ip:port); if unconfigured, announce `direct=false` (can only initiate, cannot be pulled from).
3. A querier does not treat `direct=false` nodes as P2SP data sources, but they can still be members of the "mirror URL pool" (the mirror pull direction is initiated by the node itself, which traverses NAT naturally).
4. The Rendezvous flags `direct=false` nodes with a NAT flag for the querier to filter (§8 message fields).

This model covers the two main roles in a home scenario: the NAS (usually port-mapped or same-subnet access) and the desktop machine (the puller). **What it cannot cover**: both behind symmetric NATs and neither willing to map ports — they are not data sources for each other, but still share mirror URLs and metadata (retaining most of the value).

### 5.3 Deferred Items and Trigger Conditions

| Technology | What It Solves | Why Not Now | Trigger Conditions |
|------|---------|---------------|---------|
| UPnP-PCP / NAT-PMP | NAT'd nodes automatically get a public port | Long tail of router compatibility; requires local address-enumeration facilities first | Users demonstrate significant friction with port mapping |
| STUN + UDP/TCP hole punching | Traversal beyond symmetric NAT | Rendezvous-coordinated hole punching is a new protocol plane; success rate depends on NAT type and cannot be promised | Strong demand remains after UPnP lands |
| TURN relay | Fallback for all unreachable cases | Violates "index-and-leave" (§3.3); Rendezvous bandwidth cost returns to platform form | Evaluation only, no commitment |

---

## 6. Resource Sharing Model (D5/D6)

### 6.1 Sharing Plane Definition: Three Resource Classes

**What is shared** is derived from the P2SP data plane's needs — multi-source segmentation needs "multiple sources of the same content", and sources fall into three classes:

| Resource Class | Content | Consumption | Integrity Guarantee |
|--------|------|---------|-----------|
| **R1 completed files** | Files a node has finished downloading: sha256, filename, size, holding node (including data service port) | As a P2SP data source (§10.3 embedded HTTP service) | SHA-256 verification after download (§6.3) |
| **R2 mirror URLs** | Original/final URLs recorded when a node's download succeeds (URL, ETag, Last-Modified) | Candidate sources injected into the metalink mirror-pool semantics | Decided by the URL-side semantics (metalink hash / existing verification chain) |
| **R3 task metadata** | Filename, size, ETag, Last-Modified, Accept-Ranges capability | Auxiliary: If-Range resume bridging, pre-download metadata preview, same-name dedup | Informational, does not participate in verification |

**What is not shared** (privacy and legal boundaries, justification in §9.4):

- Paths: only filenames are shared, **not directory structures** (local paths are private and also leak directory layout).
- Download history: only "currently held completed files" are announced, never "what was downloaded in the past" (history is owned exclusively by the node's local SQLite).
- Unfinished tasks: piece-level sharing (mutual transfer while downloading) is deferred (§13) — content integrity cannot be promised during a normal HTTP download (the hash is unknown, §6.4); metalink tasks are the exception but Stage 1 does not specialize a piece protocol for them.

### 6.2 Content Addressing

R1 resources use **SHA-256 hex** as the primary key. SHA-256 over SHA-1 because:

- Consistent with the repository mainstream: metalink whole-file verification, incremental download, and S3 SigV4 payload hashing all use SHA-256 as primary;
- `FileHasher::calculate_streaming(path, HashAlgorithm::SHA256)` (EVP chunked 256KB, `packages/libfalcon-protocols/src/file_hash.cpp:65`) is ready-made, streaming GB-scale files without reading everything into memory;
- SHA-1 is retained only for the BT infohash compatibility surface; no new SHA-1-dependent surface is introduced.

Both the announce primary key and the query primary key are sha256; the filename is only a display and disk-naming suggestion (the same sha256 can have different names on different nodes).

### 6.3 Integrity Verification: Enforced at the Consumer

Bytes pulled from swarm sources are **treated identically to bytes pulled from any mirror**, entering the existing verification chain with zero privilege:

- metalink tasks: the completed file goes through the existing `verify_or_discard` semantics (the hash verification → rename → Completed convention in metalink_handler.cpp), falling back to serial mirrors on failure (existing behavior);
- Normal tasks + sidecar hash (§6.4): after download completes, `FileHasher::verify_streaming` verifies; failure follows the task's existing failure semantics;
- Normal tasks with no hash to verify: R1 sources **do not participate** (the hash-source rules of §6.4 guarantee "swarm sources always have a sha256 to verify" — the announce itself is hash-driven).

The value of this rule: swarm introduces no new trust plane. A bad node giving wrong bytes faces exactly the same outcome as a bad mirror giving wrong bytes — verification failure, source evicted, fallback; the completed file always passes verification before being published (the existing "completion = trustable" invariant).

### 6.4 Hash Sources (The Other Half of D6): When a Lookup Can Succeed

The core tension: **a normal HTTP download does not know the completed file's hash before completion** (no out-of-band hash), so "query swarm during download" is inherently impossible for normal tasks. Three tracks coexist:

1. **Announce after completion (main track, covers all tasks)**: task Completed → background `calculate_streaming(SHA256)` on the completed file → announce R1. Hash derivation is a background action that does not block the completion callback (the tens-of-seconds hashing cost of a GB-scale file only delays the announcement, not the download);
2. **Sidecar probing (enhancement track, covers "before download")**: when a `.sha256`/`.sha1` sidecar file exists next to the URL (community convention, `file.bin` ↔ `file.bin.sha256`), probe the sidecar before the task starts to obtain the hash → queryable throughout, swarm sources injectable during download;
3. **metalink (natural track)**: the document carries the whole-file hash, queryable throughout the task — **the only form in Stage 1 that supports "query during download"**.

Rule table:

| Task Form | Query-Injection During Download | Announce After Completion |
|---------|---------------|-----------|
| metalink | Yes (hash known out-of-band) | Yes (verify existing hash, no recomputation needed) |
| Normal + sidecar exists | Yes | Yes |
| Normal, no sidecar | No (can only download from traditional sources outside the Rendezvous) | Yes (derive after completion and announce) |

### 6.5 Permissions and Privacy Boundaries

- **Group system**: nodes are organized into groups (at least one, e.g., `family`) on the Rendezvous; announcements/queries carry group tokens (§9.3), invisible across groups. Multi-group support (one node in multiple circles) is deferred.
- **Filename visibility**: filenames are shared by default; a `share.hash_only` configuration omits filenames from R1 announcements (the consumer names the file by sha256, filename chosen by itself).
- **Sharing switch hierarchy**: global `--p2sp-share` (default **off**) → group-level (revoking the node on the Rendezvous side is exit) → no per-file switch (Stage 1; per-file granularity awaits real demand).
- **Off by default**: the same decision as the todo.md memo's "upload policy default off (learning from the Thunder public-relations lesson)". Enabling sharing is an explicit user action, and the switch controls both sides of "announce" and "be pulled from" (there is no intermediate state of "only upload without announcing" — the semantics are simple and auditable).

---

## 7. Rendezvous Service Responsibilities and API (D7)

### 7.1 Form: Independent Single Binary `falcon-swarmd` (Working Name)

- Single executable, configuration file `swarm.json` (aligned with the daemon.json convention: CLI > file > defaults), one-command self-hosting;
- Technical base reuses daemon's self-implemented stack: HTTP + WebSocket on the same port, JSON-RPC 2.0 dispatch, `token:` authentication, WS frame protocol (`websocket_frame.{hpp,cpp}`), optional SQLite persistence — **zero new dependencies**;
- Why not reuse the falcon-daemon process: different responsibilities (daemon is "my downloads", swarmd is "the circle's directory"), different lifecycles (daemon restarts should not shake the whole circle's directory), different deployment locations (swarmd resides on a cloud machine, daemon on every machine). The two share only the protocol family and library code.

### 7.2 Responsibility List (Five Things; Index-and-Leave)

1. **Identity registration**: new nodes register (public key + signature proof), recording node fingerprints and group membership;
2. **Peer directory**: maintain a liveness table of "node → reachability (direct/NAT flag/last_seen/version)";
3. **Resource index**: storage and TTL expiry of R1/R2 resource announcements; query by sha256 prefix / filename;
4. **Liveness heartbeat**: evict nodes on heartbeat timeout, jointly evicting their announcements (if the holder is gone, the announcement is meaningless);
5. **Change notifications**: WS push of resource additions/expiry and node up/down (consumers refresh the mirror pool immediately).

**Explicitly not done** (aligned with §3.3): byte relay, content caching, search ranking, social graphs.

### 7.3 API Plane (JSON-RPC 2.0, Dual-Carried over HTTP and WS)

Alongside daemon's aria2.* method family; the Rendezvous-side method prefix is `falcon.swarm.`:

```
POST /jsonrpc            # 与 daemon 同路径惯例;WS 升级亦在 /jsonrpc
Authorization: Bearer <server_token>     # 传输层准入(区别于节点身份层)
```

| Method | Direction | Purpose |
|------|------|------|
| `falcon.swarm.register` | Node→Rendezvous | Register/re-register (key identity + group token + reachability) |
| `falcon.swarm.challenge` | Rendezvous→Node | Signature challenge at registration (§9.2) |
| `falcon.swarm.heartbeat` | Node→Rendezvous | Liveness renewal (60s interval, 180s timeout) |
| `falcon.swarm.announce` | Node→Rendezvous | Batch announce R1/R2 resources (with TTL) |
| `falcon.swarm.retract` | Node→Rendezvous | Explicitly withdraw resources (when a file is deleted) |
| `falcon.swarm.query` | Node→Rendezvous | Query holding nodes by sha256 / filename |
| `falcon.swarm.unsubscribe` | Node→Rendezvous | Graceful deregistration (clears directory, notifies online nodes) |

WS notifications (Rendezvous→node, in JSON-RPC notification form):

| Notification | Trigger |
|------|------|
| `falcon.swarm.onResourceAdded` | New announcement in the circle (incremental; queries need no polling) |
| `falcon.swarm.onResourceExpired` | TTL expiry / holder offline |
| `falcon.swarm.onPeerJoined` / `onPeerLeft` | Node up/down |

HTTP GET health plane: `GET /v1/health` (consistent with the desktop IPC `/v1/health` convention; side-effect-free probe).

### 7.4 Rendezvous Storage Model

- Memory-primary: both the node table and the resource table are small-circle scale (hundreds of nodes × thousands of resources); in-process maps suffice;
- Optional SQLite persistence (aligned with the daemon TaskStorage precedent): after a Rendezvous restart the directory survives, avoiding a fleet-wide re-announcement storm; the resource table carries TTL fields, expired entries cleaned at load;
- Pure in-memory operation with no persistence configuration (after restart nodes self-heal via the re-registration mechanism, §12).

---

## 8. Wire Protocol and Message Formats (D8)

### 8.1 Design Principles

- **The full JSON-RPC 2.0 family**: the request/response/notification triad is fully isomorphic with daemon; both the client side (`WebSocketRpcClient` / JSON-RPC HTTP client) and the Rendezvous dispatch layer have ready-made code;
- **Signature coverage rules**: the Ed25519 signature is computed over `payload = method + "\n" + nonce + "\n" + sha256_hex(params_json)`, where `params_json` is compact JSON (key order = the field order of the examples in this document; the implementation fixes it with a canonical serialization function) — simple, unambiguous, easy to cross-check on both ends;
- All timestamps are UTC RFC 3339; all hashes are lowercase hex; node fingerprint = first 16 bytes hex of SHA-256(public key DER).

### 8.2 Registration and Challenge (Proving Identity)

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

The meaning of challenge-signature (why not "register = trust"): the `node_id` in the registration message is self-reported; the challenge receipt turns "holding the private key corresponding to that fingerprint" into a Rendezvous-verified fact, eliminating ID squatting (§9.2).

### 8.3 Announce (Batch, with TTL)

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

When `kind: "mirror"` carries the same sha256, the Rendezvous merges: resource `e3b0c442...` = {holding node NAS(direct), mirror URL(...)} — the querier gets all R1+R2 sources in one query, exactly the P2SP "multi-source" semantics.

### 8.4 Query (Returns the Source Set)

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

`direct:false` node sources are filtered by the querier (§5.2 rule 3); the protocol layer does not make decisions for the client.

### 8.5 WS Notifications (Incremental)

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

## 9. Security (D9)

### 9.1 Threat Model

The adversary profile of a private circle, ordered by realistic priority:

1. **Outsider eavesdropping/tampering** (a third party on the network path) — solved by transport encryption;
2. **Identity impersonation** (an outsider node pretending to be a circle member to enter the directory) — solved by challenge-signature + group tokens;
3. **Malicious/compromised in-circle nodes** (fake data, resource exhaustion) — solved by integrity verification (§6.3) + rate limiting/quota + manual revocation;
4. **Compromised Rendezvous** (worst case: the whole directory leaks) — mitigations: the directory contains only sha256/filenames/addresses, no content, no paths; TLS pinning guards against a man-in-the-middle Rendezvous impersonator;
5. **Explicitly not defended**: a trusted in-circle node deliberately distributing resources it obtained (technically unsolvable; the boundary of the trust model).

### 9.2 Authentication: Challenge-Signature (Ed25519)

- **Why asymmetric**: a pre-shared symmetric token only proves "is a member of the circle" and cannot distinguish nodes — no per-node audit, rate limiting, or revocation; one compromised node compromises the whole circle's credentials. The Ed25519 private key never leaves the machine; the Rendezvous stores only public keys;
- **Why Ed25519**: OpenSSL 1.1.0+'s EVP API has native support (`EVP_PKEY_ED25519`, and the project's dependency table is OpenSSL 1.1+); short signatures (64B) with fast verification, suited to the high-frequency signing of heartbeats/announces;
- **Challenge flow**: the four steps of §8.2. The nonce is generated by the node (anti-replay), the challenge is generated by the Rendezvous (anti-precomputation), and the signature object binds node_id and nonce;
- **ID squatting protection**: the fingerprint = hash of the public key; the same fingerprint necessarily means the same key pair; a registration that squats an existing node_id fails because the challenge cannot be signed.

### 9.3 Transport and Session

- **Mandatory TLS**: self-hosting has no CA, so **self-signed certificate + fingerprint pinning** — the node configures `server_fingerprint` (manually confirmed and recorded at first connection, the same trust model as SSH known_hosts); refuse a Rendezvous without TLS or with a mismatched fingerprint;
- **Session credentials**: successful registration issues a `session` (random 128b+); heartbeat/announce/query use the session to skip repeated signing; the session is bound to the node fingerprint with a TTL of 180s, renewed with heartbeats;
- **Group token**: validate `group_token` at registration (issued by Rendezvous configuration; a static string suffices — circle admission is a one-time manual action and needs no rotation protocol).

### 9.4 Content Liability Defenses (Product-Design Level, Heavier Than Any Cryptography)

- Sharing off by default (§6.5); enabling is an explicit configuration action;
- **No search**: the Rendezvous provides only "by sha256 / exact filename" queries — no full-text retrieval, no browse listings, no hot recommendations — the directory is inherently unusable as a content distribution entry point;
- Circle isolation: resources are mutually invisible across groups; the Rendezvous has no "public zone" concept;
- Rendezvous-side audit logs (who announced/queried what, when) are on by default, inspectable by the self-hoster.

### 9.5 Anti-Abuse (Rendezvous-Side Behavior Constraints)

| Means | Parameter (Default) | Description |
|------|-------------|------|
| Registration rate limit | 5 per IP per minute | Prevent registration storms |
| Announce rate limit | 60 requests/min per node, ≤256 entries per request | Heartbeat-period announces go in batch |
| Query rate limit | 120 requests/min per node | After consumer caching + WS incremental, query frequency is low |
| Resource quota | ≤10k announcements per node | Prevent directory flooding |
| Revocation | Rendezvous configuration blacklist(node_id) | Immediately evict the node and all its announcements, notifying online nodes with `onPeerLeft` |

Behavior over limits: HTTP 429 + JSON-RPC error (code -32002); the node side does exponential backoff (§12).

### 9.6 Node-Side Data Service Hardening (Stage 2, Pre-Set Constraints)

The embedded HTTP data service (§10.3) is read-only, recognizes only sha256 paths (`GET /by-sha256/<hex>`), and does not expose filesystem paths; when requests need group-membership validation, it is passed alongside the Rendezvous session (simplification: Stage 1 trusts same-subnet/private networks, and resource paths are not enumerable — the sha256 path itself carries 256 bits of entropy; the hardened stage supports per-request tokens).

---

## 10. Integration Points with the Existing Daemon/Engine (D10)

New node-side capabilities split into four modules, each with its integration point (evidenced by the 2026-09 code survey):

### 10.1 Configuration Plane (Aligned with the Existing Full-Chain Convention of file-allocation / seed-ratio)

| Plane | Integration Point | Fields |
|----|------|------|
| CLI | `arg_parser` + `main.cpp` help + `config_loader` trio (JSON read/write/merge) | `--p2sp-share`, `--swarm-server`, `--swarm-fingerprint`, `--p2sp-advertise` |
| daemon | `daemon/config.hpp` `DownloadConfig` section + `config.cpp` parsing + `main.cpp` apply + SIGHUP diff-replay | `p2sp.share` / `p2sp.rendezvous` etc.; the announce switch is SIGHUP-hot-updatable, the listen port is not (restart required, aligned with existing semantics) |
| RPC | `json_rpc_server.cpp` addUri key mapping (aria2-compatible key convention) | per-download `p2sp-share` override; `falcon.swarm.status`/`falcon.swarm.setShare` extension methods (desktop/browser extension controls sharing via the existing RPC plane, **zero new UI protocol**) |
| Key | `~/.config/falcon/swarm_key.pem` (mode 0600) | Generated on first enabling of sharing, persisted for identity stability |

### 10.2 Completion Hook: TaskManager Layer, Not the Engine Layer

**Decision**: the announce trigger hangs on `IEventListener` (TaskManager layer), not on V2 engine completion functions. Justification:

- The TaskManager layer's completion branch (the Completed path of `on_status_changed` in `task_manager.cpp`) already dispatches `dispatch_completed(task_id, output_path, total_bytes, elapsed)` — **the completed file path and total length needed for announcing are readily available in the event**;
- Coverage: V1 curl (daemon/CLI's **default** data plane, `http_engine` defaults to "v1") + V2 + metalink + other protocols, all covered in one shot; engine-layer completion (the two COMPLETED points of `complete_group_if_all_segments_done` and the 304 branch in `http_commands.cpp`) covers only V2 HTTP, and `RequestGroup::set_status` is a pure assignment with no hook to hang;
- Ready-made subscription pattern: daemon's `TaskStorageListener` (registered at daemon `main.cpp:358-361`) is the same trio; the raw pointer + `ListenerDetacher` RAII removal convention is copied directly;
- New listener `SwarmAnnouncer`: Completed → enqueue announcement (background thread) → `calculate_streaming(SHA256)` → announce. Hashing and networking are fully backgrounded, **zero added latency on the completion callback path**;
- BT seeding tasks only reach Completed when seeding ends (existing semantics); filter by URL/options to announce HTTP/metalink tasks only (BT resource sharing goes over the BT network itself, no duplicate announcements).

### 10.3 Inbound Data Service (Stage 2): Embedded Read-Only HTTP

- Form: an in-process read-only HTTP endpoint `GET /by-sha256/<hex>`, supporting Range (V2 P2SP pullers' segmented requests);
- The listen port follows the getsockname readback precedent of `json_rpc_server.cpp:555-561` (`port=0` random port, `port()` accessor for external use); the bind address defaults to private-network reachable (configurable);
- Data egress: completed files streamed directly (sendfile semantics); **Stage 2 does not do piece-level service** — it serves only "fully held" resources; half-finished files are never announced (§6.1);
- Completed files with known sha256 are hashed before announcement (the 10.2 background chain), so the service needs no full-disk hash scan at startup (announcement state is persisted in local SQLite and loaded at restart).

### 10.4 Query Injection: Swarm as a Mirror Candidate Source

- Injection point: the P2SP source discovery layer. Tasks with known sha256 (the three tracks of §6.4) `query` at startup → `sources` with `type:"node"` convert to the V2 multi-mirror URL form (`http://<addr>/by-sha256/<hex>`) and `type:"url"` enters the mirror pool directly, competing in the same pool as metalink mirrors under the same source-switching rules;
- Relation to metalink Stage 2: swarm sources are a **new source kind** for the mirror pool, not a new protocol — the metalink bridge's gates (whole-file hash + OpenSSL) are satisfied naturally (a swarm source always carries sha256);
- Segment-level source switching on failure, timeout sweeping, and If-Range all reuse the existing P2SP mechanics; a swarm source behaves no differently from any mirror source (the zero-privilege principle of §6.3 expressed at the integration layer).

### 10.5 Metadata Consumption (Value-Add, Non-Blocking)

R3 metadata is used for: duplicate-task hints within the circle ("this file already exists in the circle — fetch it directly?") and size previews before download. Exposed to desktop/extension via a daemon RPC extension method (`falcon.swarm.lookup`); deferred to Stage 3.

---

## 11. Failure Degradation

**Master principle: swarm is an enhancement, not a dependency.** Every degradation path terminates at "back to today's download behavior":

| Failure | Behavior | Recovery |
|------|------|------|
| Rendezvous unreachable | Node downloads proceed as usual; announce/query fail silently with exponential backoff (1s start, ×2, capped at 5min, with jitter); WS disconnects follow the existing `WebSocketRpcClient` reconnect cadence | Once the Rendezvous returns: re-register (identity key stable, session rebuilt) + full re-announce (local announce state persists; incremental gap-fill) |
| Rendezvous restart (no persistence) | Same as above; the re-registration storm is absorbed by register rate limiting (§9.5) + client backoff jitter | The whole swarm self-heals within the backoff window |
| Query returns nothing | Normal path: traditional sources (metalink document / the URL itself), no swarm involvement | WS `onResourceAdded` arriving later does **not** auto-retry started tasks (Stage 1 keeps the semantics simple; a manual user refresh triggers a fresh query) |
| Swarm-source pull fails / times out / fails verification | Segment-level source switching to the next source (existing P2SP semantics); node addresses that fail repeatedly go into a session-local blacklist | Forgotten when the session ends (announce TTL naturally refreshes holder state) |
| Heartbeat-timeout eviction | The Rendezvous evicts the node together with its announcements; WS `onPeerLeft` notifies the circle | Node side re-registers with backoff |
| Announce TTL expires unrenewed (node left) | The directory converges naturally | — |
| Local sharing switch off | Stop announcing and the data service; **already-announced resources are withdrawn gracefully via `retract`**; download/query behavior preserved (configurable `share.one_way`: consume without serving) | — |

Design trade-off: query results are not injected into running tasks (Stage 1), because "a new source joining mid-download" carries segment-reassignment complexity, and the existing P2SP already has a cross-segment-connection mechanism that could absorb it — left as a Stage 3 evaluation item (§13); the degradation matrix stays simple in the first stage.

---

## 12. Staged Milestones and Acceptance Boundaries

> Each stage is independently acceptable and independently mergeable; acceptance aligns with repo test conventions (loopback integration tests, self-contained on all three platforms, no external network dependency).

### Stage 0: Rendezvous Prototype + Identity Plane

**Scope**: the `falcon-swarmd` single binary (register/challenge/heartbeat/query/WS notifications + rate limiting); node-side key generation and `SwarmClient` (register/heartbeat/query, no announce).

**Acceptance**:
- Loopback e2e: two nodes + one Rendezvous (fixed test port / random port), registration challenge round-trip, heartbeat-timeout eviction, query returns the correct sources, WS notifications arrive;
- Injection surface: clean error paths for registration failures (wrong token / bad signature / rate limit hit), aligned with the repo `injection.hpp` convention;
- Wire-protocol cross-check: canonical-serialization and signature round-trip unit tests (aligned with the BT Base32 "independently generated" vector convention).

### Stage 1: Announce/Query + Completion Hook (Read-Only Enhancement, Zero Data-Plane Changes)

**Scope**: `SwarmAnnouncer` (TaskManager-layer listener + background hashing + announce), the full configuration chain (CLI/config/daemon/RPC), `retract`, TTL renewal.

**Acceptance**:
- e2e: node A completes a download → visible in the Rendezvous directory → node B's query hits (metadata consistent); A deletes the file → retract → B's query misses;
- **Degradation iron-rule acceptance**: kill the Rendezvous process; A/B download tasks (including in-progress ones) are unaffected throughout, results byte-for-byte identical;
- Default-off acceptance: the entire existing ctest suite unchanged (sharing off by default = regression proof that engine/event paths have zero intrusion).

### Stage 2: Inbound Data Service + P2SP Pulling (Data Plane Connected)

**Scope**: embedded read-only HTTP (§10.3), query injection into the mirror pool (§10.4), NAT-flag filtering.

**Acceptance**:
- e2e: a metalink task (or sidecar task) pulled in mixed segmented fashion from "swarm node sources + original mirrors", result byte-for-byte identical, whole-file hash verification passes, no temp residue;
- Bad-source rejection: a mock server impersonating a swarm source that returns wrong bytes → verification fails → source switched → result still correct (a direct test of §6.3's zero privilege);
- `direct:false` filtering: NAT-flagged nodes are not used as data sources, but their mirror URLs still participate;
- ASan full suite zero warnings (new network + threading code, aligned with repo convention).

### Stage 3 (Optional, Triggered on Demand)

mDNS LAN discovery, R3 metadata consumption (daemon extension method + desktop duplicate hints), dynamic mid-download source joining, multiple groups.

**Trigger assessment (2026-10-10 review, ruled under delegated user authority)**: remains untriggered. Basis: this section says verbatim "optional, triggered on demand" + the per-item trigger conditions in the §13 deferred table (dynamic source joining = frequency of new sources appearing mid-download proving the value; mDNS = the "Rendezvous unreachable but same subnet" scenario actually exists; R3 = assessed against desktop demand; multiple groups = demand for multiple circles appearing) + no corresponding product requirement recorded in the repo to date; Stages 1/2 already deliver the complete discovery-sharing-consumption chain, and no demand signal means no work starts.

---

## 13. Explicitly Not Done and Deferred

### Not Done (Design Rulings, Not Resource Issues)

| Item | Reason |
|----|------|
| Public infrastructure / public index | §3.2: content liability is an existential legal risk (Thunder/eMule prosecution history); the operating entity is misaligned |
| Global content search / browsing / trending | §9.4: the directory must never be usable as a distribution entrance; this is a design red line, not a feature gap |
| Anonymous participation | Fundamentally conflicts with identity authentication (D2); private circles have no anonymity need |
| Points / incentives / reputation systems | Small private circles need no game-theoretic machinery; incentive systems are artifacts of public networks |
| Rendezvous byte relay (TURN) | §3.3: pushes the Rendezvous back toward being a content-distribution form |
| End-to-end content encryption (encrypted sharing the consumer cannot verify) | §6.3's verification chain anchors on plaintext hashes; E2E encryption gets its own design if the need appears |
| Mobile nodes | Mobile-network NAT is deeply unreachable (§5); sharing value approaches zero |

### Deferred (Each with Trigger Conditions)

| Item | Trigger Condition |
|----|---------|
| NAT hole-punching family (UPnP→STUN→relay evaluation, §5.3) | User-measured experience significantly degraded by NAT'd nodes being unavailable as data sources |
| Piece-level sharing (partial-file exchange) | The "multiple machines all want it but nobody finished downloading" scenario for large files (GB+) becomes genuinely frequent in the circle |
| Dynamic mid-download source joining | Frequency of new swarm sources appearing during long-tail large-file downloads proves the value |
| mDNS LAN discovery | The "Rendezvous unreachable but same subnet" scenario actually exists |
| Multiple group memberships | Demand for one user in multiple circles appears |
| R3 metadata consumption | Assessed against desktop demand after Stages 1/2 land |

---

## 14. Risks and Open Questions

| Risk | Mitigation |
|------|------|
| Ed25519 unavailable on very old OpenSSL (<1.1.0) | Same gate as metalink hashing: without the capability the sharing switch greys out; downloads unaffected |
| Canonical JSON serialization differing between the two ends breaks signature verification | Unit-test round-trip cross-checks + field order pinned by wire-protocol examples (§8.1) |
| swarmd single point becomes a "must be online" component for the circle | The degradation matrix (§11) pins the blast radius of Rendezvous failure to the sharing-enhancement layer |
| Announce hashing competes with user I/O | Background thread + low-priority sequential reads (streaming 256KB chunks are sequential anyway); optional `share.hash_after_minutes` staggering |
| Embedded data service scanned from outside the LAN | Default bind to private network / designated interface; the sha256 path is not enumerable; the hardening stage adds per-request tokens (§9.6) |

**Open questions** (decisions needed before implementation; do not block the design):
1. URL privacy of R2 mirror announcements: mirror URLs may carry private tokens (e.g. signed object-storage URLs) — first version via user-configured exclusion rules (no filtering by default, documented warning), or regex filtering by default? Leaning toward the latter; to be settled when Stage 1 is implemented.
2. Session credential persistence: Rendezvous restart invalidates sessions and the whole swarm re-registers vs. persisting sessions for continued use — leaning toward the former (the re-registration protocol is itself an idempotent self-healing path; simplicity first).

---

## 15. Conclusion

**Form**: self-hosted private swarm — the user self-hosts `falcon-swarmd` (index plane, reusing the daemon's self-implemented HTTP+WS JSON-RPC stack with zero new dependencies), the node-to-node data plane connects directly (reusing every V2 P2SP mechanism), and the Rendezvous indexes and leaves.

**Ten decisions**:

| # | Decision | Core Argument |
|---|------|---------|
| D1 | Self-hosted private swarm | One answer to all three questions: content liability / operations / cold start (§3) |
| D2 | Ed25519 persistent identity; fingerprint as ID | Node-level audit/revocation; anti-squatting (§9.2) |
| D3 | Central Rendezvous authoritative; both DHT routes rejected | Small circles gain nothing from DHT; repo evidence shows zero DHT announce foundation plus structural public-exposure/NAT-broken defects (§4) |
| D4 | No traversal in Stage 1; puller connects actively + direct flag | Deployment forms cover the main scenarios; the local address infrastructure has zero foundation (§5) |
| D5 | Sharing plane = completed files / mirrors / metadata, three classes | Derived backward from data-plane needs; paths and history not shared (§6.1) |
| D6 | SHA-256 content addressing + consumer-enforced verification, zero privilege | Reuses `verify_streaming`; bad swarm sources meet the same fate as bad mirrors (§6.3) |
| D7 | Independent single binary; index-and-leave | Triple separation of responsibility / lifecycle / deployment (§7) |
| D8 | JSON-RPC 2.0 + `falcon.swarm.*` + WS notifications | Isomorphic to the daemon protocol family; both ends' code already exists (§8) |
| D9 | TLS pinning + challenge-signature + rate limiting/quota/revocation + sharing off by default | Full coverage of the private-circle threat model; default-off is a product red line (§9) |
| D10 | TaskManager-layer completion event drives announce; queries inject into the mirror pool | Covers the default V1 engine; `dispatch_completed` already carries path and total size (§10.2) |

**Roadmap**: Stage 0 (Rendezvous + identity) → Stage 1 (announce/query + completion hook, zero download-plane changes) → Stage 2 (data plane connected, mixed-source segmented pulling) → Stage 3 (on demand). Each stage is independently acceptable; Stage 1's "killing the Rendezvous is invisible to downloads" and "default off leaves the full test suite unchanged" are this design's two non-negotiable acceptance iron rules.

Reconciliation with the todo.md memo: Route C's "central index" lands in **self-hosted private** form (sidestepping the premises its legal and operations assessments assume); of the memo's three items, "index announce" and "upload policy (default off)" are fully designed here, and "inbound listening" is refined into Stage 2's embedded read-only data service; the memo's NAT traversal assessment (§5) and cold-start characterization (§3.2) each get their conclusions.

---

## 16. Stage 1 Implementation Design (Closure 2026-10-08)

This section turns the Stage 1 scope of §12 (`SwarmAnnouncer` + full configuration chain + `retract` + TTL renewal) into a design that can guide implementation, and rules on the two open questions of §14. Implementation proceeds in increments; each increment is committed only when fully green.

### 16.1 Open-Question Rulings (§14 → Settled)

1. **R2 mirror URL privacy = one-cut filtering of query/fragment**. When a mirror URL carries a signed token, the token almost always lives in the query (S3 presigned / OSS signatures / Kodo tokens all do) — the announce side applies a **minimum hygiene check to R2 URLs: any URL containing `?` or `#` drops that R2 entry (counted as rejected), keeping only R1**; non-http/https schemes are dropped the same way. No regex whitelist (high maintenance cost, false kills hard to diagnose). LAN addresses (RFC 1918) are not filtered — the P2SP main scenario is LAN/small circles, and internally reachable addresses are exactly legitimate announce content. `share.announce_mirrors: false` turns off R2 announcement wholesale.
2. **Session credentials are not persisted**. The re-registration protocol is itself an idempotent self-healing path (Rendezvous restart → whole swarm gets -32003 → re-register); the announce state machine keys off a **local intent table** (in memory): after re-registration succeeds, a full re-announce fills the gap. The key file is the identity; the session is only a lease.

### 16.2 Server Side (falcon-swarmd): announce/retract Semantics

**Signature form (diverging from the register challenge style)**: announce/retract are high-frequency (60/min) and idempotent in effect (upsert), so a challenge round-trip per request is not worth it. The signature payload follows the §8.1 formula, with the **nonce semantic slot carrying the session**:

```
payload = method + "\n" + session + "\n" + sha256_hex(canonical_params_without_sig)
```

Parameter integrity is bound by sha256(params) (anti-substitution — the same mechanism as register step2), identity is guaranteed by Ed25519; the replay window = re-upserting the same resource, with no security consequence (idempotent).

**announce(params = {session, sig, resources:[...]})**:
- Auth chain: Bearer (transport layer) → session validity (-32003) → signature verification (-32005). Verification failure follows the register convention.
- params validation: resources must be an array of ≤256 entries (§9.5), over-limit is -32602; each entry strictly schema'd (unknown fields rejected): `kind` ∈ {`file`,`mirror`}; file must carry sha256 (64 hex lowercase); mirror must carry url (http/https) + sha256; `ttl_s` defaults to 86400, clamped to [3600, 604800] (1h..7d).
- **Merge semantics** (§8.3): same sha256 → same resource. file entries upsert the node source (node_id = the node owning the session; direct/addr/agent taken from the registration state); mirror entries upsert the url source (etag/last_modified/accept_ranges optional). **The url source's owner is recorded as the announcer** (SwarmResourceSource.node_id reused as the owner field) — retract and node-eviction linkage depends on it.
- name/size: resource-level single values, **non-empty later write wins** (deterministic; a hash_only announce's absent name does not overwrite an existing name).
- expires_at: **max(current value, now + ttl_s)** — with multiple announcers coexisting, a short-ttl announcer must not shorten others' renewals; a single announcer's self-renewal semantics are unchanged.
- result = `{accepted, rejected, expires_at}` (resource-level expires_at, max over the resources hit, **value is remaining whole seconds** (numeric; an empty resources array is legal, accepted=0 and expires_at=0). An individually invalid resource (malformed hash / URL privacy-filtered) counts as rejected without failing the request; only params-level errors give -32602.
- **onResourceAdded trigger: broadcast only when a resource first appears** (params object, per §8.5); adding a source to an existing resource does not broadcast (the incremental semantic = new resource; source changes are covered by the query plane).

**retract(params = {session, sig, sha256s:[64hex,...]})**:
- Semantics: remove **all sources** this node holds on the resource (the node source + url sources this node announced).
- Resource sources emptied → delete the resource + broadcast `onResourceExpired` (§7.3 trigger table: TTL expiry / holder offline — explicit retract emptying falls under the same semantic); sources not empty (other nodes still hold it) → no broadcast. Unknown sha256 counts into unknown without error.
- result = `{removed, unknown}`.

**TTL expiry and node-eviction linkage (corrects the Stage 0 ruling)**: the sweep scans `resources_`; `expires_at <= now` → delete the resource + onResourceExpired (Stage 0's "expired resources give no notification" was a placeholder with a permanently empty table; Stage 1 makes it live per the §7.3 trigger table). sweep/unsubscribe evicting a node → evicts all its sources along with it; a resource whose sources empty → delete the resource + onResourceExpired.

**Rate limiting/quota**: a third RateLimiter instance (announce 60/min per **node_id** — a different key from register/query's per-IP; announce quota is counted against identity); active announcements per node ≤10k entries (counted by source ownership), over-quota rejects the whole request with -32002 (429 sent alongside, per the existing form).

### 16.3 Node Side (falcon-swarmd client): SwarmClient Extensions

- `SwarmError announce(const std::vector<AnnounceResource>&)` / `SwarmError retract(const std::vector<std::string>& sha256s)`; `AnnounceResource{kind, sha256, name, size, url, etag, last_modified, accept_ranges, ttl_s}`.
- The signature nonce slot carries the current session; non-zero-result rejected/unknown counts are left for the caller to interpret; network/protocol failures surface as SwarmError.

### 16.4 SwarmAnnouncer (New daemon Component)

New files `packages/falcon-daemon/src/daemon/swarm_announcer.{hpp,cpp}`, **compiled into the falcon-daemon executable target** (it depends on falcon_swarm_client→CURL; falcon_daemon_core does not link CURL and must not take it). Lifecycle follows the existing listener-registration form in main.cpp (the `TaskStorageListener` + `ListenerDetacher` RAII pattern, main.cpp:374-389 precedent).

**Completion hook (D10)**: implements `IEventListener::on_completed(task_id, output_path, total)` → enqueues into the background hash queue; hashing (`FileHasher::calculate_streaming`, SHA-256) and networking all run on the announcer worker thread, **zero added latency on the completion callback path**. Optional `share.hash_delay_s` staggering.

**Filter chain (in order)**:
1. `share.enabled` false, or `one_way` (consume only) true → do not announce;
2. per-download override: options `p2sp_share` (three-state ""/"true"/"false", "" = follow global);
3. `magnet:` prefixed tasks are not announced (BT resources go over the BT network's own discovery plane; `.torrent` file tasks announce as ordinary files, self-consistent — others can obtain the torrent file over P2SP, no collateral damage);
4. Hash failure (file vanished / read error) → abandon this entry, no retry, no alert escalation (one WARN).

**Announce content**: R1 (file, sha256, name/size per mode) + when the task URL is http(s) and passes the privacy filter (16.1), attach R2 (mirror, url = task URL; etag/last_modified not available at the task layer, defaults; accept_ranges omitted by default, not announced). `share.mode: "standard"` announces name+size; `"hash_only"` omits size too (size is a side channel for guessing content).

**TTL renewal loop (announcer thread)**: period = min(5min, ttl/3). Each round, for each active announcement:
- File exists and (mtime,size) matches the announce-time values → re-send announce (renewal);
- File gone → retract + remove from the active table (the retract entry point of e2e acceptance iron rule (a));
- (mtime,size) changed → retract the old hash + re-hash and announce the new hash (the old resource is not left dangling);
- `SwarmClient::session()` changed (detected in the renewal loop) → **full re-announce** (the gap-fill path of §16.1 ruling 2);
- announce failure → exponential backoff (1s start, ×2, capped at 5min, with jitter); -32003 needs no special handling (the SwarmClient heartbeat thread re-registers automatically, and announce naturally succeeds once healed).

**Shutdown semantics**: stop() halts the hash queue and the renewal loop but **does not retract** — shutdown is not file deletion; after re-registration a full re-announce fills the gap; announcements leaked by abnormal shutdown self-heal under the TTL ceiling (7d). Hot-toggling `share.enabled` off → full retract, then stop (§11 "graceful withdrawal").

**Private key**: `~/.config/falcon/swarm_key.pem` 0600, via the existing `SwarmKeyStore` load_or_create; a daemon+CLI on the same machine sharing a key will kick each other's sessions (each side's heartbeats ping-pong -32003); self-healing, no damage, documented as known behavior.

### 16.5 Full Configuration Chain

**daemon (daemon.json `p2sp` section; wholly absent by default = sharing off)**:

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

- Type errors exit with an error (before daemonize), unknown keys warn — per the existing config split; an invalid `mode` warns and keeps standard (aligned with the http_engine precedent).
- SIGHUP: all `share.*` hot-reload (enabled → false triggers full retract); `rendezvous.*` changes → "restart required" warning.

**RPC (Stage 1 increment 4)**: `falcon.swarm.status` (announcer state snapshot: enabled/registered/node_id/session/announced_count/queue_depth) + `falcon.swarm.setShare` (params {enabled}; a runtime toggle with the same semantics as hot reload); addUri per-download option `p2sp-share` ("true"/"false" strings, aria2 style); consumption point = the announcer filter chain, persisted as a trailing TaskStorage options JSON field.

**CLI (Stage 1 increment 4)**: `--p2sp-share` / `--swarm-server host:port` / `--p2sp-advertise addr` wired through the config_loader trio; `--swarm-fingerprint` parses but warns and ignores at startup (the Stage 0 client has no TLS; forward declaration); the CLI one-shot form has **no renewal and no data service** — after the download completes it hashes synchronously (blocking; the CLI has no background host) → announce once → exit; announce failure WARNs without failing the download; the resource-availability window = min(TTL, before exit), honestly disclosed. The daemon is the first-class form of sharing; CLI announce is best effort.

### 16.6 Test Design (Preventing Same-Bug Self-Corroboration)

- **Server pure unit** (virtual clock): announce upsert merge / node-url source ownership / expires_at max semantics / name later-write-wins / ttl clamping / rejected vs -32602 triage / retract partial removal vs empty-and-delete / eviction linkage / TTL sweep notifications / per-node rate limiting / 10k quota / signature failure -32005 / nonce=session payload cross-check.
- **Loopback wire protocol**: real-socket announce→query hit with metadata consistent (first half of iron rule (a)) / retract → miss / announce notification truly arrives over WS / the two onResourceExpired trigger surfaces / 429 and -32002 sent together.
- **client**: SwarmClient announce/retract × loopback Rendezvous (signature round-trip with real verification).
- **announcer**: hash queue (fake Hasher injection) / every filter-chain branch / the three (mtime+size)-change branches (renew/retract/re-announce) / session-change full gap-fill / backoff (virtual clock or short intervals) / shutdown without retract.
- **e2e acceptance (iron rules, closed in increment 4)**: (a) A completes → B's query hits (metadata consistent) → A deletes the file → retract → B misses; (b) kill the Rendezvous process; A/B download tasks (including in-progress) unaffected throughout, results byte-for-byte identical; (c) default off, the entire existing ctest suite unchanged.

### 16.7 Increment Split

| Increment | Content | Gate |
|---|---|---|
| 1 | This section's design committed to the doc (done) + swarmd server announce/retract/sweep linkage/notifications/rate limiting/quota | swarmd unit + loopback all green (✅ done 1b0a13f) |
| 2 | SwarmClient announce/retract + tests | swarmd four targets all green (✅ done) |
| 3 | SwarmAnnouncer + daemon p2sp config section + main wiring + SIGHUP | daemon full suite + swarmd all green (✅ done 2026-10-09) |
| 4 | RPC falcon.swarm.status/setShare + addUri p2sp-share + CLI argument chain + the four e2e acceptance rules + doc reconciliation (daemon/swarmd CLAUDE.md, README) | repo-wide ctest all green + e2e iron rules (✅ done 2026-10-09: the three e2e iron rules 3/3 + 10/10 stress runs, target `falcon_daemon_swarm_e2e_tests`) |

---

## 17. Stage 2 Implementation Closure (2026-10-10)

Stage 2 (inbound data service + P2SP pulling) is implemented, in six commits: `8c7ee56` (data service),
`e3bb66a` (query injection into the mirror pool), `d141b95` (bad-source rejection nail), `64f5097` (winsock2
build fix), `102d4e1` (query plane independent of the sharing switch), `ca09aa3` (e2e mixed-pull iron rule).

### 17.1 Landed Form vs. Design-Item Reconciliation

| Design Item | Landed | Reconciliation/Deviation |
|---|---|---|
| §10.3 inbound read-only HTTP `GET /by-sha256/<hex>` + Range | `swarm_data_service.{hpp,cpp}` (daemon): accept thread + detached-connection-thread ledger (per the 2026-10-01 lifecycle discipline); the registry is filled via `ISwarmDataRegistry` by the announcer's "completed → hashed → announced" chain; half-finished files are never registered; the file is exposed only by sha256, never by local path | Method whitelist GET/HEAD (HEAD is an implementation bonus); one request per connection (`Connection: close`, no keep-alive); 404 (unregistered / path mismatch) / 400 (malformed hex) / 405 idempotent semantics all tested |
| §10.3 bind address | `make_data_service_config`: `p2sp.rendezvous.advertise_addr` shaped "ip:port" → binds on the advertised port (announce and actual listen agree, e2e probes directly); empty/unparseable → 0.0.0.0:0 random port + `port()` readback | Bind failure warns without blocking (the data plane is an incremental capability; a refused connection at the far end is absorbed by segment-level source switching) |
| §10.4 query injection into the mirror pool | protocols public seam `mirror_source.hpp` (function callbacks; protocols does not depend backward on the daemon; with no host registered there is zero query surface); daemon-side `SwarmSourceProvider`: node sources → `http://<addr>/by-sha256/<hex>`; url sources (http/https) passed through as-is; the metalink V2 bridge mirror pool = document mirrors first (If-Range owner) + deduplicated swarm sources appended | **Enhanced semantics: no query when the document mirror pool already has ≥2** (§10.4 only says inject, not when to query — source supplementation happens only when the pool is insufficient, zero redundant queries); the resume path ignores swarm-source drift (breakpoints are not invalidated); segment-level failure source switching / timeout sweeping / If-Range all reuse the existing P2SP mechanics, swarm sources carry zero privilege |
| §10.5 NAT-flag filtering | provider `node_source_usable`: missing advertise object / empty addr / no ':' / `direct==false` are all skipped; the local machine's own node_id is skipped | url sources bypass NAT filtering (mirror URLs still participate, per design) |
| §11 degradation | Query failures back off exponentially 30s×2^n capped at 300s; no queries hit the network during suppression; any success clears it; the query failure surface leaks nothing (exceptions/shape mismatches all collapse to an empty table — a discovery-plane failure never blocks downloads) | **The query plane is independent of the sharing switch** (102d4e1): a process-level shared SwarmClient lazily created + idempotent start (start_mutex_); with share off, announcing stops and the data service stops, but queries stay alive — the landing of §11's one_way semantics; the shutdown callback unwires the seam before draining (in-flight queries hold shared_ptr copies that extend lifetime, no dangling) |
| §12 four acceptance items | See §17.2 | All pass |

### 17.2 Test and Acceptance Reconciliation

- **Gates and logging**: V2 multi-source only when document mirrors + swarm sources total ≥2, otherwise fall back to serial delegation (same as the single-mirror behavior before Stage 2); skip reasons logged by tier (pool insufficient / seam unregistered / query exception).
- **Provider pure unit, 12 cases** (`swarm_source_provider_test`): parse_sources shape tolerance across all branches (node/url/unknown type/non-object/dedup/self-exclusion) / the `node_source_usable` direct matrix / backoff state machine (failure suppresses → success clears; no network queries during suppression).
- **Data service, 12 cases** (`swarm_data_service_test`, POSIX loopback): random port full coverage / exact Range slicing / HEAD with no body / unregistered 404 / malformed hex 400 / path 404 / method 405 / unregister and clear / completed file missing 404 / idempotent stop / concurrent clients.
- **metalink bridge +7 cases** (`metalink_handler_test`): swarm sources extending the pool enable multi-source / duplicates of document mirrors are not re-admitted / provider exceptions fall back to serial / non-http sources filtered / empty results / pool sufficient, no query / **bad-source rejection** (d141b95: `SwarmSourceBadBytesRejectedByWholeFileHash` — a swarm source serving wrong bytes → whole-file hash verification fails → falls back to serial over document mirrors → result still correct, the direct test of §6.3's zero privilege).
- **e2e mixed-pull iron rule** (ca09aa3, `SwarmDaemonE2E.MetalinkV2BridgePullsSwarmSourceIntoMirrorPool`): A (share on + advertise_addr) completes from the legacy mirror and announces a node source; B (share off + http_engine=v2) gets a single document mirror that refuses Range (416) — the discriminator that makes the serial path structurally doomed; B's swarm query injects the node source → V2 multi-source segmented mixed pull; assertions: on the document mirror, plain GET ≥1 and Range GET ≥1 (Range only appears under V2 multi-segment rotation; the serial-fallback form cannot produce it) + result byte-for-byte identical + no `.falcon.*` residue + all three processes exit cleanly. Register rate limiting counts per-IP against RPC calls (A's announce + probe + B's query = 3 clients), raised explicitly in e2e.
- **ASan**: swarm full suite + metalink bridge 198/198, daemon/RPC 233/233, zero warnings (including real-binary e2e 4/4).
- **CI**: all 8 jobs green (ca09aa3, run 37996089515).

### 17.3 Known Boundaries (Honestly Disclosed)

- The data service is a minimal hand-written HTTP/1.1 (status line + Content-Length/Content-Range), no keep-alive; it serves only complete published files, no piece-level service (§10.3 ruling);
- §11's "repeat-offender nodes into a session-level blacklist" is not done — bad sources are rejected wholesale by whole-file hashing; a repeat bad source merely wastes retry budget, left for Stage 3 evaluation;
- Query injection happens only at download start (first attempt / fallback); running tasks do not gain sources (§11's explicit Stage 1 semantic; dynamic source joining left for Stage 3);
- e2e is a real three-process topology (2 daemon + rdv, fork-dependent), POSIX-only (same posture as Stage 1); Windows is covered by the pure unit and loopback cases of the provider/data service/bridge.