# Falcon 下载器 - aria2 风格架构文档

## 概述

Falcon 下载器采用了与 aria2 类似的事件驱动架构，实现了高性能、可扩展的下载引擎。本文档详细说明了 Falcon 的核心架构设计。

## 架构设计原则

1. **命令模式 (Command Pattern)**: 所有下载操作都封装为命令对象
2. **事件驱动 (Event-Driven)**: 基于 I/O 多路复用的异步 I/O 模型
3. **连接复用 (Connection Pooling)**: Socket 连接池减少连接建立开销
4. **分段下载 (Segmented Download)**: 支持多线程分块下载提高速度
5. **任务队列 (Task Queue)**: 请求组管理实现任务调度和并发控制

## 核心组件

### 1. 命令系统 (Command System)

命令系统是 Falcon 的核心，所有下载操作都通过命令执行。

#### 命令基类

```cpp
class Command {
public:
    virtual bool execute(DownloadEngineV2* engine) = 0;
    CommandStatus status() const noexcept { return status_; }

protected:
    explicit Command(TaskId task_id);
    void mark_active();
    void mark_completed();
    bool handle_result(ExecutionResult result);
};
```

#### HTTP 命令链

HTTP 下载通过命令链实现：

```
HttpInitiateConnectionCommand (建立连接)
    ↓
HttpResponseCommand (解析响应头)
    ↓
HttpDownloadCommand (下载数据)
    ↓
HttpRetryCommand (失败重试，可选)
```

**命令状态转换:**
- READY → ACTIVE → COMPLETED
- READY → ACTIVE → ERROR

### 2. 事件轮询 (Event Poll)

跨平台 I/O 多路复用实现：

| 平台 | 实现 | 特点 |
|------|------|------|
| Linux | EPollEventPoll | 高性能，支持大量连接 |
| macOS/BSD | KqueueEventPoll | 高性能，原生支持 |
| 其他 | PollEventPoll | 可移植，性能较低 |

#### 使用示例

```cpp
auto poll = EventPoll::create();

// 注册读事件
poll->add_event(fd, static_cast<int>(IOEvent::READ),
    [](int fd, int events, void* user_data) {
        // 处理可读事件
    });

// 等待事件（100ms 超时）
int num_events = poll->poll(100);
```

### 3. Socket 连接池 (Socket Pool)

连接复用减少 TCP/TLS 握手开销：

#### SocketKey

```cpp
struct SocketKey {
    std::string host;
    uint16_t port;
    std::string username;
    std::string proxy;
};
```

#### 使用示例

```cpp
SocketPool pool(std::chrono::seconds(30), 16);  // 30s超时，最大16个空闲连接

// 获取连接
SocketKey key{"example.com", 80};
auto socket = pool.acquire(key);
if (socket && socket->is_valid()) {
    // 复用现有连接
} else {
    // 创建新连接
}

// 释放连接回池
pool.release(socket);
```

### 4. 请求组 (Request Group)

RequestGroup 表示一个完整的下载任务：

#### 生命周期

```
WAITING (等待执行)
    ↓
ACTIVE (正在下载)
    ↓
PAUSED / COMPLETED / ERROR / REMOVED
```

#### 多镜像支持

```cpp
std::vector<std::string> urls = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://mirror3.example.com/file.zip"
};

RequestGroup group(1, urls, options);

// 自动尝试下一个镜像
if (download_failed) {
    group.try_next_uri();
}
```

### 5. 请求组管理器 (RequestGroupMan)

管理多个下载任务的调度和并发控制：

#### 并发控制

```cpp
RequestGroupMan manager(5);  // 最多同时下载 5 个任务

// 添加10个任务
for (int i = 0; i < 10; ++i) {
    manager.add_request_group(std::make_unique<RequestGroup>(...));
}

// 从等待队列激活任务（最多5个）
manager.fill_request_group_from_reserver(engine);
```

### 6. 文件哈希验证 (File Hasher)

支持多种哈希算法验证文件完整性：

```cpp
// 计算 SHA256 哈希
auto hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA256);

// 验证文件
auto result = FileHasher::verify("file.zip",
    "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f",
    HashAlgorithm::SHA256);

if (result.valid) {
    std::cout << "文件验证通过\n";
}
```

## 事件驱动循环

DownloadEngineV2 的主事件循环：

```cpp
void DownloadEngineV2::run() {
    running_ = true;

    while (!is_shutdown_requested()) {
        // 1. 检查所有任务是否完成
        if (request_group_man_->all_completed()) {
            break;
        }

        // 2. 执行例程命令（定期任务）
        execute_routine_commands();

        // 3. 等待 I/O 事件
        int events = event_poll_->poll(config_.poll_timeout_ms);

        // 4. 处理就绪事件
        process_ready_events();

        // 5. 执行命令队列
        execute_commands();

        // 6. 清理已完成的命令
        cleanup_completed_commands();

        // 7. 更新任务状态
        update_task_status();

        // 8. 从等待队列激活新任务
        request_group_man_->fill_request_group_from_reserver(this);
    }

    running_ = false;
}
```

