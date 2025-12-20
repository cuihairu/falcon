# Falcon 使用示例

本目录包含 `libfalcon` 核心库的使用示例代码。

---

## 示例列表

| 文件 | 说明 |
|------|------|
| `simple_download.cpp` | 基础下载示例（单文件） |
| `batch_download.cpp` | 批量下载示例 |
| `progress_listener.cpp` | 自定义进度监听器 |
| `plugin_custom.cpp` | 自定义协议插件 |

---

## 编译与运行

### 编译示例

```bash
cmake -B build -S . -DFALCON_BUILD_EXAMPLES=ON
cmake --build build
```

### 运行示例

```bash
# 简单下载
./build/bin/simple_download https://example.com/file.zip

# 批量下载
./build/bin/batch_download urls.txt

# 自定义监听器
./build/bin/progress_listener https://example.com/large_file.tar.gz
```

---

## 示例代码概览

### simple_download.cpp（待实现）

```cpp
#include <falcon/download_engine.hpp>
#include <iostream>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <URL>\n";
        return 1;
    }

    falcon::DownloadEngine engine;
    falcon::DownloadOptions options;
    options.output_directory = "./downloads";

    auto task = engine.start_download(argv[1], options);
    task->wait();

    if (task->status() == falcon::TaskStatus::Completed) {
        std::cout << "Download completed: " << task->output_path() << "\n";
        return 0;
    } else {
        std::cerr << "Download failed\n";
        return 1;
    }
}
```

---

**下一步**：实现完整的示例代码，配合核心库开发同步更新。
