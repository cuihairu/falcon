# Architecture Design

This document describes Falcon's core structure based on the current repository code, not the idealized interfaces from early design drafts.

## Overall Structure

```text
falcon/
├── libs/
│   ├── core/       # Core download engine, task model, event system
│   ├── protocols/  # Standard download protocols and protocol-related implementations
│   ├── storage/    # Object storage and remote resource browsing
│   └── drives/     # SPI facade only: loader + pure link/search parsing, no third-party deps
├── plugins/
│   └── drives/     # SPI plugins: cloud_storage / resource_search / config_storage (MODULE .so/.dll)
├── apps/
│   ├── cli/        # Command-line tool
│   ├── daemon/     # Background service
│   ├── swarmd/     # P2SP rendezvous service
│   └── desktop/    # Qt6 desktop application
└── docs/                   # Documentation site
```

## Library Dependencies

```
falcon_protocols  →  falcon_core
falcon_storage    →  falcon_core
falcon_drives     →  falcon_core
```

Reverse dependencies are forbidden: `core` does not depend on `protocols/storage/drives`.

The top level also provides a compatibility aggregate target:

```text
Falcon::falcon = falcon_core + falcon_protocols + falcon_storage + falcon_drives
```

`Falcon::falcon` mainly serves historical compatibility and "link everything in one go" scenarios. Internal applications, tests, and new modules should prefer linking the specific targets they need — for example `Falcon::core`, `Falcon::protocols`, `Falcon::storage`, or `Falcon::drives` — to avoid implicitly pulling unrelated modules into the final link graph.

## Current Layering Status

### libfalcon-core

The core layer contains the stable domain model and scheduling abstractions:

- `DownloadTask`, `TaskManager`, `DownloadEngine`
- `ProtocolRegistry`, `IProtocolHandler`
- `EventDispatcher`, `IEventListener`
- Basic types, exceptions, logging, versioning, password management

`falcon_core` can be built and tested independently, with no dependency on concrete protocol implementations. Built-in protocol registration goes through weak-symbol stub functions provided by `builtin_protocol_handlers_stub.cpp`; when the final program also links `falcon_protocols`, the real implementations in `libfalcon-protocols/src/builtin_protocol_handlers.cpp` override the stubs and register concrete handlers such as HTTP and FTP.

### libfalcon-protocols

The protocol layer depends on `falcon_core` and provides the concrete download protocols and transport capabilities:

- Protocol handlers such as HTTP and FTP
- Segmented downloads, incremental downloads, file hashing
- Protocol-side networking infrastructure such as sockets and event polling
- Optional private protocol plugins

The protocol layer can be extended with additional protocols, but protocol implementation details must not leak back into core.

### libfalcon-storage

The storage layer depends on `falcon_core` and provides remote resource browsing and object-storage browsing capabilities — for example the FTP/S3/OSS/COS/Kodo/Upyun browsers. It must not depend on `falcon_protocols`.

### libfalcon-drives

`falcon_drives` is a **facade only**: it keeps the public C++ API (`CloudStorageManager`,
`ResourceSearchManager`, `ConfigManager`), the plugin loader, and pure in-process parsing
(`CloudLinkDetector`, `search::detail::*`). It links only `falcon_core` and has **no
third-party dependency**. Concrete implementations live in `plugins/drives/` as independent
`MODULE` libraries (`.so` / `.dylib` / `.dll`) and are discovered at runtime through the
**stable C ABI** defined in `libs/core/include/falcon/abi/drive_api.h`. No STL type, C++
class, or C++ virtual table crosses the shared-library boundary. See §"Plugin ABI Boundary"
below.

## Runtime Layers

The build packages above are composition units. The responsibilities below are the seven runtime layers derived from actual ownership, transport, configuration, and lifecycle evidence. Term-level evidence is collected in the [source-layer glossary](./glossary.md).

### L1: Engine Domain and Scheduling

`DownloadEngine` is the stable facade for adding, starting, pausing, resuming, canceling, and observing tasks; its implementation wires `TaskManager`, `ProtocolRegistry`, and `EventDispatcher` (`libs/core/src/download_engine.cpp:372`, `libs/core/include/falcon/download_engine.hpp:16`). `TaskManager` owns queues, priority, worker execution, and finished-task cleanup (`libs/core/include/falcon/task_manager.hpp:22`). Events leave this layer through a bounded worker pool (`libs/core/src/event_dispatcher.cpp:17`). Engine state is process memory; persistence belongs to daemon assembly.

### L2: Protocol and Transfer Data Plane