## 与 aria2 的对比

| 特性 | aria2 | Falcon |
|------|-------|--------|
| 架构 | 事件驱动 + 命令模式 | ✅ 事件驱动 + 命令模式 |
| I/O 多路复用 | epoll/kqueue/IOCP | ✅ epoll/kqueue/poll |
| 连接池 | ✅ | ✅ |
| 分段下载 | ✅ | ✅ |
| 多镜像支持 | ✅ | ✅ |
| 命令行参数 | 丰富 | ✅ 30+ aria2 兼容参数 |
| RPC 接口 | XML-RPC/JSON-RPC | 🚧 计划中 |
| BitTorrent | ✅ | 🚧 计划中 |
| Metalink | ✅ | ❌ 未计划 |

## 性能优化

### 1. 连接复用

- 复用 HTTP/HTTPS 连接，避免重复握手
- 30 秒空闲超时，最多保留 16 个空闲连接

### 2. 非阻塞 I/O

- 所有 Socket 设置为非阻塞模式
- 事件驱动避免线程阻塞

### 3. 零拷贝

- 使用 `sendfile`/`splice` 系统调用（计划中）
- 减少用户态/内核态数据拷贝

### 4. 内存管理

- 智能指针自动管理内存
- RAII 确保资源正确释放

## 扩展性

### 添加新的协议处理器

1. 继承 `AbstractCommand`
2. 实现 `execute()` 方法
3. 在 `RequestGroup::create_initial_command()` 中注册

### 添加新的命令类型

```cpp
class MyCustomCommand : public AbstractCommand {
public:
    bool execute(DownloadEngineV2* engine) override {
        // 实现命令逻辑
        return handle_result(ExecutionResult::OK);
    }
};
```

## 线程安全

### 线程模型

- **主线程**: 事件循环和命令调度
- **工作线程**: 文件 I/O 和哈希计算（计划中）

### 锁策略

- 命令队列使用 `std::mutex` 保护
- Socket 连接池使用线程安全队列
- 请求组管理器使用读写锁（计划中）

## 错误处理

### 命令执行错误

```cpp
bool execute(DownloadEngineV2* engine) override {
    try {
        // 执行逻辑
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("命令执行失败: " << e.what());
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }
}
```

### 重试机制

```cpp
HttpRetryCommand retry_cmd(task_id, url, options, retry_count);
if (retry_cmd.should_retry()) {
    // 自动重试
}
```

## 配置选项

### 引擎配置

```cpp
struct EngineConfigV2 {
    std::size_t max_concurrent_tasks = 5;
    std::size_t global_speed_limit = 0;
    int poll_timeout_ms = 100;
    bool enable_disk_cache = true;
    std::size_t disk_cache_size = 4 * 1024 * 1024;
};
```

### 下载选项

```cpp
struct DownloadOptions {
    std::size_t max_connections = 1;
    std::size_t timeout_seconds = 30;
    std::size_t max_retries = 3;
    std::string output_directory;
    std::string output_filename;
    std::size_t speed_limit = 0;
    bool resume_if_exists = true;
};
```

## 最佳实践

### 1. 使用连接池

```cpp
// 推荐：复用连接
auto socket = socket_pool->acquire(key);
if (socket && socket->is_valid()) {
    use_existing_connection(socket);
} else {
    create_new_connection();
}
```

### 2. 错误处理

```cpp
// 推荐：捕获并处理异常
try {
    engine->run();
} catch (const std::exception& e) {
    std::cerr << "下载失败: " << e.what() << std::endl;
}
```

### 3. 资源清理

```cpp
// 推荐：使用 RAII
{
    auto poll = EventPoll::create();
    // 使用 poll
} // 自动清理
```

## 未来计划

### 短期 (1-3 个月)

- [ ] 完善分段下载实现
- [ ] 添加 RPC 接口
- [ ] 支持 BitTorrent 协议
- [ ] 添加更多单元测试

### 中期 (3-6 个月)

- [ ] 支持 Metalink
- [ ] 实现 SFTP 协议
- [ ] 添加图形界面 (GUI)
- [ ] 性能优化和基准测试

### 长期 (6-12 个月)

- [ ] 支持 P2P 协议
- [ ] 分布式下载
- [ ] 云存储集成
- [ ] 移动端支持

## 参考资料

- [aria2 源码](https://github.com/aria2/aria2)
- [aria2 设计文档](https://aria2.github.io/manual/en/html/aria2c.html)
- [libevent 文档](https://libevent.org/)
- [Linux epoll 手册](https://man7.org/linux/man-pages/man7/epoll.7.html)

## 贡献指南

欢迎贡献代码！请遵循以下步骤：

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 开启 Pull Request

### 代码规范

- 遵循 Google C++ Style Guide
- 使用 clang-format 格式化代码
- 添加单元测试覆盖新功能
- 更新相关文档

## 许可证

Apache License 2.0 - 详见 [LICENSE](../LICENSE) 文件
