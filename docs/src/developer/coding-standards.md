# 编码规范

本文档定义了 Falcon 项目的编码规范。

## C++ 标准

- **最低要求**: C++17
- **推荐**: C++20

## 命名规范

### 文件命名

```
// 文件名使用 snake_case
download_engine.hpp
download_engine.cpp
http_plugin.cpp
```

### 类命名

```cpp
// 类名使用 PascalCase
class DownloadEngine {
};

class HttpClient {
};

class BittorrentPlugin {
};
```

### 函数命名

```cpp
// 函数使用 snake_case
void start_download();
int get_task_count();
bool is_valid_url();
```

### 变量命名

```cpp
// 局部变量和参数：snake_case
void func(int connection_count, std::string url) {
    int file_size = 0;
    bool is_connected = false;
}

// 成员变量：尾随下划线
class MyClass {
private:
    int task_count_;
    std::string config_path_;
    bool is_initialized_;
};

// 静态成员变量：k_ 前缀 + 尾随下划线
class MyClass {
private:
    static const int k_max_retries_;
    static std::string k_default_user_agent_;
};

// 常量：全大写 + 下划线
const int MAX_CONNECTIONS = 100;
const double DEFAULT_TIMEOUT = 30.0;
constexpr size_t BUFFER_SIZE = 4096;
```

### 命名空间

```cpp
// 小写，简短
namespace falcon {
namespace http {
namespace internal {

}
}
}
```

### 枚举

```cpp
// 枚举类使用 PascalCase
enum class TaskStatus {
    Pending,
    Running,
    Paused,
    Completed,
    Failed
};

// 强类型枚举值使用 PascalCase
auto status = TaskStatus::Running;
```

### 类型别名

```cpp
// 类型别名使用 PascalCase
using TaskId = uint64_t;
using DownloadCallback = std::function<void(const ProgressEvent&)>;
using PluginMap = std::unordered_map<std::string, std::unique_ptr<IPlugin>>;
```

## 代码组织

### 头文件结构

```cpp
// 1. 许可证和版权声明（可选）
// 2. 头文件保护
#pragma once

// 3. 包含对应的 C 标准库
#include <cstdint>
#include <cstring>

// 4. 包含 C++ 标准库
#include <memory>
#include <string>
#include <vector>

// 5. 包含第三方库
#include <curl/curl.h>

// 6. 包含项目内部头文件
#include <falcon/types.hpp>
#include <falcon/plugin_interface.hpp>

// 7. 命名空间声明
namespace falcon {

// 8. 前向声明
class DownloadTask;
struct DownloadOptions;

// 9. 类定义
class DownloadEngine {
    // ...
};

} // namespace falcon
```

### 源文件结构

```cpp
// 1. 包含对应的头文件
#include "download_engine.hpp"

// 2. 包含 C 标准库
#include <cstring>

// 3. 包含 C++ 标准库
#include <algorithm>
#include <iostream>

// 4. 包含第三方库
#include <spdlog/spdlog.h>

// 5. 包含项目内部头文件
#include "internal/task_manager.hpp"
#include "internal/event_loop.hpp"

// 6. 匿名命名空间（内部链接）
namespace {

constexpr int kDefaultTimeout = 30;
std::string log_prefix = "[DownloadEngine]";

} // namespace

// 7. 命名空间
namespace falcon {

// 8. 静态成员定义
const int DownloadEngine::kMaxRetries = 3;

// 9. 成员函数定义
DownloadEngine::DownloadEngine()
    : task_manager_(std::make_unique<TaskManager>()) {
}

// 10. 自由函数
void helper_function() {
    // ...
}

} // namespace falcon
```

## 注释规范

### 文件注释

```cpp
/**
 * @file download_engine.hpp
 * @brief 下载引擎类定义
 * @author Falcon Team
 * @date 2025-12-25
 * @copyright Apache License 2.0
 */
```

### 类注释

```cpp
/**
 * @class DownloadEngine
 * @brief 高性能下载引擎
 *
 * 提供多协议下载支持，包括 HTTP、FTP、BitTorrent 等。
 * 支持断点续传、多线程下载、速度限制等功能。
 *
 * @example
 * @code
 * DownloadEngine engine;
 * engine.loadAllPlugins();
 * auto task = engine.startDownload("https://example.com/file.zip");
 * task->wait();
 * @endcode
 */
class DownloadEngine {
};
```

### 函数注释

