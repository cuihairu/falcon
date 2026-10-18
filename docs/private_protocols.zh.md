[English](private_protocols.md) | **中文**

# Falcon 私有协议支持

> [!NOTE]
> 本文档偏设计与背景说明；当前可用说明请优先查看 `docs/src/protocols/private.md`。

本文档介绍了 Falcon 下载器对各种私有下载协议的支持。

## 支持的协议列表

### 1. 迅雷协议 (Thunder)

**协议标识**: `thunder://` 和 `thunderxl://`

**说明**: 迅雷是国内广泛使用的下载工具，使用特殊的编码方式来分享下载链接。

**链接格式**:
- 经典格式: `thunder://[Base64编码的内容]`
- 离线格式: `thunderxl://[Base64编码的内容]`

**实现细节**:
- Base64解码
- 移除 AA/ZZ 标记（经典格式）
- 自动转换为原始 HTTP/FTP 链接

**示例**:
```cpp
// 内置协议在 DownloadEngine 构造时自动注册，无需手动创建插件
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

// 下载迅雷链接
std::string url = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg==";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 2. QQ旋风协议 (QQDL)

**协议标识**: `qqlink://` 和 `qqdl://`

**说明**: QQ旋风是腾讯推出的下载工具，支持通过特殊链接分享资源。

**链接格式**:
- GID格式: `qqlink://[GID]|[URL]`（竖线分隔，GID 后的第二个字段是目标 URL）
- Base64格式: `qqlink://[Base64编码的URL]`

**实现细节**:
- GID 前缀字段跳过后提取其后的 URL
- Base64 回退：解码结果须以 http/https/ftp 开头
- 支持多字段 `|` 分隔形态（取首个含 `://` 的字段）

**示例**:
```cpp
// 内置协议在 DownloadEngine 构造时自动注册
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

std::string url = "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 3. 快车协议 (FlashGet)

**协议标识**: `flashget://` 和 `fg://`

**说明**: 快车（FlashGet）是一款老牌的下载管理器。

**链接格式**:
- 完整格式: `flashget://[Base64载荷]&ref=[引用页面]`（`&ref=` 尾缀剥除，载荷解码后剥 `[FLASHGET]` 前缀）
- 短格式: `fg://[URL编码的URL]`（URL 编码，不是 Base64）
- Base64简写: `flashget://[Base64载荷]`（无 `&ref=` 尾缀同样支持）

**实现细节**:
- URL 解码 / Base64 解码按载荷形态自动分派
- `[FLASHGET]` 前缀剥除（位于解码后的载荷内部）
- 自动转换为标准 HTTP 下载

**示例**:
```cpp
// 内置协议在 DownloadEngine 构造时自动注册
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

std::string url = "flashget://W0ZMQVNIR0VUXWh0dHA6Ly9leGFtcGxlLmNvbS9maWxlLnppcA==";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 4. ED2K协议 (eDonkey2000)

**协议标识**: `ed2k://`

**说明**: ED2K是eDonkey2000网络使用的协议，也被称为电驴协议，用于P2P文件共享。

**链接格式**:
- 文件链接: `ed2k://file|[文件名]|[文件大小]|[MD4哈希]|/`（可带 `|h=AICH哈希` 段）
- 服务器链接 `ed2k://server|...` 当前不支持，解析会报错

**实现细节**:
- 文件链接字段解析（名称/大小/哈希，字段不足 4 个报错）
- AICH（高级完整性检验）哈希段识别
- 多源委托下载

**示例**:
```cpp
// 内置协议在 DownloadEngine 构造时自动注册
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

std::string url = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";
auto task = engine.add_task(url, options);
engine.start_task(task->id());
```

### 5. HLS/DASH流媒体协议

**协议标识**: URL 以 `.m3u8` 结尾时路由到 HLS 处理器（`.mpd` 尚未实现）

**说明**: HTTP Live Streaming (HLS) 是常用的流媒体协议；DASH 清单解析尚未实现。

**HLS格式**: `.m3u8` 播放列表文件

