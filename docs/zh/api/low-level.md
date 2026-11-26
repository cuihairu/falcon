[English](/api/low-level) | **简体中文**

# 底层工具 API

公开 C++ API 以 `DownloadEngine` 和 `add_task()` 为中心（见[核心 API](/zh/api/core)）。早期指南还直接暴露了一些底层构件；它们仍在源码树中，这里供需要自建传输管线或协议处理器的调用方参考。普通使用者无需接触。

> 头文件位于 `packages/libfalcon-protocols/include/falcon/`。除非在写协议处理器或集成测试，否则优先使用引擎 API。

## 文件哈希

`FileHasher` 计算并校验整文件哈希，引擎在完整性校验与 Metalink 发布时内部使用。

```cpp
#include <falcon/file_hash.hpp>

using namespace falcon;

auto hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA256);
auto result = FileHasher::verify(
    "downloaded_file.zip", expected_hex, HashAlgorithm::SHA256);
if (result.valid) {
    // 完整性通过
} else {
    // result.expected / result.calculated 描述不一致
}
```

- 支持算法：MD5、SHA1、SHA256、SHA512。
- `FileHasher::detect_algorithm(hash_string)` 按摘要长度推断算法。
- `FileHasher::verify_multiple(path, vector<pair<string, HashAlgorithm>>)` 用多个候选摘要校验同一文件。

## Socket 连接池

`SocketPool` 复用空闲连接，降低大量小传输的握手开销。

```cpp
#include <falcon/net/socket_pool.hpp>

using namespace falcon::net;

SocketPool pool(std::chrono::seconds(30), 16);  // 空闲 TTL、最大空闲数

SocketKey key;
key.host = "example.com";
key.port = 80;
key.username = "";   // 可选 HTTP 基本认证
key.proxy = "";      // 可选代理地址

auto socket = pool.acquire(key);
if (socket && socket->is_valid()) {
    // 使用 socket->fd()
    pool.release(socket);
}
```

该池对 `DownloadEngine` 透明：HTTP 系处理器自动复用连接。

## 事件轮询

`EventPoll` 是平台抽象的事件就绪循环（epoll / kqueue / IOCP），V2 引擎在其上构建。主要面向自定义协议处理器。

```cpp
#include <falcon/net/event_poll.hpp>

using namespace falcon::net;

auto poll = EventPoll::create();  // 自动选择平台最优后端

poll->add_event(fd, static_cast<int>(IOEvent::READ),
    [](int fd, int events, void* user_data) {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        // 处理 n
    });

poll->modify_event(fd, static_cast<int>(IOEvent::WRITE));
poll->remove_event(fd);

int ready = poll->poll(100);  // 超时毫秒
```

## 镜像故障切换

同一个内容可用多个 URL 创建任务，引擎把它们当作镜像，某个源失败时自动切换到下一个。

```cpp
std::vector<std::string> mirrors = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://backup.example.com/file.zip",
};
TaskId id = engine.add_download(mirrors, options);
```

镜像选择也是 [P2SP 共享网络](/zh/developer/p2sp-network)中源切换的基础。

## 性能建议

- `options.max_connections` 与带宽匹配：慢速链路 4，快速链路 8–16。
- 保持连接复用开启（连接池自动处理，无需额外配置）。
- 提高 `options.min_segment_size`，避免小文件被无意义切分。
- 用 `options.speed_limit`（单任务）或 `daemon.json` / RPC 的 `max-overall-download-limit`（全局，仅 daemon）避免占满带宽。

## 另见

- [核心 API](/zh/api/core) — 引擎、任务、选项与状态类型
- [事件 API](/zh/api/events) — 监听器与进度上报
- [插件 API](/zh/api/plugins) — 编写协议处理器
- [P2SP 共享网络](/zh/developer/p2sp-network) — 节点发现与来源共享