```cpp
/**
 * @brief 开始下载任务
 *
 * 根据指定的 URL 和选项创建并启动下载任务。
 * 如果 URL 不被任何插件支持，将返回 nullptr。
 *
 * @param url 下载 URL，支持 http/https/ftp/magnet 等
 * @param options 下载选项，包括连接数、保存路径等
 * @param listener 事件监听器，用于接收进度和状态通知
 * @return std::shared_ptr<DownloadTask> 任务对象，失败返回 nullptr
 *
 * @throws InvalidURLException 当 URL 格式错误时
 * @throws PluginNotFoundException 当没有支持的插件时
 *
 * @note 调用此函数后需要调用 task->wait() 等待下载完成
 *
 * @see DownloadTask
 * @see DownloadOptions
 */
std::shared_ptr<DownloadTask> startDownload(
    const std::string& url,
    const DownloadOptions& options = {},
    IEventListener* listener = nullptr
);
```

### 成员变量注释

```cpp
class DownloadEngine {
private:
    /// 任务管理器，负责所有任务的生命周期管理
    std::unique_ptr<TaskManager> task_manager_;

    /// 插件管理器，负责协议插件的加载和注册
    std::unique_ptr<PluginManager> plugin_manager_;

    /// 事件循环，处理异步事件
    EventLoop event_loop_;

    /// 当前运行的任务数量
    std::atomic<int> active_tasks_;
};
```

### 行内注释

```cpp
// 使用常规注释解释代码意图
int result = calculate();  // 单行注释放在语句后面

// 多行注释
// 在复杂逻辑前添加解释
// 1. 首先检查连接状态
// 2. 然后发送请求
// 3. 最后处理响应
```

## 格式化

使用 **clang-format** 自动格式化，配置文件 `.clang-format`：

```yaml
BasedOnStyle: Google
IndentWidth: 4
ColumnLimit: 100
```

### 自动格式化

```bash
# 格式化所有文件
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format -i

# 检查格式
find . -name "*.cpp" -o -name "*.hpp" | xargs clang-format --dry-run --Werror
```

## 最佳实践

### RAII

```cpp
// 好：使用 RAII 管理资源
class FileHandle {
public:
    FileHandle(const std::string& path) : fd_(open(path.c_str(), O_RDONLY)) {
        if (fd_ < 0) {
            throw std::runtime_error("Failed to open file");
        }
    }

    ~FileHandle() {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    // 禁止拷贝
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // 允许移动
    FileHandle(FileHandle&& other) noexcept : fd_(other.fd_) {
        other.fd_ = -1;
    }

private:
    int fd_;
};
```

### 智能指针

```cpp
// 独占所有权
std::unique_ptr<DownloadTask> task = std::make_unique<DownloadTask>();

// 共享所有权
std::shared_ptr<DownloadEngine> engine = std::make_shared<DownloadEngine>();

// 弱引用，避免循环引用
std::weak_ptr<DownloadTask> weak_task = task;
```

### 异常安全

```cpp
// 好：使用 RAII，异常安全
std::lock_guard<std::mutex> lock(mutex_);

// 好：使用 unique_lock，可手动控制锁
std::unique_lock<std::mutex> lock(mutex_);
// ... 做一些事情
lock.unlock();

// 避免：裸指针和手动资源管理
// int* ptr = new int(42);
// delete ptr;  // 可能忘记释放或异常时未执行
```

### const 正确性

```cpp
// 好：标记为 const
int getValue() const { return value_; }
bool isEmpty() const { return items_.empty(); }
const std::string& getName() const { return name_; }

// 好：使用 const 引用传递
void process(const std::string& input);
void update(const Config& config);
```

### 默认参数

```cpp
// 好：使用默认参数
void download(const std::string& url,
              const DownloadOptions& options = {},
              IEventListener* listener = nullptr);

// 避免：函数重载仅用于提供默认值
void download(const std::string& url);
void download(const std::string& url, const DownloadOptions& options);
```

## 禁止事项

### ❌ 不要做的事情

```cpp
// 1. 不要使用 C 风格转换
// char* ptr = (char*)malloc(100);

// 好：使用 C++ 类型转换
char* ptr = static_cast<char*>(malloc(100));

// 2. 不要使用 NULL
// void* ptr = NULL;

// 好：使用 nullptr
void* ptr = nullptr;

// 3. 不要使用 using namespace std;
// using namespace std;

// 好：使用具体的 using 声明或不使用
using std::vector;

// 4. 不要使用 bool 作为位域
// struct Flag { unsigned int ready : 1; };

// 5. 不要在头文件中使用 using 指令
// using namespace std;

// 6. 不要使用全局变量
// int global_counter = 0;

// 好：使用命名空间内的常量或静态成员
namespace { const int kGlobalCounter = 0; }

// 7. 不要使用魔数
// if (size > 1073741824) { }

// 好：使用命名常量
constexpr int k1GB = 1073741824;
if (size > k1GB) { }
```

::: tip 代码审查
提交代码前，请确保代码符合以上规范。使用 `./scripts/check-format.sh` 检查格式。
:::
