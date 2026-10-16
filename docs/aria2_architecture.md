[中文](aria2_architecture.zh.md) | **English**

# Falcon Downloader - aria2-Style Architecture Document

> [!NOTE]
> This document is architecture research material, not an authoritative interface description of the current implementation.

## Overview

The Falcon downloader adopts an event-driven architecture similar to aria2. This document describes its core architecture design.

## Architecture Design Principles

1. **Command Pattern**: All download operations are encapsulated as command objects
2. **Event-Driven**: An asynchronous I/O model based on I/O multiplexing
3. **Connection Pooling**: A socket connection pool that reduces connection establishment overhead
4. **Segmented Download**: Multi-threaded block downloads that increase speed
5. **Task Queue**: Request groups implement task scheduling and concurrency control

## Core Components

### 1. Command System

The command system is the heart of Falcon; every download operation runs through commands.

#### Command Base Class

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

#### HTTP Command Chain

An HTTP download runs through a chain of commands:

```
HttpInitiateConnectionCommand (establish connection)
    ↓
HttpResponseCommand (parse response headers)
    ↓
HttpDownloadCommand (download data)
    ↓
HttpRetryCommand (retry on failure, optional)
```

**Command state transitions:**
- READY → ACTIVE → COMPLETED
- READY → ACTIVE → ERROR

### 2. Event Poll

Cross-platform I/O multiplexing implementation:

| Platform | Implementation | Characteristics |
|------|------|------|
| Linux | EPollEventPoll | Supports a large number of connections |
| macOS/BSD | KqueueEventPoll | Native system interface |
| Other | PollEventPoll | Portable, lower performance |

#### Usage Example

```cpp
auto poll = EventPoll::create();

// Register a read event
poll->add_event(fd, static_cast<int>(IOEvent::READ),
    [](int fd, int events, void* user_data) {
        // Handle readable event
    });

// Wait for events (100ms timeout)
int num_events = poll->poll(100);
```

### 3. Socket Pool

Connection reuse reduces TCP/TLS handshake overhead:

#### SocketKey

```cpp
struct SocketKey {
    std::string host;
    uint16_t port;
    std::string username;
    std::string proxy;
};
```

#### Usage Example

```cpp
SocketPool pool(std::chrono::seconds(30), 16);  // 30s timeout, at most 16 idle connections

// Acquire a connection
SocketKey key{"example.com", 80};
auto socket = pool.acquire(key);
if (socket && socket->is_valid()) {
    // Reuse the existing connection
} else {
    // Create a new connection
}

// Release the connection back to the pool
pool.release(socket);
```

### 4. Request Group

A `RequestGroup` represents a complete download task:

#### Lifecycle

```
WAITING (waiting to run)
    ↓
ACTIVE (downloading)
    ↓
PAUSED / COMPLETED / ERROR / REMOVED
```

#### Multi-Mirror Support

```cpp
std::vector<std::string> urls = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://mirror3.example.com/file.zip"
};

RequestGroup group(1, urls, options);

// Automatically try the next mirror
if (download_failed) {
    group.try_next_uri();
}
```

### 5. RequestGroupMan

Manages scheduling and concurrency control for multiple download tasks:

#### Concurrency Control

```cpp
RequestGroupMan manager(5);  // At most 5 concurrent downloads

// Add 10 tasks
for (int i = 0; i < 10; ++i) {
    manager.add_request_group(std::make_unique<RequestGroup>(...));
}

// Activate tasks from the waiting queue (at most 5)
manager.fill_request_group_from_reserver(engine);
```

### 6. File Hasher

Supports multiple hash algorithms for verifying file integrity:

```cpp
// Calculate a SHA256 hash
auto hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA256);

// Verify a file
auto result = FileHasher::verify("file.zip",
    "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f",
    HashAlgorithm::SHA256);

if (result.valid) {
    std::cout << "File verified\n";
}
```

## Event-Driven Loop

The main event loop of DownloadEngineV2:

```cpp
void DownloadEngineV2::run() {
    running_ = true;

    while (!is_shutdown_requested()) {
        // 1. Check whether all tasks are complete
        if (request_group_man_->all_completed()) {
            break;
        }

        // 2. Execute routine commands (periodic work)
        execute_routine_commands();

        // 3. Wait for I/O events
        int events = event_poll_->poll(config_.poll_timeout_ms);

        // 4. Process ready events
        process_ready_events();

        // 5. Execute the command queue
        execute_commands();

        // 6. Clean up completed commands
        cleanup_completed_commands();

        // 7. Update task status
        update_task_status();

        // 8. Activate new tasks from the waiting queue
        request_group_man_->fill_request_group_from_reserver(this);
    }

    running_ = false;
}
```

## Comparison with aria2

