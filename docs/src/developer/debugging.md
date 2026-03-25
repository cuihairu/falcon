# 调试技巧

本文档给出当前仓库下更实用的调试入口，重点围绕真实存在的目标和接口。

## Debug 构建

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON

cmake --build build
```

## Sanitizer

当前顶层 `CMakeLists.txt` 暴露的是：

```bash
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_ENABLE_SANITIZERS=ON

cmake --build build
```

文档里不再使用不存在的 `FALCON_ENABLE_ASAN` / `FALCON_ENABLE_TSAN` 选项名。

## GDB

```bash
gdb --args ./build/bin/falcon-cli https://example.com/file.zip
```

常用断点更适合打在当前真实接口上：

```gdb
(gdb) break main
(gdb) break falcon::DownloadEngine::add_task
(gdb) break falcon::DownloadEngine::start_task
(gdb) break falcon::DownloadTask::set_status
```

条件断点示例：

```gdb
(gdb) break falcon::DownloadEngine::add_task if url.find("example") != std::string::npos
```

## LLDB

```bash
lldb ./build/bin/falcon-cli -- https://example.com/file.zip
```

```lldb
(lldb) b main
(lldb) b falcon::DownloadEngine::add_task
(lldb) b falcon::DownloadEngine::start_task
(lldb) run
```

## 日志

从当前实现看，日志级别最终会走 `falcon::set_log_level(...)`。更有效的调试方式通常是：

- 在 Debug 构建下运行 CLI
- 结合 `--verbose`
- 在关键实现点下断点

```bash
./build/bin/falcon-cli --verbose https://example.com/file.zip
```

## 性能分析

### perf

```bash
perf record -g ./build/bin/falcon-cli https://example.com/file.zip
perf report
```

### Benchmark

如果要写性能样例，建议也基于当前真实接口：

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

## 常见排查点

### URL 不支持

先看：

```cpp
engine.is_url_supported(url)
engine.get_supported_protocols()
```

### 任务未启动

确认是否：

- `add_task()` 成功返回任务
- 显式调用了 `start_task(task->id())`
- 构建中启用了对应插件

### 任务状态异常

优先观察：

- `DownloadTask::status()`
- `DownloadTask::error_message()`
- `IEventListener::on_status_changed()`

## 核心调试入口

如果要理解主流程，优先看这些实现：

- `packages/libfalcon/src/download_engine.cpp`
- `packages/libfalcon/src/task_manager.cpp`
- `packages/libfalcon/src/plugin_manager.cpp`
- `packages/libfalcon/src/event_dispatcher.cpp`
