[中文](architecture.zh.md) | **English**

# Architecture Design

This document describes Falcon's core structure based on the current repository code, not the idealized interfaces from early design drafts.

## Overall Structure

```text
falcon/
├── packages/
│   ├── libfalcon-core/     # Core download engine, task model, event system
│   ├── libfalcon-protocols/# Standard download protocols and protocol-related implementations
│   ├── libfalcon-storage/  # Object storage and remote resource browsing
│   ├── libfalcon-drives/   # Cloud drives, share links, account-state capabilities
│   ├── falcon-cli/         # Command-line tool
│   └── falcon-daemon/      # Background service
├── apps/
│   └── desktop/            # Qt6 desktop application
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
- Segmented downloads, incremental downloads, request groups, file hashing
- Protocol-side networking infrastructure such as sockets and event polling
- Optional private protocol plugins

The protocol layer can be extended with additional protocols, but protocol implementation details must not leak back into core.

### libfalcon-storage

The storage layer depends on `falcon_core` and provides remote resource browsing and object-storage browsing capabilities — for example the FTP/S3/OSS/COS/Kodo/Upyun browsers. It must not depend on `falcon_protocols`.

### libfalcon-drives

The cloud-drive layer depends on `falcon_core` and provides cloud drive, share-link, resource-search, and configuration-management capabilities. This layer still contains placeholder source files and several optional feature switches, and is less mature than `core` and the core protocol layer.

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
