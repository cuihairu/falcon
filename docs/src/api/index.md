# API Reference

Falcon currently exposes a C++ API; the CLI is built on the same core library capabilities.

## Header entry point

```cpp
#include <falcon/falcon.hpp>
```

`falcon.hpp` aggregates the following public interfaces:

- `download_engine.hpp`
- `download_options.hpp`
- `download_task.hpp`
- `event_listener.hpp`
- `protocol_handler.hpp`
- `types.hpp`
- `version.hpp`

## Quick start

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

Notes:

- Constructing `DownloadEngine` loads the built-in protocol handlers available in the current build.
- Add tasks with `add_task()` or `add_tasks()`, then start them with `start_task()`.
- Manage event callbacks with `add_listener()` and `remove_listener()`.

## Relationship to the CLI

Common CLI options map approximately to `DownloadOptions` fields as follows:

| CLI option | C++ field |
|------|------|
| `-o`, `--output` | `output_filename` |
| `-d`, `--directory` | `output_directory` |
| `-c`, `--connections` | `max_connections` |
| `--limit` | `speed_limit` |
| `--proxy` | `proxy` |
| `--retry-wait` | `retry_delay_seconds` |
| `--no-verify-ssl` | `verify_ssl = false` |

::: tip Detailed API
Continue with:
- [Core API](./core.md)
- [Plugin API](./plugins.md)
- [Events API](./events.md)
:::
