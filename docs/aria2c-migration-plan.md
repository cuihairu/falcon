# aria2c 核心功能迁移计划

> 基于对 [aria2/aria2](https://github.com/aria2/aria2) 源码的深度分析，制定 Falcon 下载器的核心功能实现路线图。

## 文档版本

- **创建日期**: 2025-12-24
- **最后更新**: 2025-12-25
- **aria2c 分析版本**: 1.37.0 (commit: b519ce04)
- **参考来源**: [aria2 DeepWiki 文档](https://github.com/aria2/aria2)
- **实现状态**: ✅ Phase 1-6 已完成

---

## 实现进度总结

| Phase | 名称 | 状态 | 完成日期 |
|-------|------|------|----------|
| Phase 1 | 命令系统 (Command Pattern) | ✅ 完成 | 2025-12-25 |
| Phase 2.1 | EventPoll 接口 | ✅ 完成 | 2025-12-25 |
| Phase 2.2 | 事件驱动 DownloadEngineV2 | ✅ 完成 | 2025-12-25 |
| Phase 3 | RequestGroup 管理 | ✅ 完成 | 2025-12-25 |
| Phase 4 | Socket 连接池 | ✅ 完成 | 2025-12-25 |
| Phase 5 | aria2 兼容命令行参数 | ✅ 完成 | 2025-12-25 |
| Phase 6 | 文件校验功能 (SHA-1/SHA256/MD5) | ✅ 完成 | 2025-12-25 |

---

## 一、aria2c 核心架构分析

### 1.1 架构总览

```
┌─────────────────────────────────────────────────────────────┐
│                         aria2c 架构                          │
├─────────────────────────────────────────────────────────────┤
│                                                               │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │   CLI/UI    │───▶│DownloadEngine│◀───│  RPC Server │     │
│  │   入口      │    │   (核心引擎)  │    │  (远程控制)  │     │
│  └─────────────┘    └──────┬──────┘    └─────────────┘     │
│                             │                                │
│                    ┌────────▼────────┐                       │
│                    │ RequestGroupMan │                       │
│                    │  (请求组管理器)  │                       │
│                    └────────┬────────┘                       │
│                             │                                │
│         ┌───────────────────┼───────────────────┐            │
│         ▼                   ▼                   ▼            │
│  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐     │
│  │RequestGroup │    │RequestGroup │    │RequestGroup │...  │
│  │ (下载任务1)  │    │ (下载任务2)  │    │ (下载任务N)  │     │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘     │
│         │                   │                   │            │
│  ┌──────▼──────┐    ┌──────▼──────┐    ┌──────▼──────┐     │
│  │ SegmentMan  │    │ SegmentMan  │    │ SegmentMan  │     │
│  │(分段管理器)  │    │(分段管理器)  │    │(分段管理器)  │     │
│  └──────┬──────┘    └──────┬──────┘    └──────┬──────┘     │
│         │                   │                   │            │
│  ┌──────▼───────────────────▼───────────────────▼──────┐    │
│  │              Command Queue (命令队列)                 │    │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐   │    │
│  │  │HttpInit ││Download ││FtpInit  ││BtPeer   │... │    │
│  │  │Command  ││Command  ││Command  ││Command  │   │    │
│  │  └─────────┘ └─────────┘ └─────────┘ └─────────┘   │    │
│  └────────────────────────────────────────────────────┘    │
│                             │                                │
│                    ┌────────▼────────┐                       │
│                    │   EventPoll     │                       │
│                    │ (epoll/kqueue)  │                       │
│                    └────────┬────────┘                       │
│                             │                                │
│  ┌──────────────────────────▼───────────────────────────┐   │
│  │              协议实现层 (Protocol Layer)               │   │
│  │  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐ ┌────────┐     │   │
│  │  │ HTTP │ │ FTP  │ │ SFTP │ │  BT  │ │Metalink│     │   │
│  │  └──────┘ └──────┘ └──────┘ └──────┘ └────────┘     │   │
│  └──────────────────────────────────────────────────────┘   │
│                             │                                │
│  ┌──────────────────────────▼───────────────────────────┐   │
│  │              磁盘 I/O 层 (Disk I/O)                    │   │
│  │  ┌─────────────┐    ┌─────────────┐                   │   │
│  │  │DiskAdaptor  │    │AbstractDisk │                   │   │
│  │  │(磁盘适配器)  │    │Writer(写入器) │                   │   │
│  │  └─────────────┘    └─────────────┘                   │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

### 1.2 核心类对比分析

| aria2c 类名 | Falcon 对应类 | 完成度 | 说明 |
|------------|--------------|--------|------|
| `DownloadEngine` | `DownloadEngine` | 70% | 需增强事件驱动 |
| `RequestGroupMan` | `TaskManager` | 65% | 需增强队列管理 |
| `RequestGroup` | `DownloadTask` | 75% | 需增强状态管理 |
| `SegmentMan` | `SegmentDownloader` | 80% | 基本完成 |
| `Command` | 无 | 0% | **需新增** |
| `FileEntry` | `FileInfo` | 60% | 需增强 |
| `Option` | `DownloadOptions` | 70% | 需扩展选项 |
| `OptionHandler` | 无 | 0% | **需新增** |

---

## 二、核心功能迁移路线图

### Phase 1: 命令系统重构 (Command Pattern)

**目标**: 实现 aria2 风格的事件驱动命令系统

#### 1.1 Command 基类设计

```cpp
// packages/libfalcon/include/falcon/commands/command.hpp

#pragma once

#include <falcon/types.hpp>
#include <memory>

namespace falcon {

class DownloadEngine;

/**
 * @brief 命令基类 - aria2 风格
 *
 * 所有下载操作的基类，采用命令模式实现事件驱动的下载流程
 */
class Command {
public:
    enum class Status {
        READY,       // 准备执行
        ACTIVE,      // 正在执行
        COMPLETED,   // 已完成
        ERROR        // 错误状态
    };

    virtual ~Command() = default;

    /**
     * @brief 执行命令
     * @return true 如果命令已完成并应从队列移除
     * @return false 如果命令需要再次执行（等待 I/O）
     */
    virtual bool execute(DownloadEngine* engine) = 0;

    /**
     * @brief 获取命令状态
     */
    Status status() const noexcept { return status_; }

    /**
     * @brief 获取命令名称（用于调试）
     */
    virtual const char* name() const = 0;

protected:
    Status status_ = Status::READY;
    TaskId task_id_ = 0;
};

/**
 * @brief 抽象命令基类 - 提供通用功能
 */
class AbstractCommand : public Command {
public:
    AbstractCommand(TaskId task_id) : task_id_(task_id) {}

    TaskId task_id() const noexcept { return task_id_; }

protected:
    void transition(Status new_status) {
        status_ = new_status;
    }
};

} // namespace falcon
```

#### 1.2 HTTP 命令实现

```cpp
// packages/libfalcon/include/falcon/commands/http_commands.hpp

#pragma once

#include <falcon/commands/command.hpp>
#include <falcon/http/request.hpp>
#include <falcon/http/response.hpp>

namespace falcon::commands {

/**
 * @brief HTTP 连接初始化命令
 *
 * 对应 aria2 的 HttpInitiateConnectionCommand
 */
class HttpInitiateConnectionCommand : public AbstractCommand {
public:
    HttpInitiateConnectionCommand(TaskId task_id,
                                   std::shared_ptr<HttpRequest> request);

    bool execute(DownloadEngine* engine) override;
    const char* name() const override { return "HttpInitiateConnection"; }

private:
    std::shared_ptr<HttpRequest> request_;
};

/**
 * @brief HTTP 响应处理命令
 *
 * 对应 aria2 的 HttpResponseCommand
 */
class HttpResponseCommand : public AbstractCommand {
public:
    HttpResponseCommand(TaskId task_id,
                         std::shared_ptr<HttpResponse> response);

    bool execute(DownloadEngine* engine) override;
    const char* name() const override { return "HttpResponse"; }

private:
    std::shared_ptr<HttpResponse> response_;
};

/**
 * @brief HTTP 下载数据命令
 *
 * 对应 aria2 的 HttpDownloadCommand
 */
class HttpDownloadCommand : public AbstractCommand {
public:
    HttpDownloadCommand(TaskId task_id,
                         std::shared_ptr<HttpResponse> response,
                         SegmentId segment_id);

    bool execute(DownloadEngine* engine) override;
    const char* name() const override { return "HttpDownload"; }

private:
    std::shared_ptr<HttpResponse> response_;
    SegmentId segment_id_;
};

} // namespace falcon::commands
```

**文件清单**:
- `packages/libfalcon/include/falcon/commands/command.hpp`
- `packages/libfalcon/include/falcon/commands/http_commands.hpp`
- `packages/libfalcon/src/commands/http_commands.cpp`

---

### Phase 2: 事件驱动的 DownloadEngine

**目标**: 实现类似 aria2 的事件循环和 I/O 多路复用

#### 2.1 EventPoll 接口

```cpp
// packages/libfalcon/include/falcon/net/event_poll.hpp

#pragma once

#include <falcon/types.hpp>
#include <functional>
#include <map>
#include <memory>

namespace falcon::net {

/**
 * @brief I/O 事件类型
 */
enum class IOEvent {
    READ = 1,
    WRITE = 2,
    ERROR = 4
};

/**
 * @brief 事件回调函数
 */
using EventCallback = std::function<void(int fd, int events)>;

/**
 * @brief I/O 多路复用接口
 *
 * 跨平台封装：epoll (Linux), kqueue (macOS/BSD), WSAPoll (Windows)
 */
class EventPoll {
public:
    virtual ~EventPoll() = default;

    /**
     * @brief 添加文件描述符监听
     */
    virtual void add_event(int fd, int events, EventCallback cb) = 0;

    /**
     * @brief 修改文件描述符事件
     */
    virtual void modify_event(int fd, int events) = 0;

    /**
     * @brief 移除文件描述符监听
     */
    virtual void remove_event(int fd) = 0;

    /**
     * @brief 等待事件（带超时）
     * @return 发生的事件数量
     */
    virtual int poll(int timeout_ms) = 0;

    /**
     * @brief 创建平台特定的 EventPoll 实现
     */
    static std::unique_ptr<EventPoll> create();
};

} // namespace falcon::net
```

#### 2.2 DownloadEngine 增强

```cpp
// packages/libfalcon/include/falcon/download_engine_v2.hpp

#pragma once

#include <falcon/commands/command.hpp>
#include <falcon/net/event_poll.hpp>
#include <falcon/task_manager.hpp>
#include <deque>
#include <memory>

namespace falcon {

/**
 * @brief 增强版下载引擎 - aria2 风格
 *
 * 核心改进：
 * 1. 事件驱动的命令执行循环
 * 2. Socket 连接池管理
 * 3. I/O 多路复用支持
 * 4. 例程命令（后台任务）
 */
class DownloadEngineV2 {
public:
    DownloadEngineV2(const EngineConfig& config);
    ~DownloadEngineV2();

    /**
     * @brief 启动事件循环（阻塞直到所有任务完成）
     */
    void run();

    /**
     * @brief 添加命令到执行队列
     */
    void add_command(std::unique_ptr<Command> command);

    /**
     * @brief 添加例程命令（周期性执行的后台任务）
     */
    void add_routine_command(std::unique_ptr<Command> command);

    /**
     * @brief 获取 EventPoll 实例
     */
    net::EventPoll* event_poll() noexcept { return event_poll_.get(); }

    /**
     * @brief 获取任务管理器
     */
    TaskManager* task_manager() noexcept { return task_manager_.get(); }

    /**
     * @brief 请求关闭（优雅关闭）
     */
    void shutdown() { halt_requested_ = 1; }

    /**
     * @brief 强制关闭
     */
    void force_shutdown() { halt_requested_ = 2; }

private:
    void execute_commands();
    void execute_routine_commands();
    bool is_shutdown_requested() const;

    std::unique_ptr<net::EventPoll> event_poll_;
    std::unique_ptr<TaskManager> task_manager_;

    std::deque<std::unique_ptr<Command>> command_queue_;
    std::deque<std::unique_ptr<Command>> routine_commands_;

    std::atomic<int> halt_requested_{0};
};

} // namespace falcon
```

**文件清单**:
- `packages/libfalcon/include/falcon/net/event_poll.hpp`
- `packages/libfalcon/src/net/event_poll_epoll.cpp` (Linux)
- `packages/libfalcon/src/net/event_poll_kqueue.cpp` (macOS/BSD)
- `packages/libfalcon/src/net/event_poll_wsa.cpp` (Windows)
- `packages/libfalcon/include/falcon/download_engine_v2.hpp`
- `packages/libfalcon/src/download_engine_v2.cpp`

---

### Phase 3: RequestGroup 管理

**目标**: 实现类似 aria2 的 RequestGroup 生命周期管理

#### 3.1 RequestGroup 状态机

```cpp
// packages/libfalcon/include/falcon/request_group.hpp

#pragma once

#include <falcon/types.hpp>
#include <falcon/download_options.hpp>
#include <falcon/segment_downloader.hpp>
#include <falcon/file_info.hpp>
#include <memory>
#include <vector>

namespace falcon {

/**
 * @brief 请求组状态 - 对应 aria2 的 RequestGroup 状态
 */
enum class RequestGroupStatus {
    WAITING,     // 等待执行（在队列中）
    ACTIVE,      // 正在下载
    PAUSED,      // 已暂停
    COMPLETED,   // 已完成
    ERROR,       // 错误
    REMOVED      // 已移除
};

/**
 * @brief 请求组 - 表示一个下载任务
 *
 * 对应 aria2 的 RequestGroup 类
 * 一个 RequestGroup 可以包含多个文件（如 BitTorrent）
 */
class RequestGroup {
public:
    RequestGroup(TaskId id,
                 const std::vector<std::string>& uris,
                 const DownloadOptions& options);

    /**
     * @brief 初始化下载
     */
    bool init();

    /**
     * @brief 创建初始命令
     */
    std::unique_ptr<Command> create_initial_command();

    /**
     * @brief 获取/设置状态
     */
    RequestGroupStatus status() const noexcept { return status_; }
    void set_status(RequestGroupStatus status) { status_ = status; }

    /**
     * @brief 获取任务 ID
     */
    TaskId id() const noexcept { return id_; }

    /**
     * @brief 获取文件列表
     */
    const std::vector<FileInfo>& files() const { return files_; }

    /**
     * @brief 获取分段下载器
     */
    SegmentDownloader* segment_downloader() { return segment_downloader_.get(); }

    /**
     * @brief 下载进度
     */
    struct Progress {
        Bytes downloaded = 0;
        Bytes total = 0;
        double progress = 0.0;
        Speed speed = 0;
    };
    Progress get_progress() const;

private:
    TaskId id_;
    RequestGroupStatus status_ = RequestGroupStatus::WAITING;
    std::vector<std::string> uris_;
    DownloadOptions options_;
    std::vector<FileInfo> files_;
    std::unique_ptr<SegmentDownloader> segment_downloader_;
};

} // namespace falcon
```

#### 3.2 RequestGroupMan 增强

```cpp
// packages/libfalcon/include/falcon/request_group_manager.hpp

#pragma once

#include <falcon/request_group.hpp>
#include <falcon/types.hpp>
#include <deque>
#include <memory>
#include <vector>

namespace falcon {

/**
 * @brief 请求组管理器 - aria2 风格
 *
 * 管理多个 RequestGroup 的生命周期
 */
class RequestGroupMan {
public:
    explicit RequestGroupMan(std::size_t max_concurrent = 5);

    /**
     * @brief 添加请求组
     */
    void add_request_group(std::unique_ptr<RequestGroup> group);

    /**
     * @brief 从保留队列激活请求组
     */
    void fill_request_group_from_reserver(DownloadEngineV2* engine);

    /**
     * @brief 暂停请求组
     */
    bool pause_group(TaskId id);

    /**
     * @brief 恢复请求组
     */
    bool resume_group(TaskId id);

    /**
     * @brief 移除请求组
     */
    bool remove_group(TaskId id);

    /**
     * @brief 获取请求组
     */
    RequestGroup* find_group(TaskId id);

    /**
     * @brief 获取活动组数量
     */
    std::size_t active_count() const { return request_groups_.size(); }

    /**
     * @brief 获取等待组数量
     */
    std::size_t waiting_count() const { return reserved_groups_.size(); }

    /**
     * @brief 检查是否所有组已完成
     */
    bool all_completed() const {
        return request_groups_.empty() && reserved_groups_.empty();
    }

private:
    std::size_t max_concurrent_;
    std::vector<std::unique_ptr<RequestGroup>> request_groups_;     // 活动组
    std::deque<std::unique_ptr<RequestGroup>> reserved_groups_;    // 等待队列
};

} // namespace falcon
```

**文件清单**:
- `packages/libfalcon/include/falcon/request_group.hpp`
- `packages/libfalcon/src/request_group.cpp`
- `packages/libfalcon/include/falcon/request_group_manager.hpp`
- `packages/libfalcon/src/request_group_manager.cpp`

---

### Phase 4: Socket 连接池

**目标**: 实现 aria2 风格的连接复用

```cpp
// packages/libfalcon/include/falcon/net/socket_pool.hpp

#pragma once

#include <falcon/types.hpp>
#include <string>
#include <memory>
#include <map>
#include <chrono>

namespace falcon::net {

/**
 * @brief Socket 连接信息
 */
struct SocketKey {
    std::string host;
    uint16_t port;
    std::string username;
    std::string proxy;

    bool operator<(const SocketKey& other) const {
        return std::tie(host, port, username, proxy) <
               std::tie(other.host, other.port, other.username, other.proxy);
    }
};

/**
 * @brief 池化的 Socket 连接
 */
class PooledSocket {
public:
    PooledSocket(int fd, const SocketKey& key);

    int fd() const noexcept { return fd_; }
    const SocketKey& key() const { return key_; }

    /**
     * @brief 检查连接是否仍然有效
     */
    bool is_valid() const;

    /**
     * @brief 获取空闲时间
     */
    std::chrono::seconds idle_time() const;

private:
    int fd_;
    SocketKey key_;
    std::chrono::steady_clock::time_point last_used_;
};

/**
 * @brief Socket 连接池 - aria2 风格
 *
 * 复用已建立的连接以提高性能
 */
class SocketPool {
public:
    SocketPool(std::chrono::seconds timeout = std::chrono::seconds(30));

    /**
     * @brief 获取或创建连接
     */
    std::shared_ptr<PooledSocket> acquire(const SocketKey& key);

    /**
     * @brief 归还连接到池中
     */
    void release(std::shared_ptr<PooledSocket> socket);

    /**
     * @brief 清理过期连接
     */
    void cleanup_expired();

    /**
     * @brief 清空所有连接
     */
    void clear();

private:
    std::chrono::seconds timeout_;
    std::map<SocketKey, std::shared_ptr<PooledSocket>> pool_;
};

} // namespace falcon::net
```

**文件清单**:
- `packages/libfalcon/include/falcon/net/socket_pool.hpp`
- `packages/libfalcon/src/net/socket_pool.cpp`
- `packages/libfalcon/src/commands/evict_socket_pool_command.cpp` (清理命令)

---

### Phase 5: 命令行参数扩展

**目标**: 添加 aria2 兼容的命令行选项

```cpp
// aria2 兼容性参数映射表

/*
 * aria2c 参数          Falcon 参数              状态
 * ==================================================
 * 基础选项:
 * -h, --help          -h, --help              ✅ 已实现
 * -V, --version       -V, --version           ✅ 已实现
 * -o, --out           -o, --output            ✅ 已实现
 * -d, --dir           -d, --directory         ✅ 已实现
 *
 * 下载队列:
 * -j, --max-concurrent-downloads  -j         ✅ 已实现
 * --enable-rpc        --enable-rpc            ❌ 待实现
 * --rpc-listen-port   --rpc-port              ❌ 待实现
 *
 * HTTP/FTP 选项:
 * -x, --max-connection-per-server  -c         ✅ 已实现
 * -s, --split         -s, --split             ✅ 已实现
 * -k, --min-split-size  -k, --min-split-size  ✅ 已实现
 * --min-turtle-speed  --min-speed             ❌ 待实现
 * --max-overall-download-limit  --limit       ✅ 已实现
 * --max-download-limit  --limit               ✅ 已实现
 *
 * 连接选项:
 * --connect-timeout   --connect-timeout       ❌ 待实现
 * --timeout           -t, --timeout           ✅ 已实现
 * --max-tries         -r, --retry             ✅ 已实现
 * --retry-wait        --retry-wait            ❌ 待实现
 *
 * 代理选项:
 * --all-proxy         --proxy                 🔄 基础支持
 * --http-proxy        --http-proxy            ❌ 待实现
 * --https-proxy       --https-proxy           ❌ 待实现
 * --ftp-proxy         --ftp-proxy             ❌ 待实现
 * --no-proxy          --no-proxy              ❌ 待实现
 *
 * HTTP 选项:
 * -U, --user-agent    -U, --user-agent        ✅ 已实现
 * -H, --header        -H, --header            ✅ 已实现
 * --load-cookies      --load-cookies          ❌ 待实现
 * --save-cookies      --save-cookies          ❌ 待实现
 * --referer           --referer               ❌ 待实现
 *
 * 文件选项:
 * -c, --continue      (默认启用)              ✅ 已实现
 * --no-continue       --no-continue           ✅ 已实现
 * --file-allocation   --file-allocation       ❌ 待实现
 * --auto-file-renaming  --auto-rename         ❌ 待实现
 *
 * 校验选项:
 * --check-integrity   --verify                ❌ 待实现
 * --checksum          --checksum              ❌ 待实现
 *
 * BitTorrent 选项:
 * --seed-time         --bt-seed-time          ❌ 待实现
 * --seed-ratio        --bt-seed-ratio         ❌ 待实现
 * --peer-id-prefix    --bt-peer-prefix        ❌ 待实现
 *
 * 高级选项:
 * --summary-interval  --summary-interval      ❌ 待实现
 * --conf-path         --config                ❌ 待实现
 */
```

**新增参数实现**:

```cpp
// packages/falcon-cli/src/aria2_compat_options.cpp

// 新增 aria2 兼容参数解析
else if (arg == "--connect-timeout") {
    if (i + 1 < argc) {
        args.connect_timeout = std::stoi(argv[++i]);
    }
}
else if (arg == "--retry-wait") {
    if (i + 1 < argc) {
        args.retry_wait = std::stoi(argv[++i]);
    }
}
else if (arg == "--http-proxy") {
    if (i + 1 < argc) {
        args.http_proxy = argv[++i];
    }
}
else if (arg == "--https-proxy") {
    if (i + 1 < argc) {
        args.https_proxy = argv[++i];
    }
}
else if (arg == "--load-cookies") {
    if (i + 1 < argc) {
        args.cookie_file = argv[++i];
    }
}
else if (arg == "--save-cookies") {
    if (i + 1 < argc) {
        args.cookie_save_file = argv[++i];
    }
}
else if (arg == "--referer") {
    if (i + 1 < argc) {
        args.referer = argv[++i];
    }
}
else if (arg == "--file-allocation") {
    if (i + 1 < argc) {
        std::string method = argv[++i];
        if (method == "none") args.file_allocation = FileAllocation::None;
        else if (method == "prealloc") args.file_allocation = FileAllocation::Prealloc;
        else if (method == "falloc") args.file_allocation = FileAllocation::Falloc;
        else if (method == "trunc") args.file_allocation = FileAllocation::Trunc;
    }
}
else if (arg == "--checksum") {
    if (i + 1 < argc) {
        // 格式: SHA-1=hexdigest, MD5=hexdigest
        std::string checksum = argv[++i];
        auto eq_pos = checksum.find('=');
        if (eq_pos != std::string::npos) {
            args.checksum_type = checksum.substr(0, eq_pos);
            args.checksum_value = checksum.substr(eq_pos + 1);
        }
    }
}
```

---

### Phase 6: 文件校验系统

**目标**: 实现 SHA-1/MD5 文件完整性校验

```cpp
// packages/libfalcon/include/falcon/checksum.hpp

#pragma once

#include <falcon/types.hpp>
#include <string>
#include <memory>

namespace falcon {

/**
 * @brief 校验和类型
 */
enum class ChecksumType {
    SHA1,
    SHA256,
    MD5,
    ADLER32
};

/**
 * @brief 校验和计算器
 */
class ChecksumCalculator {
public:
    static std::unique_ptr<ChecksumCalculator> create(ChecksumType type);

    virtual ~ChecksumCalculator() = default;

    /**
     * @brief 更新数据
     */
    virtual void update(const char* data, std::size_t size) = 0;

    /**
     * @brief 获取最终校验和（十六进制字符串）
     */
    virtual std::string finalize() = 0;

    /**
     * @brief 重置状态
     */
    virtual void reset() = 0;
};

/**
 * @brief 验证文件校验和
 */
bool verify_checksum(const std::string& file_path,
                     ChecksumType type,
                     const std::string& expected_hash);

} // namespace falcon
```

**文件清单**:
- `packages/libfalcon/include/falcon/checksum.hpp`
- `packages/libfalcon/src/checksum_sha1.cpp` (使用 OpenSSL)
- `packages/libfalcon/src/checksum_sha256.cpp`
- `packages/libfalcon/src/checksum_md5.cpp`
- `packages/libfalcon/src/checksum_verify.cpp`

---

### Phase 7: Cookie 管理支持

**目标**: 实现 Cookie 加载和保存

```cpp
// packages/libfalcon/include/falcon/http/cookie_manager.hpp

#pragma once

#include <falcon/types.hpp>
#include <string>
#include <map>

namespace falcon::http {

/**
 * @brief Cookie 信息
 */
struct Cookie {
    std::string name;
    std::string value;
    std::string domain;
    std::string path;
    std::time_t expiry = 0;
    bool secure = false;
    bool http_only = false;
};

/**
 * @brief Cookie 管理器
 *
 * 支持 Netscape Cookie 格式
 */
class CookieManager {
public:
    /**
     * @brief 从文件加载 Cookie (Netscape 格式)
     */
    bool load_from_file(const std::string& path);

    /**
     * @brief 保存 Cookie 到文件
     */
    bool save_to_file(const std::string& path) const;

    /**
     * @brief 获取指定域名/路径的 Cookie
     */
    std::string get_cookies(const std::string& domain,
                            const std::string& path) const;

    /**
     * @brief 添加 Cookie
     */
    void add_cookie(const Cookie& cookie);

    /**
     * @brief 清理过期 Cookie
     */
    void cleanup_expired();

private:
    std::multimap<std::string, Cookie> cookies_;  // key: domain
};

/**
 * @brief 为 HTTP 请求添加 Cookie 头
 */
void add_cookies_to_request(HttpRequest& request,
                             const CookieManager& manager,
                             const std::string& host,
                             const std::string& path);

} // namespace falcon::http
```

**Netscape Cookie 格式示例**:
```
# Netscape HTTP Cookie File
# This file is generated by aria2c

.domain\ttrue\tfalse\t2147483647\tname\tvalue
.example.com\ttrue\tfalse\t2147483647\tsession\tabc123
```

---

### Phase 8: RPC 接口框架

**目标**: 实现基础 JSON-RPC 支持

```cpp
// packages/libfalcon/include/falcon/rpc/rpc_server.hpp

#pragma once

#include <falcon/download_engine.hpp>
#include <memory>
#include <string>

namespace falcon::rpc {

/**
 * @brief RPC 服务器接口
 */
class RpcServer {
public:
    virtual ~RpcServer() = default;

    /**
     * @brief 启动 RPC 服务器
     */
    virtual bool start() = 0;

    /**
     * @brief 停止 RPC 服务器
     */
    virtual void stop() = 0;

    /**
     * @brief 等待服务器结束
     */
    virtual void wait() = 0;

protected:
    DownloadEngine* engine_;
};

/**
 * @brief JSON-RPC 服务器
 */
class JsonRpcServer : public RpcServer {
public:
    JsonRpcServer(DownloadEngine* engine,
                  const std::string& host = "127.0.0.1",
                  uint16_t port = 6800);

    bool start() override;
    void stop() override;
    void wait() override;

    /**
     * @brief 设置 RPC 密码令牌
     */
    void set_secret_token(const std::string& token) {
        secret_token_ = token;
    }

private:
    std::string host_;
    uint16_t port_;
    std::string secret_token_;
    void* server_;  // HTTP 服务器句柄（实现细节）
};

/**
 * @brief RPC 方法处理器
 */
class RpcMethod {
public:
    virtual ~RpcMethod() = default;

    virtual std::string execute(const std::string& params) = 0;
    virtual std::string name() const = 0;
};

/**
 * @brief aria2 兼容的 RPC 方法
 */
class aria2_addUri : public RpcMethod {
public:
    aria2_addUri(DownloadEngine* engine) : engine_(engine) {}

    std::string execute(const std::string& params) override;
    std::string name() const override { return "aria2.addUri"; }

private:
    DownloadEngine* engine_;
};

class aria2_getGlobalOption : public RpcMethod {
public:
    std::string execute(const std::string& params) override;
    std::string name() const override { return "aria2.getGlobalOption"; }
};

// 更多 RPC 方法...

} // namespace falcon::rpc
```

**核心 RPC 方法**:
1. `aria2.addUri` - 添加下载任务
2. `aria2.remove` - 移除任务
3. `aria2.pause` - 暂停任务
4. `aria2.unpause` - 恢复任务
5. `aria2.tellStatus` - 获取任务状态
6. `aria2.getGlobalOption` - 获取全局选项
7. `aria2.changeGlobalOption` - 修改全局选项
8. `aria2.getGlobalStat` - 获取统计信息
9. `aria2.purgeDownloadResult` - 清理下载记录
10. `aria2.getVersion` - 获取版本信息

---

## 三、实施计划时间表

### Sprint 1: 基础命令系统 (2 周)

- [ ] Command 基类设计
- [ ] HttpInitiateConnectionCommand
- [ ] HttpResponseCommand
- [ ] HttpDownloadCommand
- [ ] 单元测试

### Sprint 2: 事件驱动引擎 (2 周)

- [ ] EventPoll 接口与实现
  - [ ] epoll (Linux)
  - [ ] kqueue (macOS/BSD)
  - [ ] WSAPoll (Windows)
- [ ] DownloadEngineV2
- [ ] 命令队列执行
- [ ] 集成测试

### Sprint 3: RequestGroup 管理 (1.5 周)

- [ ] RequestGroup 类
- [ ] RequestGroupMan 增强
- [ ] 状态机实现
- [ ] 测试

### Sprint 4: Socket 连接池 (1 周)

- [ ] SocketPool 实现
- [ ] EvictSocketPoolCommand
- [ ] 连接复用测试

### Sprint 5: 命令行参数扩展 (1.5 周)

- [ ] aria2 兼容参数解析
- [ ] Cookie 文件支持
- [ ] Referer 支持
- [ ] 帮助文档更新

### Sprint 6: 文件校验 (1 周)

- [ ] SHA-1/SHA256/MD5 实现
- [ ] ChecksumCalculator
- [ ] 校验集成

### Sprint 7: RPC 接口 (3 周)

- [ ] JSON-RPC 服务器框架
- [ ] 核心 RPC 方法实现
- [ ] Token 认证
- [ ] RPC 测试

### Sprint 8: 测试与优化 (2 周)

- [ ] 压力测试
- [ ] 性能优化
- [ ] 文档完善

---

## 四、测试策略

### 4.1 单元测试

```cpp
// packages/libfalcon/tests/commands/command_test.cpp

TEST(CommandTest, HttpInitiateConnection) {
    // 测试 HTTP 连接初始化命令
}

TEST(CommandTest, CommandQueue) {
    // 测试命令队列执行
}

TEST(EventPollTest, EpollCreate) {
    // 测试 epoll 创建
}

TEST(SocketPoolTest, ConnectionReuse) {
    // 测试连接复用
}

TEST(RequestGroupTest, StateTransition) {
    // 测试状态转换
}

TEST(ChecksumTest, SHA1Calculation) {
    // 测试 SHA-1 计算
}
```

### 4.2 集成测试

```bash
# 与 aria2c 行为对比测试

# HTTP 下载
aria2c https://example.com/file.zip -x 8 -s 8
falcon-cli https://example.com/file.zip -c 8

# 断点续传
aria2c https://example.com/file.zip -c --continue
falcon-cli https://example.com/file.zip --continue

# 限速
aria2c https://example.com/file.zip --max-download-limit=1M
falcon-cli https://example.com/file.zip --limit 1M
```

### 4.3 性能基准测试

| 测试项 | aria2c | Falcon (当前) | Falcon (目标) |
|--------|--------|--------------|--------------|
| 单文件下载速度 | 100% | 90% | 100%+ |
| 并发 8 连接 | 100% | 85% | 100%+ |
| 内存占用 | 100% | 80% | 80% |
| 启动时间 | 100% | 120% | 100% |

---

## 五、参考资源

### aria2c 源码参考

- [aria2/aria2 GitHub](https://github.com/aria2/aria2)
- [aria2 API 文档](https://aria2.github.io/manual/en/html/)
- [libaria2 接口](https://aria2.github.io/manual/en/html/libaria2.html)

### 关键源文件

| aria2 文件 | Falcon 对应文件 | 说明 |
|-----------|----------------|------|
| `src/DownloadEngine.h/cc` | `download_engine_v2.hpp/cpp` | 下载引擎 |
| `src/RequestGroup.h/cc` | `request_group.hpp/cpp` | 请求组 |
| `src/RequestGroupMan.h/cc` | `request_group_manager.hpp/cpp` | 请求组管理 |
| `src/SegmentMan.h/cc` | `segment_downloader.hpp/cpp` | 分段管理 |
| `src/Command.h` | `commands/command.hpp` | 命令基类 |
| `src/AbstractCommand.h/cc` | `commands/abstract_command.hpp/cpp` | 抽象命令 |
| `src/HttpResponseCommand.cc` | `commands/http_commands.cpp` | HTTP 响应命令 |
| `src/SocketCore.h/cc` | `net/socket.hpp/cpp` | Socket 封装 |
| `src/Option.h/cc` | `download_options.hpp/cpp` | 选项处理 |
| `src/RpcMethod.h/cc` | `rpc/rpc_server.hpp/cpp` | RPC 方法 |

### 设计模式参考

1. **Command Pattern**: 命令模式用于下载操作
2. **State Pattern**: 状态模式用于 RequestGroup 状态管理
3. **Strategy Pattern**: 策略模式用于不同协议处理
4. **Observer Pattern**: 观察者模式用于事件监听

---

## 六、风险评估

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 事件循环性能问题 | 高 | 充分的性能测试，必要时优化 |
| Socket 连接池泄漏 | 中 | 实现超时清理机制 |
| 跨平台兼容性 | 中 | 使用抽象层，多平台测试 |
| RPC 安全问题 | 高 | Token 认证 + TLS 支持 |
| Cookie 格式兼容性 | 低 | 严格遵循 Netscape 格式 |

---

## 七、完成标准

### 功能完成度

- [ ] 所有核心命令实现
- [ ] EventPoll 全平台支持
- [ ] Socket 连接池工作正常
- [ ] RequestGroup 生命周期完整
- [ ] 文件校验功能通过测试
- [ ] Cookie 管理符合规范
- [ ] RPC 方法与 aria2 兼容

### 性能指标

- [ ] 下载速度 ≥ aria2c 的 95%
- [ ] 内存占用 ≤ aria2c 的 90%
- [ ] 支持 100+ 并发任务
- [ ] 响应时间 < 100ms (RPC)

### 测试覆盖

- [ ] 单元测试覆盖率 ≥ 70%
- [ ] 集成测试全部通过
- [ ] 压力测试稳定运行 24 小时

---

**文档维护**: 每完成一个 Phase，更新对应的状态标记。

**最后更新**: 2025-12-24
