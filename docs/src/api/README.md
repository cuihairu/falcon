# API 参考

Falcon 提供 C++ API 和 CLI 工具两种使用方式。

## C++ API

### 核心头文件

```cpp
#include <falcon/falcon.hpp>           // 主头文件
#include <falcon/download_engine.hpp>  // 下载引擎
#include <falcon/types.hpp>            // 类型定义
#include <falcon/events.hpp>           // 事件系统
```

### 快速开始

```cpp
#include <falcon/falcon.hpp>

int main() {
    // 创建下载引擎
    falcon::DownloadEngine engine;

    // 加载所有插件
    engine.loadAllPlugins();

    // 配置选项
    falcon::DownloadOptions options;
    options.max_connections = 5;
    options.output_directory = "./downloads";

    // 开始下载
    auto task = engine.startDownload(
        "https://example.com/file.zip",
        options
    );

    // 等待完成
    task->wait();

    return 0;
}
```

## CLI 工具

### 基本语法

```bash
falcon-cli [OPTIONS] URL
```

### 选项

| 选项 | 说明 |
|------|------|
| `-o, --output <file>` | 指定输出文件名 |
| `-d, --directory <dir>` | 指定保存目录 |
| `-c, --connections <n>` | 连接数（多线程下载） |
| `-l, --limit <speed>` | 速度限制（如 1M） |
| `--continue` | 断点续传 |
| `--proxy <url>` | 代理服务器 |
| `-v, --verbose` | 详细输出 |
| `-h, --help` | 显示帮助 |
| `-V, --version` | 显示版本 |

::: tip 详细 API
请查看以下文档：
- [核心 API](./core.md)
- [插件 API](./plugins.md)
- [事件 API](./events.md)
:::
