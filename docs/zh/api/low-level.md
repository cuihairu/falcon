[English](/api/low-level) | **简体中文**

# 底层工具 API

公开 C++ API 以 `DownloadEngine` 和 `add_task()` 为中心（见[核心 API](/zh/api/core)）。少量底层工具保留用于完整性校验和自定义协议处理器；普通使用者无需接触。随单引擎收口，旧的 socket 连接池与事件轮询工具已移除，不再属于公开 API。

> 头文件位于 `libs/protocols/include/falcon/`。除非在写协议处理器或集成测试，否则优先使用引擎 API。

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

## 镜像回退

Metalink 文档可为同一内容列出多个镜像。单一 V1（libcurl）引擎按串行顺序尝试镜像，并在发布前校验文件。这是镜像回退，不是多源分段下载：段级换源、If-Range 与超时清扫当前均未启用。P2SP 同样只做发现，不注入下载；见 [P2SP 共享网络](/zh/developer/p2sp-network)。

## 性能建议

- `options.max_connections` 与带宽匹配：慢速链路 4，快速链路 8–16。
- libcurl 在处理器支持时复用 HTTP 连接，无需单独配置连接池。
- 提高 `options.min_segment_size`，避免小文件被无意义切分。
- 用 `options.speed_limit`（单任务）或 `daemon.json` / RPC 的 `max-overall-download-limit`（全局，仅 daemon）避免占满带宽。

## 另见

- [核心 API](/zh/api/core) — 引擎、任务、选项与状态类型
- [事件 API](/zh/api/events) — 监听器与进度上报
- [插件 API](/zh/api/plugins) — 编写协议处理器
- [P2SP 共享网络](/zh/developer/p2sp-network) — 节点发现与来源共享
