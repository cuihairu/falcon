# API 参考

Falcon 当前主要提供 C++ API，CLI 则基于同一套核心库能力构建。

## 头文件入口

```cpp
#include <falcon/falcon.hpp>
```

`falcon.hpp` 会聚合以下公共接口：

- `download_engine.hpp`
- `download_options.hpp`
- `download_task.hpp`
- `event_listener.hpp`
- `protocol_handler.hpp`
- `types.hpp`
- `version.hpp`

## 快速开始

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";

    auto task = engine.add_task(
        "https://example.com/file.zip",
        options
    );

    if (task) {
        engine.start_task(task->id());
        task->wait();
    }

    return 0;
}
```

说明：

- `DownloadEngine` 构造时会加载当前构建中可用的内置协议处理器。
- 任务通过 `add_task()` 或 `add_tasks()` 加入，再通过 `start_task()` 启动。
- 事件回调通过 `add_listener()` / `remove_listener()` 管理。

## CLI 对应关系

CLI 常见参数与 `DownloadOptions` 大致对应如下：

| CLI 参数 | C++ 字段 |
|------|------|
| `-o`, `--output` | `output_filename` |
| `-d`, `--directory` | `output_directory` |
| `-c`, `--connections` | `max_connections` |
| `--limit` | `speed_limit` |
| `--proxy` | `proxy` |
| `--retry-wait` | `retry_delay_seconds` |
| `--no-verify-ssl` | `verify_ssl = false` |

::: tip 详细 API
请继续查看：
- [核心 API](./core.md)
- [插件 API](./plugins.md)
- [事件 API](./events.md)
:::
