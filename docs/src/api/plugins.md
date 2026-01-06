# 插件 API

本文档介绍 Falcon 的插件开发 API。

## IProtocolHandler

所有协议插件必须实现的基础接口。

### 接口定义

```cpp
namespace falcon {

class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    /// 协议名称（如 "http", "ftp"）
    virtual std::string protocol() const = 0;

    /// 插件显示名称
    virtual std::string name() const { return protocol(); }

    /// 插件版本
    virtual std::string version() const { return "1.0.0"; }

    /// 检查是否可以处理该 URL
    virtual bool canHandle(const std::string& url) const = 0;

    /// 开始下载
    virtual std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener
    ) = 0;

    /// 支持的文件扩展名（可选）
    virtual std::vector<std::string> supportedExtensions() const {
        return {};
    }

    /// 插件初始化（可选）
    virtual bool initialize() { return true; }

    /// 插件清理（可选）
    virtual void cleanup() {}
};

}
```

## PluginManager

插件管理器，负责加载和管理协议插件。

### 类定义

```cpp
namespace falcon {

class PluginManager {
public:
    // 注册插件
    void registerPlugin(std::unique_ptr<IProtocolHandler> plugin);

    // 获取协议处理器
    IProtocolHandler* getHandler(const std::string& url);
    IProtocolHandler* getHandlerByProtocol(const std::string& protocol);

    // 加载内置插件
    void loadBuiltinPlugins();

    // 加载外部插件
    bool loadExternalPlugin(const std::string& path);

    // 查询
    std::vector<std::string> getSupportedProtocols() const;
    bool supportsProtocol(const std::string& protocol) const;

    // 插件信息
    size_t getPluginCount() const;
    std::vector<PluginInfo> getPluginInfo() const;
};

}
```

### 使用示例

```cpp
falcon::PluginManager manager;

// 加载内置插件
manager.loadBuiltinPlugins();

// 查询支持的协议
auto protocols = manager.getSupportedProtocols();
for (const auto& proto : protocols) {
    std::cout << "支持: " << proto << std::endl;
}

// 获取特定协议的处理器
auto* handler = manager.getHandlerByProtocol("http");
if (handler) {
    std::cout << "HTTP 插件: " << handler->name() << std::endl;
}
```

## 协议选项

### HttpOptions

```cpp
namespace falcon {

struct HttpOptions {
    // 认证
    std::string username;
    std::string password;

    // 连接
    int timeout = 30;
    int connect_timeout = 10;
    bool keep_alive = true;

    // SSL/TLS
    bool verify_ssl = true;
    std::string ssl_cert_file;
    std::string ssl_key_file;

    // 请求
    std::string user_agent = "Falcon/1.0";
    std::map<std::string, std::string> headers;
    std::string referer;

    // 压缩
    bool accept_encoding = true;

    // 重定向
    bool follow_redirects = true;
    int max_redirects = 5;
};

}
```

### FtpOptions

```cpp
namespace falcon {

struct FtpOptions {
    // 认证
    std::string username = "anonymous";
    std::string password = "anonymous@";

    // 连接
    bool passive_mode = true;
    int port = 21;

    // SSL/TLS
    enum class SslMode { NONE, EXPLICIT, IMPLICIT };
    SslMode ssl_mode = SslMode::NONE;
    bool verify_ssl = true;

    // 传输
    enum class TransferMode { BINARY, ASCII };
    TransferMode transfer_mode = TransferMode::BINARY;

    // 超时
    int timeout = 30;
    int connect_timeout = 30;
};

}
```

### BittorrentOptions

```cpp
namespace falcon {

struct BittorrentOptions {
    // 保存
    std::string save_path = "./downloads";

    // 连接
    int port = 6881;
    int max_connections = 500;
    int max_uploads = 8;

    // 网络发现
    bool enable_dht = true;
    bool enable_lsd = true;
    bool enable_upnp = true;
    bool enable_pex = true;

    // 传输
    bool sequential_download = false;
    std::vector<std::string> selected_files;

    // 速度限制
    int64_t download_limit = 0;
    int64_t upload_limit = 0;

    // 做种
    bool seed_mode = false;
    float seed_ratio = 0;
    std::chrono::minutes seed_time{0};
};

}
```

### HlsOptions

```cpp
namespace falcon {

struct HlsOptions {
    // 输出
    std::string output_file;

    // 质量
    std::string quality = "auto";  // auto, 360p, 480p, 720p, 1080p
    bool auto_select = true;
    int stream_index = -1;

    // 加密
    std::string encryption_key;
    std::string encryption_iv;

    // 合并
    bool merge_segments = true;
    std::string ffmpeg_path = "ffmpeg";
};

}
```

## PluginInfo

插件信息结构。

```cpp
namespace falcon {

struct PluginInfo {
    std::string name;           // 插件名称
    std::string protocol;       // 协议名称
    std::string version;        // 版本号
    std::string description;    // 描述
    std::vector<std::string> extensions;  // 支持的扩展名
};

}
```

::: tip 更多 API
- [核心 API](./core.md)
- [事件 API](./events.md)
:::
