# 调试技巧

本文档介绍 Falcon 项目的调试方法和技巧。

## 编译配置

### Debug 模式

```bash
# Debug 模式编译（包含调试信息）
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_BUILD_TESTS=ON

cmake --build build
```

### RelWithDebInfo 模式

```bash
# 带调试信息的优化版本
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo

cmake --build build
```

### AddressSanitizer

```bash
# 启用地址消毒器
cmake -B build -S . \
  -DCMAKE_BUILD_TYPE=Debug \
  -DFALCON_ENABLE_ASAN=ON

cmake --build build
./build/bin/falcon-cli <URL>
```

## GDB 调试

### 基本使用

```bash
# 启动 GDB
gdb --args ./build/bin/falcon-cli https://example.com/file.zip

# 或先启动后加载文件
gdb ./build/bin/falcon-cli
(gdb) set args https://example.com/file.zip
```

### 常用命令

```bash
# 设置断点
(gdb) break main
(gdb) break falcon::DownloadEngine::startDownload

# 运行
(gdb) run

# 单步执行
(gdb) next       # 下一行（跳过函数）
(gdb) step       # 下一步（进入函数）
(gdb) finish     # 完成当前函数
(gdb) continue   # 继续执行

# 查看变量
(gdb) print variable_name
(gdb) print *this
(gdb) print array[0]@10  # 查看数组元素

# 查看调用栈
(gdb) backtrace
(gdb) frame 0

# 查看线程
(gdb) info threads
(gdb) thread 2
```

### 条件断点

```bash
# 条件断点
(gdb) break falcon::DownloadEngine::startDownload if url.find("example") != std::string::npos

# 断点命令列表
(gdb) commands
Type commands for breakpoint(s) 1, one per line.
End with a line saying just "end".
>print url
>print options
>continue
>end
```

### 观察点

```bash
# 当变量值改变时停止
(gdb) watch task_count_

# 当变量被读取时停止
(gdb) rwatch task_count_

# 当变量被读写时停止
(gdb) awatch task_count_
```

## LLDB 调试 (macOS)

### 基本使用

```bash
# 启动 LLDB
lldb ./build/bin/falcon-cli -- https://example.com/file.zip
```

### 常用命令

```bash
# 设置断点
(lldb) breakpoint set --name main
(lldb) b main

# 运行
(lldb) run

# 单步执行
(lldb) next       # 下一行
(lldb) step       # 进入函数
(lldb) continue   # 继续

# 查看变量
(lldb) expr variable_name
(lldb) p variable_name

# 查看调用栈
(lldb) bt
```

## 日志调试

### 日志级别

```cpp
#include <falcon/logging.hpp>

// 使用日志宏
FALCON_LOG_TRACE("详细追踪信息: " << value);
FALCON_LOG_DEBUG("调试信息: " << value);
FALCON_LOG_INFO("普通信息");
FALCON_LOG_WARN("警告信息");
FALCON_LOG_ERROR("错误信息");
FALCON_LOG_CRITICAL("严重错误");
```

### 设置日志级别

```bash
# 环境变量
export FALCON_LOG_LEVEL=debug
./build/bin/falcon-cli <URL>

# 代码中设置
falcon::Logger::setLevel(falcon::LogLevel::Debug);
```

### 日志输出

```cpp
// 格式化输出
FALCON_LOG_INFO("下载进度: " << downloaded << " / " << total
                << " (" << (downloaded * 100.0 / total) << "%)");

// 带上下文的日志
FALCON_LOG_DEBUG("[" << task_id << "] 连接到 " << host << ":" << port);
```

## 性能分析

### perf (Linux)

```bash
# 记录性能数据
perf record -g --call-graph dwarf ./build/bin/falcon-cli <URL>

# 查看报告
perf report

# 生成火焰图
perf script | ./FlameGraph/flamegraph.pl > flamegraph.svg
```

### Instruments (macOS)

```bash
# 启动 Instruments
instruments -t "Time Profiler" ./build/bin/falcon-cli <URL>

# 或使用 XCode
xcrun xctrace record --template "Time Profiler" --launch ./build/bin/falcon-cli -- <URL>
```

### Google Benchmark

```cpp
#include <benchmark/benchmark.h>

static void BM_HttpDownload(benchmark::State& state) {
    falcon::DownloadEngine engine;
    for (auto _ : state) {
        auto task = engine.startDownload("http://localhost:8080/file.zip");
        task->wait();
    }
}

BENCHMARK(BM_HttpDownload);

BENCHMARK_MAIN();
```

## 内存检查

### Valgrind

```bash
# 内存泄漏检查
valgrind --leak-check=full \
         --show-leak-kinds=all \
         --track-origins=yes \
         ./build/bin/falcon-cli <URL>

# 详细报告
valgrind --leak-check=full --verbose \
         ./build/bin/falcon-cli <URL>
```

### AddressSanitizer

```bash
# 编译时启用
cmake -B build -S . -DFALCON_ENABLE_ASAN=ON
cmake --build build

# 运行
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=0
./build/bin/falcon-cli <URL>
```

## 常见问题调试

### 段错误 (Segmentation Fault)

```bash
# 使用 GDB 获取崩溃信息
gdb -ex "run" -ex "bt" --args ./build/bin/falcon-cli <URL>

# 或使用 core dump
ulimit -c unlimited
./build/bin/falcon-cli <URL>  # 崩溃后生成 core 文件
gdb ./build/bin/falcon-cli core
```

### 死锁

```bash
# 使用 GDB 检查线程状态
gdb ./build/bin/falcon-cli
(gdb) run
# Ctrl+C 挂起
(gdb) info threads
(gdb) thread apply all bt

# 使用 ThreadSanitizer
cmake -B build -S . -DFALCON_ENABLE_TSAN=ON
cmake --build build
./build/bin/falcon-cli <URL>
```

### 内存泄漏

```bash
# 使用 Valgrind
valgrind --leak-check=full ./build/bin/falcon-cli <URL>

# 或使用 AddressSanitizer
export ASAN_OPTIONS=detect_leaks=1
./build/bin/falcon-cli <URL>
```

### 性能问题

```bash
# 使用 perf
perf record -g ./build/bin/falcon-cli <URL>
perf report

# 使用采样分析
perf top -p $(pidof falcon-cli)
```

## IDE 调试

### VS Code

创建 `.vscode/launch.json`：

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "name": "(gdb) Launch",
      "type": "cppdbg",
      "request": "launch",
      "program": "${workspaceFolder}/build/bin/falcon-cli",
      "args": ["https://example.com/file.zip"],
      "stopAtEntry": false,
      "cwd": "${workspaceFolder}",
      "environment": [],
      "externalConsole": false,
      "MIMode": "gdb",
      "miDebuggerPath": "/usr/bin/gdb"
    }
  ]
}
```

### CLion

1. Run → Edit Configurations
2. 添加新的 CMake Application
3. 设置 Target: `falcon-cli`
4. 设置 Program arguments: URL
5. 设置 Working directory: 项目根目录

## 远程调试

### GDB Server

```bash
# 在远程机器上
gdbserver :1234 ./build/bin/falcon-cli <URL>

# 在本地机器上
gdb ./build/bin/falcon-cli
(gdb) target remote remote-ip:1234
```

::: tip 调试建议
- 优先使用日志调试简单问题
- 使用断点调试复杂逻辑
- 使用性能分析工具定位瓶颈
- 使用内存检查工具发现泄漏
:::