| Feature | aria2 | Falcon |
|------|-------|--------|
| Architecture | Event-driven + command pattern | Event-driven + command pattern |
| I/O multiplexing | epoll/kqueue/IOCP | epoll/kqueue/poll |
| Connection pool | Yes | Yes |
| Segmented download | Yes | Yes |
| Multi-mirror support | Yes | Yes |
| CLI arguments | Extensive | 60+ aria2-compatible arguments |
| RPC interface | XML-RPC/JSON-RPC | JSON-RPC (HTTP and WebSocket on the same port, provided by falcon-daemon) |
| BitTorrent | Yes | Yes (libtorrent) |
| Metalink | Yes | Yes (vendored libmetalink) |

## Performance Optimization

### 1. Connection Reuse

- Reuse HTTP/HTTPS connections to avoid repeated handshakes
- 30-second idle timeout, at most 16 idle connections kept

### 2. Non-Blocking I/O

- All sockets are set to non-blocking mode
- Event-driven design avoids thread blocking

### 3. Zero Copy

- Use `sendfile`/`splice` system calls (planned)
- Reduce data copies between user space and kernel space

### 4. Memory Management

- Smart pointers manage memory automatically
- RAII ensures resources are released correctly

## Extensibility

### Adding a New Protocol Handler

1. Inherit from `AbstractCommand`
2. Implement the `execute()` method
3. Register it in `RequestGroup::create_initial_command()`

### Adding a New Command Type

```cpp
class MyCustomCommand : public AbstractCommand {
public:
    bool execute(DownloadEngineV2* engine) override {
        // Implement command logic
        return handle_result(ExecutionResult::OK);
    }
};
```

## Thread Safety

### Thread Model

- **Main thread**: Event loop and command scheduling
- **Worker threads**: File I/O and hash computation (planned)

### Locking Strategy

- The command queue is protected by `std::mutex`
- The socket pool uses a thread-safe queue
- The request group manager uses a read-write lock (planned)

## Error Handling

### Command Execution Errors

```cpp
bool execute(DownloadEngineV2* engine) override {
    try {
        // Execution logic
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Command execution failed: " << e.what());
        return handle_result(ExecutionResult::ERROR_OCCURRED);
    }
}
```

### Retry Mechanism

```cpp
HttpRetryCommand retry_cmd(task_id, url, options, retry_count);
if (retry_cmd.should_retry()) {
    // Retry automatically
}
```

## Configuration Options

### Engine Configuration

```cpp
struct EngineConfigV2 {
    std::size_t max_concurrent_tasks = 5;
    std::size_t global_speed_limit = 0;
    int poll_timeout_ms = 100;
    bool enable_disk_cache = true;
    std::size_t disk_cache_size = 4 * 1024 * 1024;
};
```

### Download Options

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

## Best Practices

### 1. Use the Connection Pool

```cpp
// Recommended: reuse connections
auto socket = socket_pool->acquire(key);
if (socket && socket->is_valid()) {
    use_existing_connection(socket);
} else {
    create_new_connection();
}
```

### 2. Error Handling

```cpp
// Recommended: catch and handle exceptions
try {
    engine->run();
} catch (const std::exception& e) {
    std::cerr << "Download failed: " << e.what() << std::endl;
}
```

### 3. Resource Cleanup

```cpp
// Recommended: use RAII
{
    auto poll = EventPoll::create();
    // Use poll
} // Cleaned up automatically
```

## Future Plans

The following list was written during the V2 engine design phase and is kept for archival purposes only. The RPC interface, BitTorrent, Metalink, SFTP, and the graphical interface have all shipped since then; for current progress see the roadmap in [README](../README.md).

### Short Term (1-3 months)

- [ ] Complete the segmented download implementation
- [ ] Add an RPC interface
- [ ] Support the BitTorrent protocol
- [ ] Add more unit tests

### Mid Term (3-6 months)

- [ ] Support Metalink
- [ ] Implement the SFTP protocol
- [ ] Add a graphical interface (GUI)
- [ ] Performance optimization and benchmarking

### Long Term (6-12 months)

- [ ] Support P2P protocols
- [ ] Distributed downloading
- [ ] Cloud storage integration
- [ ] Mobile support

## References

- [aria2 source code](https://github.com/aria2/aria2)
- [aria2 design document](https://aria2.github.io/manual/en/html/aria2c.html)
- [libevent documentation](https://libevent.org/)
- [Linux epoll manual](https://man7.org/linux/man-pages/man7/epoll.7.html)

## Contributing

Contributions are welcome! Please follow these steps:

1. Fork this repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

### Code Style

- Follow the Google C++ Style Guide
- Format code with clang-format
- Add unit tests covering new features
- Update the relevant documentation

## License

Apache License 2.0 - see the [LICENSE](../LICENSE) file