The core registry receives compile-selected concrete handlers at startup (`libs/protocols/src/builtin_protocol_handlers.cpp:39`). Protocol packages implement the V1 handler contract plus the libcurl-based transfer infrastructure; HTTP/FTP downloads always use this single V1 data plane, with no engine-version switch. Feature selection is CMake schema input, not a runtime config file (`CMakeLists.txt:48`, `libs/protocols/CMakeLists.txt:194`).

### L3: Remote Resource and Cloud Access

`falcon_storage` exposes browsable remote filesystem/object stores through `IResourceBrowser` and `ResourceBrowserManager` (`libs/storage/include/falcon/storage/resource_browser.hpp:81`, `libs/storage/src/resource_browser.cpp:90`). `falcon_drives` is a different extension point: share-link detection and direct-link extraction (`libs/drives/include/falcon/drives/cloud_storage_plugin.hpp:89`) plus selector-based search (`libs/drives/src/search_detail.cpp`). Its optional encrypted configuration vault is implemented in the `config_storage` plugin (`plugins/drives/config_storage/src/config_manager.cpp`). Remote browsing and drive resolution are deliberately separate interfaces because browse operations authenticate to a filesystem endpoint, while share resolution derives a URL from vendor page state.

### L4: Presentation and Local Application State

The Qt process owns widgets, pages, translation, system integration, and backend choice. The command boundary is `IDownloadBackend`; two implementations either call `DownloadEngine` directly or send JSON-RPC to the daemon (`apps/desktop/src/services/download_backend.hpp:33`, `apps/desktop/src/services/download_backend.cpp:79`, `apps/desktop/src/services/download_backend.cpp:191`). MainWindow selects the backend from QSettings (`apps/desktop/src/main_window.cpp:281`). QSettings also stores daemon connection data, cloud profiles, view preferences, and update state (`apps/desktop/src/main_window.cpp:743`, `apps/desktop/src/services/storage_service.cpp:76`, `apps/desktop/src/pages/download_page.cpp:847`). Embedded mode additionally owns `.falcon-trash/trash.json` (`apps/desktop/src/services/trash_store.cpp:117`), and search-engine rules live in `~/.config/falcon/engines.json` (`apps/desktop/src/services/search_engine_catalog.cpp:192`, `apps/desktop/src/services/search_engine_catalog.cpp:210`).

### L5: Service Assembly and Process Lifecycle

CLI assembly is a single foreground run: parse/merge options, construct the V1 engine, attach terminal listeners and optional P2SP announcers, wait, then exit (`apps/cli/src/main.cpp:864`, `apps/cli/src/main.cpp:997`, `apps/cli/src/main.cpp:1055`). Daemon assembly is long-lived: it constructs the V1 engine before restoring tasks, initializes WAL SQLite, restores non-terminal IDs, wires persistence and P2SP, starts RPC/MCP, and performs signal/config-reload handling (`apps/daemon/src/main.cpp:300`, `apps/daemon/src/main.cpp:312`, `apps/daemon/src/main.cpp:340`, `apps/daemon/src/main.cpp:517`, `apps/daemon/src/main.cpp:543`). Each process owns its own in-memory engine; only the daemon owns durable task sessions.

### L6: Control-Plane Protocols and Persistence

The daemon listener accepts raw TCP, parses HTTP, and serves `/jsonrpc` plus WebSocket notifications (`apps/daemon/src/rpc/json_rpc_server.cpp:575`, `apps/daemon/src/rpc/json_rpc_server.cpp:1086`). MCP `tools/list` and `tools/call` reuse `JsonRpcServer::dispatch_rpc` (`apps/daemon/src/rpc/mcp_server.cpp:473`, `apps/daemon/src/rpc/json_rpc_server.cpp:1176`). SQLite is the daemon's authority for task state; a listener converts engine events to throttled writes and final flushes (`apps/daemon/src/storage/task_storage.cpp:25`, `apps/daemon/src/storage/task_storage_listener.cpp:18`). No Redis or MySQL is used. This control plane is intentionally independent of L3, which talks to third-party resource/cloud APIs.

### L7: P2SP Rendezvous and Data Plane

