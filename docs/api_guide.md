# Falcon API 使用指南

## 快速开始

### 基础下载

```cpp
#include <falcon/download_engine_v2.hpp>
#include <falcon/download_options.hpp>

using namespace falcon;

int main() {
    // 1. 创建引擎配置
    EngineConfigV2 config;
    config.max_concurrent_tasks = 5;
    config.global_speed_limit = 0;  // 无限制

    // 2. 创建下载引擎
    DownloadEngineV2 engine(config);

    // 3. 配置下载选项
    DownloadOptions options;
    options.max_connections = 4;  // 4 个分段同时下载
    options.output_directory = "/tmp/downloads";
    options.timeout_seconds = 30;

    // 4. 添加下载任务
    TaskId task_id = engine.add_download(
        "https://example.com/large_file.zip",
        options
    );

    // 5. 启动事件循环（阻塞直到完成）
    engine.run();

    // 6. 获取统计信息
    auto stats = engine.get_statistics();
    std::cout << "下载完成: " << stats.total_downloaded << " 字节\n";

    return 0;
}
```

## 详细 API 说明

### DownloadEngineV2 - 下载引擎

#### 创建引擎

```cpp
// 使用默认配置
DownloadEngineV2 engine;

// 使用自定义配置
EngineConfigV2 config;
config.max_concurrent_tasks = 10;
config.poll_timeout_ms = 50;
config.enable_disk_cache = true;
config.disk_cache_size = 8 * 1024 * 1024;  // 8MB

DownloadEngineV2 engine(config);
```

#### 添加下载任务

```cpp
// 单 URL 下载
TaskId id1 = engine.add_download(
    "http://example.com/file.zip",
    options
);

// 多镜像下载（自动故障切换）
std::vector<std::string> urls = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://mirror3.example.com/file.zip"
};
TaskId id2 = engine.add_download(urls, options);
```

#### 任务控制

```cpp
// 暂停任务
engine.pause_task(task_id);

// 恢复任务
engine.resume_task(task_id);

// 取消任务
engine.cancel_task(task_id);

// 批量操作
engine.pause_all();
engine.resume_all();
engine.cancel_all();  // 相当于 shutdown()
```

#### 统计信息

```cpp
auto stats = engine.get_statistics();
std::cout << "活动任务: " << stats.active_tasks << "\n";
std::cout << "等待任务: " << stats.waiting_tasks << "\n";
std::cout << "已完成: " << stats.completed_tasks << "\n";
std::cout << "下载速度: " << (stats.global_download_speed / 1024) << " KB/s\n";
std::cout << "总下载量: " << (stats.total_downloaded / 1024 / 1024) << " MB\n";
```

### DownloadOptions - 下载选项

#### 基础选项

```cpp
DownloadOptions options;

// 并发设置
options.max_connections = 8;              // 每个 HTTP 连接最多 8 个分段
options.min_segment_size = 1024 * 1024;   // 最小分段大小 1MB
options.split = 5;                        // 最多分 5 个部分下载

// 超时设置
options.timeout_seconds = 30;             // 连接超时 30 秒
options.connect_timeout = 10;             // 连接建立超时 10 秒

// 重试设置
options.max_retries = 5;                  // 最多重试 5 次
options.retry_wait = 5;                   // 重试等待 5 秒

// 输出设置
options.output_directory = "/tmp/downloads";
options.output_filename = "custom_name.zip";
options.overwrite = false;                // 不覆盖已存在文件
```

#### HTTP 选项

```cpp
// User-Agent
options.user_agent = "Falcon/1.0 (https://github.com/falcon-downloader)";

// 自定义 HTTP 头
options.headers = {
    {"Authorization", "Bearer token123"},
    {"Accept", "application/json"}
};

// 引用页
options.referer = "https://example.com";

// 认证
options.http_user = "username";
options.http_passwd = "password";
```

#### 速度限制

```cpp
// 单任务限速
options.speed_limit = 1024 * 1024;  // 1 MB/s

// 全局限速（在引擎配置中）
EngineConfigV2 config;
config.global_speed_limit = 10 * 1024 * 1024;  // 10 MB/s
```

#### 断点续传

```cpp
options.resume_if_exists = true;  // 自动续传
options.resume_position = 1024 * 1024;  // 从 1MB 位置开始
```

### 命令行参数

Falcon CLI 支持 aria2 兼容的命令行参数：

#### 基础参数

