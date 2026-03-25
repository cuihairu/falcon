# 插件 API

本文档说明当前公共插件接口，而不是尚未进入公共头文件的协议专属选项设计稿。

## IProtocolHandler

所有协议处理器都需要实现 `IProtocolHandler`。

### 接口定义

```cpp
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    virtual std::string protocol_name() const = 0;
    virtual std::vector<std::string> supported_schemes() const = 0;
    virtual bool can_handle(const std::string& url) const = 0;

    virtual FileInfo get_file_info(
        const std::string& url,
        const DownloadOptions& options) = 0;

    virtual void download(
        DownloadTask::Ptr task,
        IEventListener* listener) = 0;

    virtual void pause(DownloadTask::Ptr task) = 0;
    virtual void resume(
        DownloadTask::Ptr task,
        IEventListener* listener) = 0;
    virtual void cancel(DownloadTask::Ptr task) = 0;

    virtual bool supports_resume() const { return false; }
    virtual bool supports_segments() const { return false; }
    virtual int priority() const { return 0; }
};
```

### 要点

- `protocol_name()` 返回处理器标识，例如 `http`、`ftp`。
- `supported_schemes()` 返回 URL scheme 列表，例如 `http` / `https`。
- `download()` 和 `resume()` 接收的是 `DownloadTask::Ptr`，而不是单独的 URL 字符串。
- 下载过程中的状态和进度应通过 `DownloadTask` 与 `IEventListener` 协同更新。

## ProtocolHandlerFactory

公共工厂类型如下：

```cpp
using ProtocolHandlerFactory = std::unique_ptr<IProtocolHandler> (*)();
```

这允许你通过 `DownloadEngine::register_handler_factory()` 延迟注册协议处理器。

## PluginManager

`PluginManager` 是公共头文件里可见的插件管理器接口，负责注册和查询协议处理器。

### 主要接口

```cpp
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    void registerPlugin(std::unique_ptr<IProtocolHandler> plugin);
    IProtocolHandler* getPlugin(const std::string& protocol) const;
    IProtocolHandler* getPluginByUrl(const std::string& url) const;

    void loadAllPlugins();
    std::vector<std::string> getSupportedProtocols() const;
    std::vector<std::string> listSupportedSchemes() const;
    bool supportsUrl(const std::string& url) const;

    void unloadPlugin(const std::string& protocol);
    void unloadAllPlugins();
    size_t getPluginCount() const;
};
```

### 使用示例

```cpp
falcon::PluginManager manager;
manager.loadAllPlugins();

for (const auto& protocol : manager.getSupportedProtocols()) {
    std::cout << protocol << std::endl;
}

if (auto* handler = manager.getPluginByUrl("https://example.com/file.zip")) {
    std::cout << handler->protocol_name() << std::endl;
}
```

## 与 DownloadEngine 的关系

大多数业务代码不需要直接操作 `PluginManager`。通常更简单的方式是：

```cpp
falcon::DownloadEngine engine;
auto task = engine.add_task("https://example.com/file.zip");
```

如果你需要注册自定义协议处理器，可以使用：

```cpp
engine.register_handler(std::make_unique<MyProtocolHandler>());
```

或：

```cpp
engine.register_handler_factory(&create_my_protocol_handler);
```

## 自定义处理器示例

```cpp
class MyProtocolHandler : public falcon::IProtocolHandler {
public:
    std::string protocol_name() const override { return "myproto"; }

    std::vector<std::string> supported_schemes() const override {
        return {"myproto"};
    }

    bool can_handle(const std::string& url) const override {
        return url.rfind("myproto://", 0) == 0;
    }

    falcon::FileInfo get_file_info(
        const std::string& url,
        const falcon::DownloadOptions& options) override;

    void download(
        falcon::DownloadTask::Ptr task,
        falcon::IEventListener* listener) override;

    void pause(falcon::DownloadTask::Ptr task) override;
    void resume(
        falcon::DownloadTask::Ptr task,
        falcon::IEventListener* listener) override;
    void cancel(falcon::DownloadTask::Ptr task) override;
};
```

::: tip 边界说明
`HttpOptions`、`FtpOptions`、`BittorrentOptions`、`HlsOptions` 这类协议专属配置结构当前不在公共头文件里作为稳定 API 暴露，因此这里不再把它们写成“现有公共接口”。
:::
