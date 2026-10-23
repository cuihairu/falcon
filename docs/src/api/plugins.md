# Plugin API

This document describes the current public plugin interface, not protocol-specific option designs that have not yet entered the public headers.

## IProtocolHandler

Every protocol handler must implement `IProtocolHandler`.

### Interface definition

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

### Key points

- `protocol_name()` returns the handler identifier, for example `http` or `ftp`.
- `supported_schemes()` returns the URL schemes, for example `http` and `https`.
- `download()` and `resume()` receive a `DownloadTask::Ptr`, not a bare URL string.
- Update status and progress during a download through `DownloadTask` together with `IEventListener`.

## ProtocolHandlerFactory

The public factory type is:

```cpp
using ProtocolHandlerFactory = std::unique_ptr<IProtocolHandler> (*)();
```

This lets you register a protocol handler lazily through `DownloadEngine::register_handler_factory()`.

## ProtocolRegistry

The registration and lookup interface currently exposed by the public headers is `ProtocolRegistry`, which registers and resolves protocol handlers.

### Main interface

```cpp
class ProtocolRegistry {
public:
    ProtocolRegistry();
    ~ProtocolRegistry();

    void register_handler(std::unique_ptr<IProtocolHandler> handler);
    IProtocolHandler* get_handler(const std::string& protocol) const;
    IProtocolHandler* get_handler_for_url(const std::string& url) const;

    void load_builtin_handlers();
    std::vector<std::string> supported_protocols() const;
    std::vector<std::string> supported_schemes() const;
    bool supports_url(const std::string& url) const;
    size_t handler_count() const;
};
```

### Usage example

```cpp
falcon::ProtocolRegistry registry;
registry.load_builtin_handlers();

for (const auto& protocol : registry.supported_protocols()) {
    std::cout << protocol << std::endl;
}

if (auto* handler = registry.get_handler_for_url("https://example.com/file.zip")) {
    std::cout << handler->protocol_name() << std::endl;
}
```

## Relationship to DownloadEngine

Most application code does not need to operate `ProtocolRegistry` directly. The simpler path is usually:

```cpp
falcon::DownloadEngine engine;
auto task = engine.add_task("https://example.com/file.zip");
```

To register a custom protocol handler, use:

```cpp
engine.register_handler(std::make_unique<MyProtocolHandler>());
```

or:

```cpp
engine.register_handler_factory(&create_my_protocol_handler);
```

## Custom handler example

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

::: tip Scope
Protocol-specific configuration structs such as `HttpOptions`, `FtpOptions`, `BittorrentOptions`, and `HlsOptions` are not currently exposed as stable public APIs in the public headers, so they are not documented here as existing public interfaces.
:::
