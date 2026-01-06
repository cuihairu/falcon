# 私有协议

Falcon 支持多种国内常用下载工具的私有协议，包括迅雷、QQ旋风和快车。

## 迅雷 (Thunder)

### 支持的格式

- `thunder://` - 经典迅雷链接
- `thunderxl://` - 迅雷离线链接

### 链接格式

```
thunder://BASE64编码的URL
thunderxl://BASE64编码的URL
```

### 编码方式

迅雷链接使用 Base64 编码，格式为：
- 经典：`AA` + 原始 URL + `ZZ`
- 离线：直接 Base64 编码

### 命令行使用

```bash
# 经典迅雷链接
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# 迅雷离线链接
falcon-cli "thunderxl://aHR0cHM6Ly9leGFtcGxlLmNvbS92aWRlby5tcDQ="
```

### C++ API

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("thunder");

    auto task = engine.startDownload(
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==",
        falcon::DownloadOptions{}
    );

    task->wait();
    return 0;
}
```

### 解码示例

```bash
# 原始 URL: http://example.com/file.zip
# 编码后: thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==

# 手动解码
echo "QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==" | base64 -d
# 输出: AAhttp://example.com/file.zipZZ
```

---

## QQ旋风 (QQDL)

### 支持的格式

- `qqlink://` - 标准链接
- `qqdl://` - 短格式

### 链接格式

```
qqlink://BASE64编码的URL
qqlink://GID|文件名
```

### 命令行使用

```bash
# 标准 QQ旋风链接
falcon-cli "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA=="

# 带 GID 的链接
falcon-cli "qqlink://1234567890ABCDEF|video.mp4"
```

### C++ API

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("qqdl");

    auto task = engine.startDownload(
        "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==",
        falcon::DownloadOptions{}
    );

    task->wait();
    return 0;
}
```

---

## 快车 (FlashGet)

### 支持的格式

- `flashget://` - 完整格式
- `fg://` - 短格式
- `[FLASHGET]` - 带标记格式

### 链接格式

```
flashget://[URL]&ref=引用页面
flashget://[FLASHGET]BASE64编码的URL
fg://BASE64编码的URL
```

### 命令行使用

```bash
# 完整格式（带引用）
falcon-cli "flashget://[http://example.com/file.zip]&ref=http://example.com"

# Base64 编码格式
falcon-cli "flashget://[FLASHGET]aHR0cDovL2V4YW1wbGUuY29tLw=="

# 短格式
falcon-cli "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw"
```

### C++ API

```cpp
#include <falcon/falcon.hpp>

int main() {
    falcon::DownloadEngine engine;
    engine.loadPlugin("flashget");

    auto task = engine.startDownload(
        "fg://aHR0cDovL2V4YW1wbGUuY29tL2ZpbGUuemlw",
        falcon::DownloadOptions{}
    );

    task->wait();
    return 0;
}
```

---

## 工具函数

### Python 解码示例

```python
import base64

def decode_thunder(url):
    """解码迅雷链接"""
    if url.startswith("thunder://"):
        encoded = url[10:]
        decoded = base64.b64decode(encoded).decode()
        if decoded.startswith("AA") and decoded.endswith("ZZ"):
            return decoded[2:-2]
        return decoded
    return url

def decode_qqdl(url):
    """解码 QQ旋风链接"""
    if url.startswith("qqlink://") or url.startswith("qqdl://"):
        prefix = "qqlink://" if url.startswith("qqlink://") else "qqdl://"
        content = url[len(prefix):]

        if "|" in content:
            # 带 GID 的格式
            return content  # GID 格式需要特殊处理

        encoded = content
        decoded = base64.b64decode(encoded).decode()
        return decoded
    return url

def decode_flashget(url):
    """解码快车链接"""
    if url.startswith("flashget://") or url.startswith("fg://"):
        if "[FLASHGET]" in url:
            encoded = url.split("[FLASHGET]")[1]
            decoded = base64.b64decode(encoded).decode()
            return decoded
        elif "[url=" in url:
            # 复杂格式
            import re
            match = re.search(r'\[url=(.*?)\]', url)
            if match:
                return match.group(1)
        elif url.startswith("fg://"):
            encoded = url[5:]
            decoded = base64.b64decode(encoded).decode()
            return decoded
    return url

# 使用示例
thunder_url = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="
print(decode_thunder(thunder_url))  # http://example.com/file.zip
```

### JavaScript 解码示例

```javascript
function decodeThunder(url) {
    if (url.startsWith("thunder://")) {
        const encoded = url.substring(10);
        const decoded = atob(encoded);
        if (decoded.startsWith("AA") && decoded.endsWith("ZZ")) {
            return decoded.substring(2, decoded.length - 2);
        }
        return decoded;
    }
    return url;
}

function decodeQqdl(url) {
    if (url.startsWith("qqlink://") || url.startsWith("qqdl://")) {
        const prefix = url.startsWith("qqlink://") ? "qqlink://" : "qqdl://";
        const content = url.substring(prefix.length);

        if (content.includes("|")) {
            return content; // GID 格式
        }

        const decoded = atob(content);
        return decoded;
    }
    return url;
}

function decodeFlashget(url) {
    if (url.startsWith("flashget://") || url.startsWith("fg://")) {
        if (url.includes("[FLASHGET]")) {
            const encoded = url.split("[FLASHGET]")[1];
            return atob(encoded);
        } else if (url.startsWith("fg://")) {
            const encoded = url.substring(5);
            return atob(encoded);
        }
    }
    return url;
}
```

## 协议对比

| 协议 | 格式 | 编码方式 | 引用追踪 |
|------|------|----------|----------|
| 迅雷 | `thunder://BASE64` | Base64 + AA/ZZ 包装 | ❌ |
| QQ旋风 | `qqlink://BASE64` | Base64 | ✅ (GID 格式) |
| 快车 | `flashget://[URL]&ref=` | Base64 或直接 URL | ✅ |

## 常见问题

### Q: 解码后 URL 无效

A: 检查：
- Base64 编码是否正确
- 是否包含 AA/ZZ 包装（迅雷）
- 原始 URL 是否有效

### Q: 下载失败

A: 可能的原因：
- 原始资源已失效
- 需要特殊的认证或 cookie
- 资源需要特定的下载器

### Q: 如何生成私有协议链接？

A: 使用编码函数：

```python
import base64

def encode_thunder(url):
    encoded = base64.b64encode(("AA" + url + "ZZ").encode()).decode()
    return f"thunder://{encoded}"

# 使用
print(encode_thunder("http://example.com/file.zip"))
# thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==
```

::: tip 注意
私有协议链接只是普通 URL 的编码形式，下载速度和质量取决于原始资源，而非协议本身。
:::
