[根目录](../../CLAUDE.md) > [packages](../) > **libfalcon**

---

# libfalcon - 核心下载引擎库

## 变更记录 (Changelog)

### 2025-12-21 - 初始化模块架构
- 创建核心库目录结构
- 定义公共 API 接口
- 设计插件系统架构
- 规划异步任务管理器

---

## 模块职责

`libfalcon` 是整个 Falcon 下载器的核心引擎库，提供以下能力：

1. **任务管理**：下载任务的创建、调度、暂停、恢复、取消
2. **下载引擎**：核心下载逻辑、速度控制、并发管理、断点续传
3. **插件系统**：协议处理器的注册、加载、卸载与生命周期管理
4. **事件系统**：下载进度、状态变更、错误处理的事件回调
5. **配置管理**：全局配置与任务级配置
6. **存储抽象**：文件写入、元数据持久化的抽象层

---

## 入口与启动

### 公共 API 头文件

所有公共 API 定义在 `include/falcon/` 目录下：

| 头文件 | 职责 |
|--------|------|
| `download_engine.hpp` | 下载引擎主类 `DownloadEngine` |
| `task_manager.hpp` | 任务管理器 `TaskManager` |
| `plugin_interface.hpp` | 插件接口 `IProtocolHandler` |
| `download_task.hpp` | 任务对象 `DownloadTask` |
| `download_options.hpp` | 配置选项 `DownloadOptions` |
| `exceptions.hpp` | 异常类定义 |
| `event_listener.hpp` | 事件监听器接口 |
| `version.hpp` | 版本信息 |

### 使用示例（伪代码）

```cpp
#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>

int main() {
    // 1. 创建下载引擎
    falcon::DownloadEngine engine;

    // 2. 配置选项
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.timeout_seconds = 30;
    options.output_directory = "/tmp/downloads";

    // 3. 启动下载任务
    auto task = engine.start_download(
        "https://example.com/file.zip",
        options
    );

    // 4. 等待完成
    task->wait();

    return 0;
}
```

---

## 对外接口

### 核心类设计

#### 1. `DownloadEngine`（下载引擎主类）

```cpp
class DownloadEngine {
public:
    DownloadEngine();
    ~DownloadEngine();

    // 任务管理
    std::shared_ptr<DownloadTask> start_download(
        const std::string& url,
        const DownloadOptions& options = {}
    );

    void pause_task(TaskID id);
    void resume_task(TaskID id);
    void cancel_task(TaskID id);

    // 全局控制
    void pause_all();
    void resume_all();
    void set_global_speed_limit(size_t bytes_per_second);

    // 插件管理
    void register_plugin(std::unique_ptr<IProtocolHandler> plugin);
    std::vector<std::string> list_supported_protocols() const;

    // 事件监听
    void add_listener(std::shared_ptr<IEventListener> listener);
};
```

#### 2. `DownloadTask`（任务对象）

```cpp
class DownloadTask {
public:
    // 状态查询
    TaskStatus status() const;
    float progress() const;  // 0.0 ~ 1.0
    size_t total_bytes() const;
    size_t downloaded_bytes() const;
    size_t speed() const;  // bytes/s

    // 控制操作
    void wait();
    bool wait_for(std::chrono::seconds timeout);
    void pause();
    void resume();
    void cancel();

    // 元数据
    TaskID id() const;
    std::string url() const;
    std::string output_path() const;
    std::chrono::system_clock::time_point start_time() const;
};
```

#### 3. `IProtocolHandler`（插件接口）

```cpp
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    // 协议标识（如 "http", "ftp", "magnet"）
    virtual std::string protocol() const = 0;

    // 是否支持该 URL
    virtual bool can_handle(const std::string& url) const = 0;

    // 启动下载
    virtual std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener
    ) = 0;

    // 获取元信息（可选）
    virtual FileInfo get_file_info(const std::string& url) const;
};
```

---

## 关键依赖与配置

### 编译依赖

| 依赖库 | 用途 | 版本 | 必选/可选 |
|--------|------|------|----------|
| spdlog | 日志 | 1.9+ | 必选 |
| nlohmann/json | 配置解析 | 3.10+ | 必选 |
| libcurl | HTTP/FTP 插件 | 7.68+ | 可选（插件） |
| libtorrent-rasterbar | BitTorrent 插件 | 2.0+ | 可选（插件） |

### CMake 配置