```bash
# 单文件下载
falcon-cli https://example.com/file.zip

# 指定输出目录
falcon-cli -d /tmp/downloads https://example.com/file.zip

# 指定输出文件名
falcon-cli -o custom_name.zip https://example.com/file.zip

# 限制并发数
falcon-cli -x 8 -s 16 https://example.com/file.zip
# -x: 每个任务最大连接数 (默认: 5)
# -s: 单服务器最大连接数 (默认: 5)
```

#### 进度显示

```bash
# 显示简化进度条
falcon-cli --simple-progress=true https://example.com/file.zip

# 控制台输出
falcon-cli --console-log-level=notice https://example.com/file.zip
```

#### 重试和超时

```bash
# 设置重试次数和等待时间
falcon-cli --max-tries=5 --retry-wait=10 https://example.com/file.zip

# 设置超时
falcon-cli --timeout=30 --connect-timeout=10 https://example.com/file.zip
```

#### 速度限制

```bash
# 限制下载速度
falcon-cli --max-download-limit=1M https://example.com/file.zip

# 设置全局速度限制
falcon-cli --max-overall-download-limit=10M https://example.com/file.zip
```

#### RPC 参数（计划中）

```bash
# 启用 RPC
falcon-daemon --enable-rpc=true --rpc-listen-port=6800

# 设置 RPC 密钥
falcon-daemon --rpc-secret=mysecrettoken

# 允许跨域请求
falcon-daemon --rpc-allow-origin-all=true
```

### 文件哈希验证

#### 计算文件哈希

```cpp
#include <falcon/file_hash.hpp>

// 计算 SHA256 哈希
auto sha256_hash = FileHasher::calculate(
    "file.zip",
    HashAlgorithm::SHA256
);
std::cout << "SHA256: " << sha256_hash << std::endl;

// 计算其他算法
auto md5_hash = FileHasher::calculate("file.zip", HashAlgorithm::MD5);
auto sha1_hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA1);
auto sha512_hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA512);
```

#### 验证文件完整性

```cpp
// 验证下载的文件
auto result = FileHasher::verify(
    "downloaded_file.zip",
    "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f",
    HashAlgorithm::SHA256
);

if (result.valid) {
    std::cout << "文件验证通过\n";
} else {
    std::cout << "文件验证失败！\n";
    std::cout << "期望: " << result.expected << "\n";
    std::cout << "实际: " << result.calculated << "\n";
}
```

#### 批量验证

```cpp
std::vector<std::pair<std::string, HashAlgorithm>> hashes = {
    {"abc123...", HashAlgorithm::MD5},
    {"def456...", HashAlgorithm::SHA1},
    {"ghi789...", HashAlgorithm::SHA256}
};

auto results = FileHasher::verify_multiple("file.zip", hashes);

for (const auto& result : results) {
    std::cout << "算法: " << static_cast<int>(result.algorithm)
              << " 有效: " << result.valid << "\n";
}
```

#### 自动检测算法

```cpp
// 根据哈希长度自动检测算法
std::string hash_str = "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f";
auto algorithm = FileHasher::detect_algorithm(hash_str);

if (algorithm == HashAlgorithm::SHA256) {
    std::cout << "检测到 SHA256 哈希\n";
}
```

### Socket 连接池

#### 创建连接池

```cpp
#include <falcon/net/socket_pool.hpp>

using namespace falcon::net;

// 30 秒空闲超时，最多保留 16 个空闲连接
SocketPool pool(std::chrono::seconds(30), 16);
```

#### 获取和释放连接

```cpp
// 定义连接键
SocketKey key;
key.host = "example.com";
key.port = 80;
key.username = "";  // HTTP 基本认证（可选）
key.proxy = "";    // 代理地址（可选）

// 获取连接
auto socket = pool.acquire(key);
if (socket && socket->is_valid()) {
    int fd = socket->fd();

    // 使用连接发送 HTTP 请求
    send_http_request(fd);

    // 释放连接回池
    pool.release(socket);
} else {
    // 创建新连接
    int fd = create_new_connection();
    auto new_socket = std::make_shared<PooledSocket>(fd, key);
    pool.release(new_socket);
}
```

### 事件轮询

#### 创建 EventPoll

```cpp
#include <falcon/net/event_poll.hpp>

using namespace falcon::net;

// 自动选择平台最优实现
auto poll = EventPoll::create();
```

#### 注册事件