`falcon-swarmd` is a separate authority, not a daemon plug-in. Its rendezvous state is in-memory node/session/challenge/resource maps (`apps/swarmd/src/rdv/swarm_rdv_state.hpp:3`), served over a self-contained HTTP/WebSocket server (`apps/swarmd/src/rdv/swarm_rpc_server.cpp:295`). The protocol file fixes signed methods, notification names, and error codes (`apps/swarmd/src/common/swarm_protocol.hpp:30`). Clients use a persistent PEM identity (`apps/swarmd/src/client/swarm_key_store.cpp:1`). The daemon's announcer publishes completions and its local HTTP service answers range requests for advertised bytes (`apps/daemon/src/daemon/swarm_announcer.cpp:1`, `apps/daemon/src/daemon/swarm_data_service.cpp:193`). Thus catalog authority and file availability have independent lifecycles.

### Configuration and State Matrix

| Layer | Config input | State store |
|---|---|---|
| L1–L2 | CMake options and caller-provided `DownloadOptions` | Process memory; transfer resume artifacts in destination files |
| L3 | Provider options supplied by the caller or desktop profile | Optional encrypted SQLite for drive profiles; remote endpoints are stateless servers |
| L4 | QSettings, `engines.json`, CLI deep-link | QSettings, `trash.json`, downloaded files |
| L5–L6 | CLI flags merged with `daemon.json`; SIGHUP reloads the file | WAL SQLite `tasks.db`, PID file, logs, daemon-owned in-memory engine |
| L7 | `swarm.json`, CLI P2SP flags, PEM key | Rendezvous maps are memory-only; key/identity is a PEM file; daemon serves advertised files |

The concrete schema readers are `apps/daemon/src/daemon/config.cpp:96`, `apps/swarmd/src/swarmd_config.cpp:94`, and `apps/cli/src/config_loader.cpp:176`. Build-time schema lives beside CMake options (`CMakeLists.txt:37`, `libs/drives/CMakeLists.txt:121`).

## Split Rationale

1. **Keep reusable domain abstractions separate from wire implementations.** Core exposes handler routing but no concrete protocol (`libs/core/src/protocol_registry.cpp:97`); protocol code supplies handlers only after the concrete package is linked (`libs/protocols/src/builtin_protocol_handlers.cpp:39`). This prevents every future protocol from changing task scheduling APIs.
2. **Do not merge share resolution with filesystem browsing.** Storage browsing requires a connected endpoint and path operations (`libs/storage/include/falcon/storage/resource_browser.hpp:117`), while drive resolution first turns a share URL into a direct resource (`libs/drives/src/cloud_storage_plugin.cpp:320`, `libs/drives/src/cloud_storage_plugin.cpp:466`). Their failure, authentication, and state models differ.
3. **Keep transport concerns inside the protocol layer, not the engine facade.** The single V1 engine delegates all HTTP/FTP byte transfer to libcurl-based protocol handlers, while scheduling and persistence stay in core and daemon assembly; adding a protocol never changes the engine facade or introduces a second engine/persistence model.
4. **Make daemon persistence event-driven rather than RPC-serialized.** SQLite writes occur from the engine listener with progress throttling and terminal flushes (`apps/daemon/src/storage/task_storage_listener.cpp:18`, `apps/daemon/src/storage/task_storage_listener.cpp:74`). RPC then reads durable truth instead of becoming the only writer.
5. **Separate long-lived control plane from disposable clients.** Daemon restores IDs and active tasks at startup (`apps/daemon/src/main.cpp:340`), while CLI constructs tasks for one run and waits (`apps/cli/src/main.cpp:997`, `apps/cli/src/main.cpp:1050`). One facade serves both without forcing CLI to own a database.
6. **Let UI command semantics survive a backend swap.** `IDownloadBackend` fixes the same add/control/query contract (`apps/desktop/src/services/download_backend.hpp:33`); MainWindow chooses embedded or RPC only at construction (`apps/desktop/src/main_window.cpp:281`). RPC can therefore be added without rewriting every Qt page.
7. **Make P2SP catalog authority crash-isolated from download processes.** The rendezvous service owns rate limits and TTL sweeps (`apps/swarmd/src/rdv/swarm_rdv_state.hpp:3`), while each node owns its key and local data service (`apps/swarmd/src/client/swarm_key_store.cpp:1`, `apps/daemon/src/daemon/swarm_data_service.cpp:193`). Catalog outage does not destroy local downloads, and node exit removes availability without corrupting a catalog database.
8. **Keep control-plane protocols behind one method dispatcher.** MCP delegates `tools/call` to `dispatch_rpc` (`apps/daemon/src/rpc/mcp_server.cpp:476`), so aria2, Falcon extensions, and MCP cannot drift into separate business logic.
9. **Keep third-party integrations behind a stable C ABI.** `libs/drives` is a dependency-free facade; curl / SQLite3 / OpenSSL / nlohmann-json live only in `plugins/drives/*` MODULE libraries. A vendor can change its page layout or SDK without forcing a rebuild of the core facade, and private/licensed plugins can ship as binaries next to the app. The only contract crossing the boundary is the C ABI in `falcon/abi/drive_api.h` — POD structs, function pointers, opaque handles, a host allocator, and extension queries. STL and C++ ABI never cross it, so third-party plugins built with a different toolchain or CRT remain loadable.

