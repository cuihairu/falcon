# Falcon 私有协议支持

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
// 创建迅雷插件并注册
auto thunderPlugin = std::make_unique<ThunderPlugin>();
engine.registerPlugin(std::move(thunderPlugin));

// 下载迅雷链接
std::string url = "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg==";
auto task = engine.startDownload(url, options);
```

### 2. QQ旋风协议 (QQDL)

**协议标识**: `qqlink://` 和 `qqdl://`

**说明**: QQ旋风是腾讯推出的下载工具，支持通过特殊链接分享资源。

**链接格式**:
- GID格式: `qqlink://[GID]|[文件信息]`
- Base64格式: `qqlink://[Base64编码的URL]`

**实现细节**:
- 支持GID（群组ID）解析
- Base64解码支持
- 文件信息提取（文件名、大小、CID等）

**示例**:
```cpp
auto qqPlugin = std::make_unique<QQDLPlugin>();
engine.registerPlugin(std::move(qqPlugin));

std::string url = "qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==";
auto task = engine.startDownload(url, options);
```

### 3. 快车协议 (FlashGet)

**协议标识**: `flashget://` 和 `fg://`

**说明**: 快车（FlashGet）是一款老牌的下载管理器。

**链接格式**:
- 完整格式: `flashget://[URL]&ref=[引用页面]`
- 短格式: `fg://[URL]`
- Base64格式: `flashget://[Base64编码的URL]`

**实现细节**:
- URL解码支持
- 引用页面提取
- 自动转换为标准HTTP下载

**示例**:
```cpp
auto flashgetPlugin = std::make_unique<FlashGetPlugin>();
engine.registerPlugin(std::move(flashgetPlugin));

std::string url = "flashget://W10=";  // 示例链接
auto task = engine.startDownload(url, options);
```

### 4. ED2K协议 (eDonkey2000)

**协议标识**: `ed2k://`

**说明**: ED2K是eDonkey2000网络使用的协议，也被称为电驴协议，用于P2P文件共享。

**链接格式**:
- 文件链接: `ed2k://file|[文件名]|[文件大小]|[MD4哈希]|/`
- 服务器链接: `ed2k://server|[服务器地址]|[端口]|[服务器名称]/`

**实现细节**:
- MD4哈希验证
- 文件大小解析
- 源地址提取
- 支持AICH（高级完整性检验）哈希

**示例**:
```cpp
auto ed2kPlugin = std::make_unique<ED2KPlugin>();
engine.registerPlugin(std::move(ed2kPlugin));

std::string url = "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/";
auto task = engine.startDownload(url, options);
```

### 5. HLS/DASH流媒体协议

**协议标识**: 通过文件扩展名识别（`.m3u8`, `.mpd`）

**说明**: HTTP Live Streaming (HLS) 和 Dynamic Adaptive Streaming over HTTP (DASH) 是常用的流媒体协议。

**HLS格式**: `.m3u8` 播放列表文件
**DASH格式**: `.mpd` 媒体呈现描述文件

**实现细节**:
- 播放列表/清单解析
- 媒体段下载
- 自动质量选择
- 使用ffmpeg合并媒体段

**示例**:
```cpp
auto hlsPlugin = std::make_unique<HLSPlugin>();
engine.registerPlugin(std::move(hlsPlugin));

// 下载HLS流
std::string hlsUrl = "https://example.com/playlist.m3u8";
auto task1 = engine.startDownload(hlsUrl, options);

// 下载DASH流
std::string dashUrl = "https://example.com/manifest.mpd";
auto task2 = engine.startDownload(dashUrl, options);
```

## 编译配置

使用 CMake 配置选项来启用/禁用特定协议：

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
falcon-cli "thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsZS56aXAuWg=="

# 下载ED2K链接
falcon-cli "ed2k://file|example.zip|1048576|A1B2C3D4E5F6789012345678901234AB|/"

# 下载HLS流媒体
falcon-cli "https://example.com/playlist.m3u8" -o video.mp4

# 批量下载（包含各种协议）
cat urls.txt
thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vZmlsGUucmFyWg==
qqlink://aHR0cDovL2V4YW1wbGUuY29tL3ZpZGVvLm1wNA==
ed2k://file|test.zip|2048576|B2C3D4E5F6789012345678901234ABC|/
https://example.com/stream.m3u8

falcon-cli -i urls.txt
```

## API编程示例

```cpp
#include <falcon/falcon.hpp>
#include <falcon/thunder_plugin.hpp>
#include <falcon/ed2k_plugin.hpp>
#include <falcon/hls_plugin.hpp>

int main() {
    // 创建下载引擎
    falcon::DownloadEngine engine;

    // 注册私有协议插件
    engine.registerPlugin(std::make_unique<falcon::ThunderPlugin>());
    engine.registerPlugin(std::make_unique<falcon::ED2KPlugin>());
    engine.registerPlugin(std::make_unique<falcon::HLSPlugin>());

    // 检查支持的协议
    auto protocols = engine.listSupportedProtocols();
    std::cout << "支持的协议: ";
    for (const auto& p : protocols) {
        std::cout << p << " ";
    }
    std::cout << std::endl;

    // 下载不同类型的链接
    falcon::DownloadOptions options;
    options.output_directory = "./downloads";

    std::vector<std::string> urls = {
        "thunder://...",
        "ed2k://...",
        "https://example.com/playlist.m3u8"
    };

    std::vector<std::shared_ptr<falcon::DownloadTask>> tasks;

    for (const auto& url : urls) {
        if (engine.supportsUrl(url)) {
            auto task = engine.startDownload(url, options);
            tasks.push_back(task);
            std::cout << "开始下载: " << url << std::endl;
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

# 运行特定协议测试
ctest --test-dir build -R "thunder_plugin_test"
ctest --test-dir build -R "ed2k_plugin_test"
ctest --test-dir build -R "hls_plugin_test"

# 运行所有私有协议测试
ctest --test-dir build -R "private_protocol"
```

## 未来计划

- [ ] 支持更多私有协议（如：网盘链接解析）
- [ ] 增强ED2K网络的搜索和源发现
- [ ] 支持HLS的DRM保护内容
- [ ] 添加协议自动检测功能
- [ ] 支持协议转换工具