```cpp
// 注册读事件
poll->add_event(socket_fd, static_cast<int>(IOEvent::READ),
    [](int fd, int events, void* user_data) {
        char buffer[4096];
        ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n > 0) {
            // 处理数据
        }
    });

// 注册写事件
poll->add_event(socket_fd, static_cast<int>(IOEvent::WRITE),
    [](int fd, int events, void* user_data) {
        const char* data = "GET / HTTP/1.1\r\n\r\n";
        send(fd, data, strlen(data), 0);
    });

// 注册错误事件
poll->add_event(socket_fd, static_cast<int>(IOEvent::ERROR | IOEvent::HANGUP),
    [](int fd, int events, void* user_data) {
        close(fd);
    });
```

#### 修改事件

```cpp
// 从读事件改为写事件
poll->modify_event(socket_fd, static_cast<int>(IOEvent::WRITE));
```

#### 移除事件

```cpp
// 移除事件监听
poll->remove_event(socket_fd);
```

#### 事件循环

```cpp
while (!should_stop) {
    // 等待事件（100ms 超时）
    int num_events = poll->poll(100);

    if (num_events > 0) {
        // 处理就绪事件（通过回调函数自动处理）
    } else if (num_events == 0) {
        // 超时，执行定期任务
    } else {
        // 错误
        std::cerr << "poll() 错误: " << poll->get_error() << std::endl;
        break;
    }
}
```

## 高级用法

### 多任务并发下载

```cpp
DownloadEngineV2 engine;

std::vector<std::string> urls = {
    "https://example.com/file1.zip",
    "https://example.com/file2.zip",
    "https://example.com/file3.zip"
};

for (const auto& url : urls) {
    engine.add_download(url, options);
}

engine.run();
```

### 自定义进度回调

```cpp
// TODO: 添加进度回调接口
// ProgressCallback callback = [](TaskId id, float progress) {
//     std::cout << "任务 " << id << ": " << (progress * 100) << "%\n";
// };
// engine.set_progress_callback(callback);
```

### 镜像故障切换

```cpp
std::vector<std::string> mirrors = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://backup.example.com/file.zip"
};

RequestGroup group(1, mirrors, options);

// 尝试下一个镜像
while (!download_complete) {
    if (download_failed) {
        if (group.try_next_uri()) {
            // 重试下一个镜像
        } else {
            // 所有镜像都失败
            break;
        }
    }
}
```

## 错误处理

### 异常处理

```cpp
try {
    engine.run();
} catch (const NetworkException& e) {
    std::cerr << "网络错误: " << e.what() << std::endl;
} catch (const FileException& e) {
    std::cerr << "文件错误: " << e.what() << std::endl;
} catch (const std::exception& e) {
    std::cerr << "未知错误: " << e.what() << std::endl;
}
```

### 日志配置

```cpp
// TODO: 添加日志配置接口
// Logger::set_level(LogLevel::DEBUG);
// Logger::add_file_handler("/tmp/falcon.log");
```

## 性能优化建议

### 1. 合理设置并发数

```cpp
// 根据网络带宽调整
options.max_connections = 16;  // 高速网络
options.max_connections = 4;   // 低速网络
```

### 2. 启用连接复用

```cpp
// 连接池自动复用，无需额外配置
SocketPool pool(std::chrono::seconds(30), 16);
```

### 3. 分段下载

```cpp
// 启用多分段下载加速
options.min_segment_size = 1024 * 1024;  // 1MB 以上才分段
options.split = 8;                        // 分 8 段下载
```

### 4. 速度限制

```cpp
// 避免占满带宽
options.speed_limit = 5 * 1024 * 1024;  // 限制 5 MB/s
```

## 常见问题

### Q: 如何断点续传？

A: Falcon 会自动检测已下载的部分：

```cpp
options.resume_if_exists = true;
```

### Q: 如何下载到特定目录？

A: 设置输出目录：

```cpp
options.output_directory = "/path/to/downloads";
```

### Q: 如何限制下载速度？

A: 使用速度限制选项：

```cpp
options.speed_limit = 1024 * 1024;  // 1 MB/s
```

### Q: 如何验证文件完整性？

A: 使用文件哈希验证：

```cpp
auto result = FileHasher::verify("file.zip", expected_hash, HashAlgorithm::SHA256);
```

## 参考文档

- [架构文档](aria2_architecture.md)
- [迁移计划](aria2c-migration-plan.md)
- [协议支持](protocol_support.md)
- [私有协议](private_protocols.md)
