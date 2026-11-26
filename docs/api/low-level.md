[简体中文](/zh/api/low-level) | **English**

# Low-Level Utilities

The public C++ API is centred on `DownloadEngine` and `add_task()` (see [Core API](/api/core)). Some older guides also exposed lower-level building blocks directly. They are still present in the source tree and are documented here for callers who build custom transfer pipelines; ordinary users do not need them.

> The headers live in `packages/libfalcon-protocols/include/falcon/`. Prefer the engine API unless you are writing a protocol handler or an integration test.

## File hashing

`FileHasher` computes and verifies whole-file hashes. It is used internally by the engine for integrity checks and Metalink publication.

```cpp
#include <falcon/file_hash.hpp>

using namespace falcon;

auto hash = FileHasher::calculate("file.zip", HashAlgorithm::SHA256);
auto result = FileHasher::verify(
    "downloaded_file.zip", expected_hex, HashAlgorithm::SHA256);
if (result.valid) {
    // integrity confirmed
} else {
    // result.expected / result.calculated describe the mismatch
}
```

- Available algorithms: MD5, SHA1, SHA256, SHA512.
- `FileHasher::detect_algorithm(hash_string)` infers the algorithm from digest length.
- `FileHasher::verify_multiple(path, vector<pair<string, HashAlgorithm>>)` verifies a file against several candidate digests.

## Socket pool

`SocketPool` keeps idle connections alive for reuse across requests, reducing handshake overhead for many small transfers.

```cpp
#include <falcon/net/socket_pool.hpp>

using namespace falcon::net;

SocketPool pool(std::chrono::seconds(30), 16);  // idle TTL, max idle sockets

SocketKey key;
key.host = "example.com";
key.port = 80;
key.username = "";   // optional HTTP basic auth
key.proxy = "";      // optional proxy address

auto socket = pool.acquire(key);
if (socket && socket->is_valid()) {
    // use socket->fd()
    pool.release(socket);
}
```

The pool is transparent to `DownloadEngine`: connection reuse happens automatically for HTTP-family handlers.

## Event poll

`EventPoll` is the platform-abstracted readiness loop (epoll / kqueue / IOCP) used by the V2 engine. It is mainly relevant when implementing a custom protocol handler.

```cpp
#include <falcon/net/event_poll.hpp>

using namespace falcon::net;

auto poll = EventPoll::create();  // picks the best backend for the platform

poll->add_event(fd, static_cast<int>(IOEvent::READ),
    [](int fd, int events, void* user_data) {
        char buf[4096];
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        // handle n
    });

poll->modify_event(fd, static_cast<int>(IOEvent::WRITE));
poll->remove_event(fd);

int ready = poll->poll(100);  // timeout in ms
```

## Mirror failover

A task can be created with several URLs pointing at the same content. The engine treats them as mirrors and moves to the next source when a transfer fails.

```cpp
std::vector<std::string> mirrors = {
    "http://mirror1.example.com/file.zip",
    "http://mirror2.example.com/file.zip",
    "http://backup.example.com/file.zip",
};
TaskId id = engine.add_download(mirrors, options);
```

Mirror selection is also the basis of the P2SP source switching described in [P2SP Sharing Network](/developer/p2sp-network).

## Performance notes

- Set `options.max_connections` to match bandwidth: 4 on slow links, 8–16 on fast ones.
- Keep connection reuse enabled (the pool handles it; no per-call setup needed).
- Raise `options.min_segment_size` so tiny files are not split wastefully.
- Use `options.speed_limit` (per task) or `daemon.json` / RPC `max-overall-download-limit` (global, daemon only) to avoid saturating a link.

## See also

- [Core API](/api/core) — engine, task, options and status types
- [Events API](/api/events) — listeners and progress reporting
- [Plugin API](/api/plugins) — writing a protocol handler
- [P2SP Sharing Network](/developer/p2sp-network) — node discovery and source sharing
