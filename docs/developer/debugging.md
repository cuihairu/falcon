# Debugging Techniques

This document lists practical debugging entry points in the current repository, focused on real targets and interfaces.

## Debug build

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON

cmake --build build
```

## Sanitizers

The current top-level `CMakeLists.txt` exposes:

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_ENABLE_SANITIZERS=ON

cmake --build build
```

This documentation no longer references the nonexistent `FALCON_ENABLE_ASAN` or `FALCON_ENABLE_TSAN` option names.

## GDB

```bash
gdb --args ./build/bin/falcon-cli https://example.com/file.zip
```

Useful breakpoints should target the real interfaces:

```txt
(gdb) break main
(gdb) break falcon::DownloadEngine::add_task
(gdb) break falcon::DownloadEngine::start_task
(gdb) break falcon::DownloadTask::set_status
```

Conditional breakpoint example:

```txt
(gdb) break falcon::DownloadEngine::add_task if url.find("example") != std::string::npos
```

## LLDB

```bash
lldb ./build/bin/falcon-cli -- https://example.com/file.zip
```

```txt
(lldb) b main
(lldb) b falcon::DownloadEngine::add_task
(lldb) b falcon::DownloadEngine::start_task
(lldb) run
```

## Logging

In the current implementation, log levels ultimately flow through `falcon::set_log_level(...)`. More effective debugging usually means:

- Running the CLI from a Debug build.
- Combining that with `--verbose`.
- Setting breakpoints at key implementation points.

```bash
./build/bin/falcon-cli --verbose https://example.com/file.zip
```

## Performance analysis

### perf

```bash
perf record -g ./build/bin/falcon-cli https://example.com/file.zip
perf report
```

### Benchmark

Benchmarks should also use the current real interfaces:

```cpp
static void BM_AddAndStartTask(benchmark::State& state) {
    for (auto _ : state) {
        falcon::DownloadEngine engine;
        auto task = engine.add_task("http://localhost:8080/file.zip");
        if (task) {
            engine.start_task(task->id());
            task->wait();
        }
    }
}
```

## Common investigation points

### Unsupported URL

Check:

```cpp
engine.is_url_supported(url)
engine.get_supported_protocols()
```

### Task does not start

Confirm that:

- `add_task()` returned a task successfully.
- `start_task(task->id())` was called explicitly.
- The matching plugin is enabled in the build.

### Abnormal task status

Observe:

- `DownloadTask::status()`
- `DownloadTask::error_message()`
- `IEventListener::on_status_changed()`

## Core debugging entry points

To understand the main flow, start with these implementations:

- `packages/libfalcon-core/src/download_engine.cpp`
- `packages/libfalcon-core/src/task_manager.cpp`
- `packages/libfalcon-core/src/protocol_registry.cpp`
- `packages/libfalcon-core/src/event_dispatcher.cpp`