```cmake
# 在项目根目录
cmake -B build -DFALCON_ENABLE_HTTP=ON -DFALCON_ENABLE_FTP=OFF
cmake --build build --target falcon
```

### 插件配置

插件通过 CMake 选项控制编译：

- `FALCON_ENABLE_HTTP`：HTTP/HTTPS 插件（默认 ON）
- `FALCON_ENABLE_FTP`：FTP 插件（默认 ON）
- `FALCON_ENABLE_BITTORRENT`：BitTorrent 插件（默认 OFF，需 libtorrent）

---

## 数据模型

### 核心数据结构

#### 1. `DownloadOptions`（下载配置）

```cpp
struct DownloadOptions {
    size_t max_connections = 1;       // 最大连接数（分块下载）
    size_t timeout_seconds = 30;      // 超时时间
    size_t max_retries = 3;           // 最大重试次数
    std::string output_directory;     // 输出目录
    std::string output_filename;      // 输出文件名（可选，自动从 URL 推断）
    size_t speed_limit = 0;           // 单任务速度限制（0 = 无限制）
    bool resume_if_exists = true;     // 断点续传
    std::string user_agent;           // 用户代理（HTTP）
    std::map<std::string, std::string> headers;  // 自定义 HTTP 头
};
```

#### 2. `TaskStatus`（任务状态）

```cpp
enum class TaskStatus {
    Pending,      // 等待中
    Downloading,  // 下载中
    Paused,       // 已暂停
    Completed,    // 已完成
    Failed,       // 失败
    Cancelled     // 已取消
};
```

#### 3. `FileInfo`（文件元信息）

```cpp
struct FileInfo {
    std::string url;
    size_t total_size = 0;            // 总大小（bytes，0 = 未知）
    std::string filename;             // 文件名
    std::string content_type;         // MIME 类型
    bool supports_resume = false;     // 是否支持断点续传
    std::chrono::system_clock::time_point last_modified;  // 最后修改时间
};
```

---

## 内部架构

### 核心组件

```
┌─────────────────────────────────────────┐
│          DownloadEngine                 │
│  ┌───────────────────────────────────┐  │
│  │       TaskManager                 │  │
│  │  - 任务队列（优先级）             │  │
│  │  - 并发控制（最大任务数）          │  │
│  │  - 状态机管理                     │  │
│  └───────────────┬───────────────────┘  │
│                  │                       │
│  ┌───────────────▼───────────────────┐  │
│  │       PluginManager               │  │
│  │  - 插件注册表                     │  │
│  │  - 协议路由（URL -> Handler）     │  │
│  └───────────────┬───────────────────┘  │
│                  │                       │
│  ┌───────────────▼───────────────────┐  │
│  │       EventDispatcher             │  │
│  │  - 事件队列                       │  │
│  │  - 监听器管理                     │  │
│  └───────────────────────────────────┘  │
└─────────────────────────────────────────┘
                  │
┌─────────────────▼─────────────────┐
│         Protocol Plugins          │
│  ┌──────┐  ┌──────┐  ┌──────┐    │
│  │ HTTP │  │ FTP  │  │  BT  │    │
│  └──────┘  └──────┘  └──────┘    │
└───────────────────────────────────┘
```

### 线程模型

- **主线程**：API 调用、事件分发
- **工作线程池**：执行下载任务（每个任务可能占用多个线程用于分块）
- **IO 线程**：异步 IO 操作（文件写入、网络收发）

### 异步模型（待定）

**方案 A**：`std::async` + `std::future`（简单，C++11 标准）
**方案 B**：C++20 协程（需编译器支持）
**方案 C**：第三方异步库（如 Boost.Asio、uvw）

> **决策**：第一阶段使用方案 A，后续可迁移至方案 B/C。

---

## 测试与质量

### 测试策略

#### 1. 单元测试（`tests/unit/`）

- `task_manager_test.cpp`：任务管理器逻辑测试
- `plugin_manager_test.cpp`：插件注册、查找测试
- `download_options_test.cpp`：配置项解析测试
- `event_dispatcher_test.cpp`：事件分发测试

#### 2. 集成测试（`tests/integration/`）

- `http_download_test.cpp`：HTTP 下载完整流程（需测试服务器）
- `resume_download_test.cpp`：断点续传测试
- `concurrent_download_test.cpp`：多任务并发测试
- `plugin_loading_test.cpp`：插件动态加载测试