## Plugin ABI Boundary

The plugin boundary is the stable C header `libs/core/include/falcon/abi/drive_api.h`
(C11-compatible; no C++, no STL, no third-party headers). Third-party cloud-drive plugins
only need to include this header and export two C symbols; they do not link `falcon_drives`.

### Discovery and loading

The facade discovers plugins at startup (or on `PluginLoader::reload()`) from:
1. `FALCON_PLUGIN_DIR` (cross-platform path list),
2. `<exe dir>/falcon/plugins` and `<exe dir>/plugins`,
3. the compile-time install directory (`FALCON_PLUGIN_DEFAULT_DIR`).

Each MODULE exports:

```c
const falcon_plugin_manifest* falcon_plugin_entry(uint32_t abi_version);
void falcon_plugin_set_host(const falcon_host_api* host);
```

`falcon_plugin_entry` returns a static manifest with one or more service descriptors.
The loader checks `FALCON_PLUGIN_ABI_COMPATIBLE()` (major version only), requires a stable
name and a complete service table, and calls `falcon_plugin_set_host` **before** any
service table is used. Failures are collected in `PluginLoader::errors()` instead of
terminating the process; the facade then degrades to "custom plugins only" (cloud),
"no engines" (search), or "not initialized" (config).

### Stability rules

- Cross-boundary data uses only fixed-width integers, pointers, and `falcon_str` /
  `falcon_kv` / `{ptr,count}` POD views. No `std::string`, `vector`, `map`, or C++ classes.
- Memory allocated by a plugin and handed to the host uses `host->allocate` and is
  released through `host->deallocate` or the service's `free_*` callback. No `malloc`/`free`
  or `new`/`delete` mismatch can occur across CRTs.
- Each ABI struct starts with `abi_version`; minor additions append at the end. New
  capabilities are queried through `extension(id)` rather than by growing a vtable.
- Breaking changes raise `FALCON_PLUGIN_ABI_MAJOR`; incompatible plugins are rejected.

### Author helpers and the role of macros

`falcon/abi/marshal.hpp` and `falcon/abi/plugin_helpers.hpp` are **inside-the-boundary**,
header-only helpers; they are not part of the ABI. They provide `view/copy`,
`StringArena::allocate<T>()`, the `HostApi` injection slot, `ArenaRegistry` (one arena per
output address), `fill_kv_array()`, `invoke_or/invoke_void()` exception shielding, and
`FALCON_PLUGIN_DEFINE_ENTRY*()` for generating the two entry symbols.

Macros are limited to that export boilerplate. ABI structs, vtable fields, memory
ownership, and lifetimes remain explicit in the C header; no macro hides them. A third
party can therefore implement a plugin from the C header alone, or use the helper box to
avoid repetitive boilerplate, without smuggling C++ types back across the boundary.
On Windows, export the symbols with `WINDOWS_EXPORT_ALL_SYMBOLS` or a `.def` file.

### Services and host capabilities

Three services are defined: `FALCON_SERVICE_CLOUD_STORAGE`, `FALCON_SERVICE_RESOURCE_SEARCH`,
and `FALCON_SERVICE_CONFIG_STORAGE`. The built-in plugins in `plugins/drives/` implement
these same ABI tables by adapting their internal C++ implementations.

`falcon_host_api` is the **reverse direction**: the host passes its capabilities into the
plugin. It exposes `allocate`/`deallocate`, `http_get`/`http_post`, `log`, and `extension`.
The HTTP callbacks are implemented by `falcon_protocols` using its existing curl dependency
(`libs/protocols/src/host_api.cpp`) and injected by the desktop application before any
plugin loads. This is the mature host-service pattern used by CLAP/VST3: plugins stay free
of third-party network libraries while still calling the kernel protocol stack.

Because `falcon::detail` injection state is `inline` static, each DLL has its own copy.
Tests that drive plugin-internal failure paths call `set_injection` on the C ABI config
service rather than `falcon::detail::ScopedInjection` in the test executable.

## Current Stability Baseline

The following have been verified with vcpkg manifest builds:

