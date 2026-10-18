[中文](mcp_server_design.zh.md) | **English**

# Falcon MCP Server Design (AI-Assistant-Driven Downloads)

Status: **Phase 2 implemented** (closed out 2026-10-08: increment 1 global-option and
seeding tools (13 tools), increment 2 stdio thin shell (`falcon-mcp`), increment 3
progress subscription (`GET /mcp` SSE notification stream); unit tests + real-binary
smoke tests closed out; for the Phase 2 reconciliation see §8, the Phase 1
reconciliation is kept as an archive).
Research reference: [Lakr233/FlowDown](https://github.com/Lakr233/FlowDown)
(AGPL-3.0, ★1.2k). Only the interface design and interaction patterns were used as
reference; no code was copied.

---

## 1. Research Findings (FlowDown Reference)

### 1.1 What FlowDown Is

A **native AI chat client** for Apple platforms (iOS/iPadOS/macCatalyst): Swift 6 +
UIKit, local MLX inference + OpenAI-compatible endpoint access, with an aggressive
privacy stance (zero telemetry, zero data retention, auditable source code).
**It is not a downloader**: its only download capability is "downloading the AI
models themselves" (see 1.4).

### 1.2 Feature Inventory

| Feature area | Shape | Relation to falcon |
|---|---|---|
| AI chat | Multi-model conversation, Markdown rendering, vision/audio attachments, templates | falcon does not do this (not its positioning) |
| Model access | OpenAI-compatible API, Ollama/LM Studio local inference, Apple Intelligence, `.fdmodel` config import/export | falcon does not do this |
| Tool calling (ModelTools) | 16 built-in tools + externally connected MCP tools, on the same tool-calling pipeline | **The MCP part is the core reference for this design** |
| Built-in tools | Calendar (2) / Reminders (5) / Web search + fetch (2) / Location (1) / Long-term memory (5) / URL metadata | falcon does not do this (not its positioning) |
| MCP | **Pure client**: connects to external MCP servers and injects their tools into the model | See 1.3 for details |
| Downloads | Only HuggingFace model file downloads | See 1.4 |
| External automation | Apple Shortcuts (App Intents), Live Activity background streaming | falcon does not do this (platform-bound) |
| Sync | iCloud (CloudKit) conversation/config sync | falcon does not do this |
| Privacy | Zero telemetry, zero data retention | falcon shares this stance (it has no telemetry to begin with) |

### 1.3 Its MCP Implementation (Research Focus)

**Correction to the research premise: FlowDown has no "MCP server"; it is a pure MCP
client**, letting AI models call tools provided by external MCP servers.
`FlowDown/Backend/MCPService/` (~1670 lines) + 6 unit-test files, wrapping the
official swift-sdk `MCP` package. The website's `.well-known/agent.json` is an agent
discovery card (describing the website itself), not an MCP endpoint. Its external
automation surface is in fact carried by Apple Shortcuts.

Client design highlights (of direct reference value for falcon's reverse design):

- **Transport supports Streamable HTTP only** (POST `/mcp/`): the sandbox forbids
  stdio and SSE is deprecated; both are explicitly not done, avoiding ambiguous user
  expectations. Per-server configuration: `endpoint` / `headers` (JSON map, auth
  credentials sent with every request) / `timeout` (seconds, default 60) /
  `nickname` / `isEnabled` + **tool-level switches** (the toolsEnabled/
  resourcesEnabled/templateEnabled trio).
- **Three-piece connection-management robustness** (`MCPService.swift`):
  ① Bounded waiting: `conversationWaitTimeout` of 15s; during session preparation a
  single hung server only loses its own tools instead of dragging down the whole
  round of conversation (original comment: "a hung server loses its tools for this
  round instead of hanging the whole conversation setup");
  ② Session-loss self-healing: when `callTool` fails, first try replacing the
  connection (`replaceConnectionIfSessionLost`), briefly wait for the new session so
  later calls in the round reconnect; if replacement fails, keep the dead connection
  and settle it as an ordinary tool error;
  ③ Tool-list cache: `toolInfoCache` caches listTools results per serverID; while
  unexpired, no re-query.
- **Onboarding UX**: the `Verify Configuration` button = a one-shot handshake +
  listTools verification; the list shows four-state badges
  Connected/Connecting/Disconnected/Connection Failed; `.fdmcp` file
  import/export/sharing (a server-configuration snapshot, importable directly via
  AirDrop/file open).
- **Invocation pipeline and confirmation**: MCP tools and built-in tools share the
  same `ModelTool` protocol; the model sends a function name + JSON arguments →
  confirmation dialog (showing server name/tool name/description, with the global
  Skip Tool Confirmation exemption) → execution → result text inlined into the
  reasoning panel, image/audio content converted into attachments. **Confirmation
  authority sits with the host**; tool providers do not re-implement it.

### 1.4 How Its Download Capability Is Organized

`ModelManager+Hub.swift`: HuggingFace `/api/models/{id}/tree/main?recursive=1` lists
the file tree → each file is handed to `Digger` (a third-party download library) for
multi-task downloading → `HubDownloadProgress` aggregates overall progress/per-file
progress/speed/cancellation. No resume control plane, no queue, no multi-source; it
is a one-shot tree download, discarded afterward. Nothing directly reusable for
falcon (falcon's general download engine is far beyond it); the only takeaway is the
"tree browsing + batch download + aggregated progress" interaction pattern
(corresponding to falcon's existing resource browsing/batch adding).

### 1.5 Reverse Positioning for falcon

FlowDown's direction: **let AI call external tools**. falcon's direction is the
opposite: **expose itself to AI as a tool**. The user says in an MCP host such as
Claude Desktop / Claude Code / Cursor, "download this link to the D drive and tell
me when it's done", and the host calls falcon's tools over MCP to complete
adding/querying/control. falcon only needs to implement the **MCP server**; tool
confirmation, multi-turn orchestration, and argument completion are all borne by the
host, and falcon stays a pure tool surface.

---

## 2. Protocol Contract

### 2.1 Transport and Shape

| Item | Decision | Rationale (following the FlowDown precedent) |
|---|---|---|
| Transport | **Streamable HTTP**: `POST /mcp` (JSON-RPC 2.0 request; response `application/json` or `text/event-stream`) | MCP's currently promoted shape; FlowDown does only this one, the best engineering cost and compatibility surface |
| stdio | Phase 2 optional thin shell (`falcon-mcp` forwards commands to the daemon RPC; **implemented, 2026-10-08 increment 2**: does not link libfalcon-core, transport injected via `HttpPostFn`) | The AI coding tool (Claude Code etc.) ecosystem largely assumes stdio; but the daemon already has an HTTP surface, so the thin shell is pure forwarding |
| Endpoint | A new `/mcp` route on the daemon's existing port (RPC port 6800) | Reuses the existing listener/auth/token configuration; no extra port opened |
| Session | Implement the `Mcp-Session-Id` response header + validation on subsequent requests | Required by the Streamable HTTP spec; the session table is bounded to prevent leaks |
| Auth | `Authorization: Bearer <token>`, reusing the daemon's `rpc.secret`; when the daemon has RPC disabled, `/mcp` is disabled along with it | Single source of truth; no second credential scheme invented |
| Initialization | `initialize` → returns `capabilities: { tools: {} }` + `serverInfo: { name: "falcon", version }` | The client (host) negotiates by capability; only tools are exposed |
| Protocol version | Negotiate `protocolVersion`; support the current stable version; unrecognized versions fall back per the spec | |

### 2.2 Result and Error Conventions

- Every tool returns `content: [{type:"text"}]` (a human-readable summary) +
  **`structuredContent`** (machine-readable JSON, consistent with the text content);
  the host's model uses both: text goes into the conversation, the structured object
  is consumed programmatically.
- Business errors (gid not found / task not in the paused state) return
  `isError: true` + explicit text; JSON-RPC-level error codes are **not** used to
  express business failures (protocol-level errors are reserved for protocol
  violations); this is translated in alignment with the daemon RPC's code semantics.
- Bytes/durations are always named with explicit units (`total_length_bytes`,
  `seeded_seconds`); speed carries a `bytes/s` semantics note in the tool
  description, to keep the model from misreading it.

---

## 3. Tool List (Phase 1: Ten + Phase 2 Increment 1: Three = 13, Mapped to the Existing daemon RPC)

Named with the `falcon_` prefix; parameter/return JSON Schemas are all explicit
(`additionalProperties: false`). The mapping column lists the existing aria2-compatible
RPC methods; the MCP layer is a **thin adapter** and writes no new business logic.

| Tool | Parameters (key points) | Return key points | Mapping |
|---|---|---|---|
| `falcon_add_download` | `urls[]` (required), `output_dir?`, `filename?`, `options?` (a whitelist of aria2 keys such as proxy/limit/retry/seed) | `gid`, `task_id` | `aria2.addUri` |
| `falcon_list_tasks` | `status?` (active/waiting/stopped/all), `offset?`, `limit?` | Snapshot array (gid/status/progress/speed/size/output path) | `tellActive`+`tellWaiting`+`tellStopped` aggregation |
| `falcon_get_task` | `gid` (required), `keys?` | Full snapshot of a single task (including BT seeding extension fields) | `aria2.tellStatus` |
| `falcon_pause_task` | `gid` | `gid` | `aria2.pause` |
| `falcon_resume_task` | `gid` | `gid` | `aria2.unpause` |
| `falcon_remove_task` | `gid`, `force?` (default false; force uses `forceRemove`) | `gid` | `aria2.remove`/`forceRemove` |
| `falcon_pause_all` / `falcon_resume_all` | none | `OK` | `pauseAll`/`unpauseAll` |
| `falcon_get_global_stats` | none | Active/waiting/paused counts, upload/download speed | `aria2.getGlobalStat` |
| `falcon_get_task_files` | `gid` | File list (path/length/completed portion) | `aria2.getFiles` |

Phase 2 additions (dependent on Phase 1 stabilizing; not in the first batch):

| Tool | Description | Mapping |
|---|---|---|
| `falcon_set_global_option` / `falcon_get_global_option` | Global speed limit/concurrency etc. | `changeGlobalOption`/`getGlobalOption` (**implemented, 2026-10-08 increment 1**: the key whitelist is not duplicated at the MCP layer; daemon-side business errors are mapped through isError) |
| `falcon_stop_seeding` | Manually stop BT seeding | `falcon.stopSeeding` (**implemented, 2026-10-08 increment 1**: code 1/2 → isError) |
| Progress subscription | Progress/status notifications pushed via the `GET /mcp` SSE notification stream (one `data: <JSON-RPC envelope>\n\n` line per notification, the same envelope body as the WS broadcast), replacing model polling (**implemented, 2026-10-08 increment 3**: auth/session admission has the same semantics as POST/DELETE; for reconciliation see §8's Phase 2 section) | Reuses the existing RpcEventBridge |

**Polling guidance written into the tool description**: `falcon_add_download`'s
return description states explicitly "poll falcon_get_task(gid) for
status==complete"; the model decides the pacing itself, and the host side has a
timeout backstop. From Phase 2 increment 3 on, the `GET /mcp` SSE notification
stream can push proactively (see the table above); the polling guidance is kept for
hosts that do not support streaming subscription.

**Destructive annotations**: `falcon_remove_task`/`falcon_pause_all` are marked
`annotations: { destructiveHint: true }`; `falcon_add_download` is marked
`readOnlyHint: false, openWorldHint: false`. The confirmation interaction is left to
the host (FlowDown's confirmation dialog is likewise a host responsibility; the tool
side does not re-implement UI).

---

## 4. Architecture Placement

- **Where it lives**: the MCP server hangs off the **daemon** (the `falcon-daemon`
  package). Desktop InProcess mode does not expose MCP: MCP's value presupposes a
  resident service (AI hosts connect at any time), while the desktop in-process
  engine's lifetime follows the GUI, so exposing it there is meaningless. daemon.json
  gains an `mcp` section (`enabled`, default false; the endpoint reuses the rpc
  section's port).
- **Implementation path**: the daemon's existing `JsonRpcServer` dispatcher is
  reused directly; the MCP layer does three things: ① the `/mcp` route hookup
  (handshake/session/Streamable HTTP framing); ② a static `tools/list` manifest
  (10 schemas, compile-time constants); ③ a name→aria2-method parameter translation
  table for `tools/call`. **Zero new business logic**.
- **Robustness implements the FlowDown client trio in reverse** (server-side view):
  the `tools/list` response is cached in memory (static manifest, zero cost); the
  session table is bounded + an invalid `Mcp-Session-Id` gets 404 and cleanup (leak
  prevention); single-session request timeouts follow the daemon RPC's existing
  handling.
- **stdio thin shell** (Phase 2; **implemented, 2026-10-08 increment 2**,
  `falcon-daemon/tools/`): a standalone small binary `falcon-mcp` reads stdin and
  writes stdout, forwarding internally to the daemon's HTTP RPC; it does not link
  libfalcon-core, avoiding a second in-process engine shape. Transport is injected
  via `HttpPostFn` (a curl implementation and a unit-test fake share the same bridge
  logic).
- **Not doing**: falcon itself will not be an MCP host/client (it does not connect
  to external MCP servers; falcon is not an AI chat product, and FlowDown's
  MCPService has no mirror value); no built-in AI tools (calendar/search/memory).

---

## 5. Security

- Default `127.0.0.1` + Bearer token (reusing rpc.secret); when no secret is
  configured, `/mcp` rejects all requests.
- Minimal tool surface: the first batch of 10 tools excludes
  `forceShutdown`/`saveSession`/`purgeDownloadResult`; this is unnecessary in
  AI-driven scenarios and shrinks the mis-operation surface (when needed, the user
  calls them directly over the daemon RPC).
- Lower bound against prompt injection: tool descriptions concatenate no runtime
  data (static strings), and progress/status are returned only via
  structuredContent, so the model will not treat server-returned content as tool
  instructions (a host-side responsibility; falcon does not amplify it).

---

## 6. Feature Gap Table (FlowDown Has / falcon Lacks)

| # | FlowDown capability | falcon status | Do / don't | Rationale |
|---|---|---|---|---|
| 1 | MCP client (connects to external MCP servers to extend AI tools) | None | **Don't** | falcon is not an AI chat product; falcon's direction is MCP **server** (this document) |
| 2 | MCP server / exposing itself as a tool | None | **Do** (Phase 1: 10 tools) | Core of this document; lets AI hosts drive downloads |
| 3 | Tool confirmation dialog + Skip Tool Confirmation | None | **Don't** | Confirmation authority belongs to the MCP host; the tool side does not re-implement it (same for FlowDown) |
| 4 | `.fdmcp` server-config import/export/sharing | None | **Do a simplified version** | The README provides per-host (Claude Desktop etc.) connection JSON snippets + one-click copy; no need to invent a file format |
| 5 | Bounded waiting / session-loss self-healing / tool-list cache | Partial (RPC-layer timeouts already exist) | **Do the corresponding items** | Server-side mirror: bounded session table + invalidation cleanup, list caching; client self-healing has no counterpart |
| 6 | HuggingFace model tree browsing + per-file downloads + aggregated progress | No dedicated feature | **Don't** | falcon already has the general multi-source download engine (metalink/segmented/BT); dedicated HF integration is not the positioning; when needed, the user can paste a direct link/metalink |
| 7 | Built-in AI tools (calendar/reminders/location/memory/web search + fetch) | None | **Don't** | Not falcon's positioning |
| 8 | Apple Shortcuts / Live Activity / iCloud sync | None | **Don't** | Platform-bound; the external automation surface is carried by MCP (cross-platform) |
| 9 | Zero telemetry / zero data retention | Already have it (no telemetry at all) | Stance aligned | — |
| 10 | Multi-protocol download engine (resume/multi-source/rate limiting/seeding) | **falcon has it, FlowDown lacks it** | Already have it | The reference has no counterpart; not a gap |

---

## 7. Implementation Boundary

- **Phase 1** (one increment): the daemon `/mcp` route + initialize/handshake/session
  + `tools/list` (10 tools) + the `tools/call` translation layer + the daemon.json
  `mcp` section + unit tests (handshake round-trip / manifest schema validation /
  translation-table parameter round-trip / error semantics) + real-host smoke tests
  (MCP inspector loopback). The README gains the onboarding snippets (gap table #4).
- **Phase 2** (closed out 2026-10-08): increment 1 global-option and seeding tools
  **implemented** (13 tools, see the §3 additions-table annotations); increment 2
  stdio thin shell **implemented** (`falcon-mcp` + the `falcon_daemon_mcp_stdio`
  bridge library, see §2.1/§4 annotations); increment 3 progress subscription
  **implemented** (the `GET /mcp` SSE notification stream, see §3 and §8's Phase 2
  reconciliation).
- Explicitly not doing: MCP client, built-in AI tools, an embedded conversation
  interface in the GUI.
- Documentation reconciliation: when the implementation lands, daemon
  CLAUDE.md/README are annotated "implemented" in sync and this document's status
  line is rewritten; before that, this document keeps the "design draft" wording
  (no fake documentation).

---

## 8. Implementation Reconciliation (Phase 1, 2026-10-07)

Differences between the Phase 1 implementation and this document's contract are
disclosed honestly (everything not listed matches item by item):

- **Response shape**: Phase 1 returns only `application/json`; the
  `text/event-stream` response stream is not implemented (progress subscription
  belongs to Phase 2); `GET /mcp` (the SSE listening slot) returns 405 +
  `Allow: POST, DELETE`. (The 405 shape went away when Phase 2 increment 3's SSE
  notification stream landed; unknown methods other than GET still get 405.)
- **Protocol version**: supports `2025-06-18` (default) and `2025-03-26`; a higher
  version declared by the client falls back to the default version.
- **`falcon_list_tasks` aggregation shape**: `status=waiting|stopped` uses aria2's
  native `(offset, limit)` pagination; `all` (default) = `tellActive` (full) +
  `tellWaiting(0,100000)` + `tellStopped(0,100000)`, the three queues merged and
  then sliced uniformly; aria2's three tell* queues have no unified pagination
  cursor, so the aggregation can only be sliced at the translation layer.
- **Auth baseline**: with `mcp.enabled` and **no `rpc.secret` configured, `/mcp` is
  403 across the board** (the landing form of §5's "when no secret is configured,
  reject all requests"); with a secret configured, `Authorization: Bearer` applies,
  and failure gives 401 + `WWW-Authenticate: Bearer`. Note the semantic difference
  from `/jsonrpc`: the latter skips validation and lets requests through when no
  secret is configured. Auth runs before method routing (a credential-less GET gets
  401 rather than 405).
- **Session table**: an in-process in-memory table, capped at 64 (oldest evicted
  when full); `initialize` ignores any session header the client sends and always
  creates a new session.
- **Batch and scalars**: an array batch gets 400 + -32600 (batch was removed in
  2025-06-18); a non-object scalar body gets 200 + a -32600 envelope (caught by the
  JSON-RPC dispatch's request-shape validation).
- **Smoke-test shape**: the design says "MCP inspector loopback"; in practice a
  9-item curl loopback was used instead (initialize handshake + tools/list manifest
  + structuredContent round-trip + notification 202 / DELETE 204 / expired session
  404 / auth 401 / no secret configured 403 / GET 405); this machine has no
  inspector runtime environment.
- **Product defect fixed along the way**: the initial version of `new_session_id()`
  looped 4 times and produced 64 hex (256 bit), contradicting its own comment of
  "32 hex (128 bit)"; after changing to ×2 it produces 32 hex, with tests pinned to
  this contract.

### Phase 2 Reconciliation (2026-10-08, Increment 3 SSE Notification Stream)

- **Stream shape**: after `GET /mcp` passes auth and session admission (same
  semantics as POST/DELETE: no secret configured → 403 / Bearer failure → 401 +
  `WWW-Authenticate` / session missing or expired → 404; `McpServer::check_auth`
  was extracted as the shared entry point), it returns 200 `text/event-stream` +
  `Cache-Control: no-cache` with no Content-Length (an unbounded stream); it writes
  the initial comment line `: falcon mcp stream` so the client can confirm the
  stream is open before the first notification. The success path does not close the
  connection; disconnection is sensed via the client's recv (peer EOF / server
  stop() shutdown).
- **Notification fan-out**: `broadcast_notification` dresses the same JSON-RPC
  notification envelope in two framings (WS TEXT frame / SSE
  `data: <envelope>\n\n` line); engine events reach the SSE stream directly via the
  existing RpcEventBridge (real daemon e2e: addUri → onDownloadStart/onDownloadError
  arriving). Admission happens only at stream-open time; an established stream is
  not re-validated when the session expires (different from WS's shape of an
  unauthenticated handshake with per-request in-session auth; SSE is a one-way
  write-only stream with no request surface).
- **Testability boundary**: the early-return branch for a failed SSE streaming
  response-header send cannot be anchored (a microsecond race window between accept
  and writing the headers, with no synchronization point; same qualification as the
  WS side); all other new code (admission / stream opening / registration /
  fan-out / shutdown wakeup / client-disconnect deregistration / CORS echo) is fully
  covered by tests.
- **Smoke-test shape**: curl loopback + real daemon e2e (`curl -N` hitting the
  timeout with 124 proves the stream does not close on its own; engine events cross
  the bridge and arrive in data lines) in place of the MCP inspector (this machine
  lacks that environment; same posture as Phase 1).
