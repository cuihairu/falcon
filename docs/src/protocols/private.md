# 私有协议

Falcon 代码库中包含迅雷、QQ 旋风、快车等私有协议插件实现，但这些插件在顶层 CMake 中默认是关闭的。

## 支持范围

| 协议 | 常见 scheme | 默认构建 |
|------|------|------|
| 迅雷 | `thunder://`、`thunderxl://` | 关闭 |
| QQ 旋风 | `qqlink://`、`qqdl://` | 关闭 |
| 快车 | `flashget://`、`fg://` | 关闭 |
| 电驴 | `ed2k://` | 关闭 |

如果你希望在当前构建中启用它们，需要在配置阶段显式打开对应开关：

```bash
cmake -B build -S . \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON
```

## 命令行示例

```bash
# 迅雷
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# QQ 旋风
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# 快车
falcon-cli "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw"

# ED2K
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"
```

## C++ API

对调用方来说，私有协议的入口仍然是统一的 `add_task()`：

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;

    auto task = engine.add_task(
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
    );

    if (!task) {
        return 1;
    }

    engine.start_task(task->id());
    task->wait();
    return 0;
}
```

## 编码说明

### 迅雷

- `thunder://` 常见格式是原始 URL 外包 `AA` / `ZZ` 后再做 Base64。
- `thunderxl://` 常见格式是直接对原始 URL 做 Base64。

### QQ 旋风

- 常见为 `qqlink://BASE64`
- 也可能出现 `qqlink://GID|文件名`

### 快车

- 常见为 `fg://BASE64`
- 也可能出现 `flashget://...`

## 注意事项

- 私有协议本质上通常只是对原始 URL 的包装，不会凭空提升源站带宽。
- 如果插件未编译启用，`DownloadEngine::is_url_supported()` 会返回 `false`。
- 私有协议站点常常还依赖 cookie、签名或时效 token，单纯解码出 URL 不一定足够。