- `falcon_core_tests`: 384 tests passing
- `falcon_split_tests`: 10 tests passing

This shows that the core behavior of `libfalcon-core` and the four-library link layering are stable within current test coverage. It does not mean the whole project is feature-complete; protocol plugins, the desktop app, the daemon, cloud drives, and resource browsing still need their own integration tests and real-world validation.

## Core Components of libfalcon-core

### DownloadEngine

`DownloadEngine` is the main entry point for callers, responsible for:

- Accepting download requests
- Managing task lifecycles
- Managing event listeners
- Cooperating with `ProtocolRegistry` and `TaskManager`

Its current public interface consists mainly of these methods:

```cpp
DownloadTask::Ptr add_task(const std::string& url,
                           const DownloadOptions& options = {});
std::vector<DownloadTask::Ptr> add_tasks(const std::vector<std::string>& urls,
                                         const DownloadOptions& options = {});

bool start_task(TaskId id);
bool pause_task(TaskId id);
bool resume_task(TaskId id);
bool cancel_task(TaskId id);

void add_listener(IEventListener* listener);
void remove_listener(IEventListener* listener);

std::vector<std::string> get_supported_protocols() const;
bool is_url_supported(const std::string& url) const;
```

Notes:

- Constructing a `DownloadEngine` loads the built-in handlers available in the current build.
- The documentation no longer presents the old `startDownload()` / `loadPlugin()` as external interfaces.

### TaskManager

`TaskManager` is responsible for:

- The task queue
- Maximum concurrency control
- Task state transitions
- Starting, pausing, resuming, and canceling individual tasks

From the `DownloadEngine` implementation, tasks are typically first created via `add_task()` and then driven forward by `start_task()`.

### ProtocolRegistry

`ProtocolRegistry` is responsible for:

- Registering protocol handlers
- Routing a URL to the appropriate handler
- Loading the built-in handlers available in the current build
- Providing protocol and scheme queries

Core methods from the public header include:

```cpp
void register_handler(std::unique_ptr<IProtocolHandler> handler);
IProtocolHandler* get_handler(const std::string& protocol) const;
IProtocolHandler* get_handler_for_url(const std::string& url) const;
void load_builtin_handlers();
bool supports_url(const std::string& url) const;
```

### IProtocolHandler

Protocol plugins plug in through `IProtocolHandler`:

```cpp
std::string protocol_name() const;
std::vector<std::string> supported_schemes() const;
bool can_handle(const std::string& url) const;
FileInfo get_file_info(const std::string& url,
                       const DownloadOptions& options);
void download(DownloadTask::Ptr task, IEventListener* listener);
void pause(DownloadTask::Ptr task);
void resume(DownloadTask::Ptr task, IEventListener* listener);
void cancel(DownloadTask::Ptr task);
```

Compared with the old design, the download interface is not "pass a URL, get a new task" — it "receives an existing `DownloadTask::Ptr`".

## Download Flow

```text
Caller invokes add_task(url, options)
        ↓
DownloadEngine validates the URL and creates a DownloadTask
        ↓
ProtocolRegistry finds the protocol handler for the URL
        ↓
TaskManager takes over the task and queues it
        ↓
start_task(id) triggers execution
        ↓
The protocol handler executes download(task, listener)
        ↓
Task status and progress are reported back through the event system
```

## Event System

The current public event interfaces are not the `ProgressEvent` / `CompleteEvent` structs, but rather:

- `TaskStatus`
- `ProgressInfo`
- `FileInfo`
- `IEventListener`

Main `IEventListener` callbacks:

```cpp
void on_status_changed(TaskId task_id,
                       TaskStatus old_status,
                       TaskStatus new_status);
void on_progress(const ProgressInfo& info);
void on_error(TaskId task_id, const std::string& error_message);
void on_completed(TaskId task_id, const std::string& output_path);
void on_file_info(TaskId task_id, const FileInfo& info);
```

## Concurrency and Threading

From the current implementation:

- `EventDispatcher` handles event dispatch
- `TaskManager` handles task scheduling
- Protocol handlers implement the concrete download logic
- `DownloadEngine::Impl` starts the event dispatcher and task manager at construction

This means callers normally do not need to manage a "main event loop" themselves.

## Design Boundaries

This document deliberately does not expand on designs that have not yet formed a stable public API:

- Async wrapper interfaces like `startDownloadAsync()`
- Coroutine-based download APIs
- Protocol-specific public option structs
- A web management interface as a de facto architecture layer

Once any of these land in actual headers, they will be appropriate to document in the architecture pages.