**实现细节**:
- 播放列表解析（EXTINF 时长信息）
- 媒体段按序下载，二进制拼接合并
- 暂不支持主播放列表（master playlist）择流与加密流

**示例**:
```cpp
// 内置协议在 DownloadEngine 构造时自动注册
falcon::DownloadEngine engine;
falcon::DownloadOptions options;

// 下载HLS流
std::string hlsUrl = "https://example.com/playlist.m3u8";
auto task = engine.add_task(hlsUrl, options);
engine.start_task(task->id());
```

## 编译配置

使用 CMake 配置选项来启用/禁用特定协议（五个私有协议默认均为 OFF）：

```bash
# 启用所有私有协议
cmake -B build \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_QQDL=ON \
  -DFALCON_ENABLE_FLASHGET=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON

# 仅启用需要的协议
cmake -B build \
  -DFALCON_ENABLE_THUNDER=ON \
  -DFALCON_ENABLE_ED2K=ON \
  -DFALCON_ENABLE_HLS=ON \
  -DFALCON_ENABLE_QQDL=OFF \
  -DFALCON_ENABLE_FLASHGET=OFF
```

## CLI使用示例

```bash
# 下载迅雷链接
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg=="

# 下载ED2K链接
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# 下载HLS流媒体
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# 批量下载（包含各种协议）
cat urls.txt
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS5yYXJaWg==
qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==
ed2k://file|test.zip|2048576|B2C3D4E5F6789012345678901234ABC|/
https://example.com/stream.m3u8

falcon-cli -i urls.txt
```

## API编程示例

```cpp
#include <falcon/falcon.hpp>

int main() {
    // 创建下载引擎（构造时自动注册全部内置协议处理器，含私有协议插件）
    falcon::DownloadEngine engine;

    // 查看支持的协议
    auto protocols = engine.get_supported_protocols();
    std::cout << "支持的协议: ";
    for (const auto& p : protocols) {
        std::cout << p << " ";
    }
    std::cout << std::endl;

    // 下载不同类型的链接
    falcon::DownloadOptions options;
    options.output_directory = "./downloads";

    std::vector<std::string> urls = {
        "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXBaWg==",
        "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/",
        "https://example.com/playlist.m3u8"
    };

    std::vector<falcon::DownloadTask::Ptr> tasks;

    for (const auto& url : urls) {
        if (engine.is_url_supported(url)) {
            auto task = engine.add_task(url, options);
            if (task) {
                engine.start_task(task->id());
                tasks.push_back(task);
                std::cout << "开始下载: " << url << std::endl;
            }
        } else {
            std::cout << "不支持的URL: " << url << std::endl;
        }
    }

    // 等待所有下载完成
    for (auto& task : tasks) {
        task->wait();
    }

    return 0;
}
```

## 注意事项

1. **安全性**: 私有协议链接可能包含任意URL，请确保来源可信
2. **法律合规**: 仅使用这些协议下载合法内容
3. **网络要求**: 某些协议可能需要特定的网络环境或代理
4. **性能**: ED2K等P2P协议的下载速度取决于网络中的源数量

## 测试

运行私有协议的单元测试：

```bash
# 编译测试
cmake -B build -DFALCON_BUILD_TESTS=ON
cmake --build build

# 运行特定协议测试（套件名：ThunderPluginTest / QQDLPluginTest /
# FlashGetPluginTest / ED2KPluginTest / HLSPluginTest）
ctest --test-dir build -R "ThunderPlugin"
ctest --test-dir build -R "ED2KPlugin"
ctest --test-dir build -R "HLSPlugin"

# 一次运行全部五个私有协议套件
ctest --test-dir build -R "ThunderPlugin|QQDLPlugin|FlashGetPlugin|ED2KPlugin|HLSPlugin"
```

## 未来计划

- [ ] 支持更多私有协议（如：网盘链接解析）
- [ ] 增强ED2K网络的搜索和源发现
- [ ] 支持HLS的DRM保护内容
- [ ] 添加协议自动检测功能
- [ ] 支持协议转换工具