#### 3. 性能测试（`tests/benchmark/`）

- `download_speed_benchmark.cpp`：下载速度测试
- `memory_usage_benchmark.cpp`：内存占用测试

### 测试覆盖率目标

- 核心逻辑：≥ 90%
- 插件接口：≥ 80%
- 整体覆盖率：≥ 80%

### 质量工具

- **静态分析**：clang-tidy、cppcheck
- **格式化**：clang-format（配置见根目录 `.clang-format`）
- **内存检查**：Valgrind、AddressSanitizer

---

## 常见问题 (FAQ)

### Q1：如何新增一个协议插件？

1. 在 `plugins/<protocol_name>/` 创建目录
2. 实现 `IProtocolHandler` 接口
3. 在 `CMakeLists.txt` 中添加插件编译选项
4. 在 `DownloadEngine` 初始化时注册插件

参考 `plugins/http/` 示例实现。

### Q2：如何监听下载进度？

实现 `IEventListener` 接口并注册到 `DownloadEngine`：

```cpp
class MyListener : public IEventListener {
    void on_progress(TaskID id, float progress) override {
        std::cout << "Task " << id << ": " << (progress * 100) << "%\n";
    }
};

engine.add_listener(std::make_shared<MyListener>());
```

### Q3：断点续传如何实现？

每个插件负责实现断点续传逻辑：

- HTTP：使用 `Range` 头请求部分内容
- FTP：使用 `REST` 命令
- BitTorrent：原生支持

核心库提供 `ResumeInfo` 数据结构用于持久化续传点。

### Q4：如何设置速度限制？

```cpp
// 全局限速
engine.set_global_speed_limit(1024 * 1024);  // 1 MB/s

// 单任务限速
DownloadOptions options;
options.speed_limit = 512 * 1024;  // 512 KB/s
auto task = engine.start_download(url, options);
```

---

## 相关文件清单

### 公共头文件（API）

```
include/falcon/
├── download_engine.hpp      # 下载引擎主类
├── task_manager.hpp         # 任务管理器
├── plugin_interface.hpp     # 插件接口
├── download_task.hpp        # 任务对象
├── download_options.hpp     # 配置选项
├── exceptions.hpp           # 异常定义
├── event_listener.hpp       # 事件监听器
└── version.hpp              # 版本信息
```

### 内部实现（源文件）

```
src/
├── download_engine.cpp      # 引擎实现
├── task_manager.cpp         # 任务管理器实现
├── plugin_manager.cpp       # 插件管理器实现
├── event_dispatcher.cpp     # 事件分发器实现
└── internal/                # 内部头文件
    ├── task_queue.hpp       # 任务队列
    ├── thread_pool.hpp      # 线程池
    └── file_writer.hpp      # 文件写入抽象
```

### 插件实现

```
plugins/
├── http/
│   ├── CLAUDE.md            # HTTP 插件文档
│   ├── http_plugin.hpp
│   ├── http_plugin.cpp
│   └── curl_wrapper.cpp     # libcurl 封装
├── ftp/
│   ├── CLAUDE.md
│   ├── ftp_plugin.hpp
│   └── ftp_plugin.cpp
└── bittorrent/
    ├── CLAUDE.md
    ├── bt_plugin.hpp
    └── bt_plugin.cpp
```

### 测试文件

```
tests/
├── unit/
│   ├── task_manager_test.cpp
│   ├── plugin_manager_test.cpp
│   └── event_dispatcher_test.cpp
├── integration/
│   ├── http_download_test.cpp
│   └── resume_download_test.cpp
└── benchmark/
    └── download_speed_benchmark.cpp
```

---

## 下一步开发计划

### 第一阶段（基础架构）

1. 实现 `TaskManager` 核心逻辑
2. 实现 `PluginManager` 与插件注册机制
3. 定义完整的公共 API 头文件
4. 实现 `EventDispatcher` 事件系统

### 第二阶段（HTTP 插件）

1. 基于 libcurl 实现 HTTP/HTTPS 插件
2. 支持基础下载功能
3. 实现断点续传
4. 单元测试与集成测试

### 第三阶段（高级特性）

1. 多线程分块下载（HTTP Range 请求）
2. 速度限制与并发控制
3. 任务优先级队列
4. 性能优化与基准测试

---

**文档维护**：每次接口变更、新增功能时，请更新本文档并在"变更记录"中添加条目。
