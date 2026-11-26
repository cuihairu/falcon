# Plugin Development Guide

This guide explains how to add a protocol handler using the current public plugin interface.

## Public interface

A plugin must implement `IProtocolHandler`:

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
};
```

## Creating a handler

### Header

```cpp
#pragma once

#include <falcon/protocol_handler.hpp>

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

### Implementation skeleton

```cpp
void MyProtocolHandler::download(
    falcon::DownloadTask::Ptr task,
    falcon::IEventListener* listener) {
    task->set_status(falcon::TaskStatus::Preparing);

    auto info = get_file_info(task->url(), task->options());
    task->set_file_info(info);

    task->set_status(falcon::TaskStatus::Downloading);

    // Keep updating progress inside the download loop.
    task->update_progress(downloaded, total, speed);

    // Write the output path and finish when complete.
    task->set_output_path(task->output_path());
    task->set_status(falcon::TaskStatus::Completed);
}
```

Key points:

- A plugin receives an existing `DownloadTask`.
- Write task status, file information, and progress back through `DownloadTask`.
- The listener interface uses the current event names `on_progress()` and `on_completed()`.

## Registering a handler

### Direct registration

```cpp
falcon::DownloadEngine engine;
engine.register_handler(std::make_unique<MyProtocolHandler>());
```

### Factory registration

```cpp
std::unique_ptr<falcon::IProtocolHandler> create_myproto_handler() {
    return std::make_unique<MyProtocolHandler>();
}

engine.register_handler_factory(&create_myproto_handler);
```

## CMake integration

To include a plugin in the repository build, follow the existing plugin directory layout, for example:

```text
packages/libfalcon-protocols/plugins/http/
packages/libfalcon-protocols/plugins/ftp/
```

Minimal example:

```cmake
add_library(falcon_plugin_myproto
    myproto_plugin.cpp
    myproto_plugin.hpp
)

target_link_libraries(falcon_plugin_myproto
    PUBLIC
        Falcon::core
)
```

## Testing guidance

Prioritize testing:

- Whether `protocol_name()` is correct.
- Whether `supported_schemes()` covers the target schemes.
- Whether `can_handle()` matches only the expected URLs.
- Whether `get_file_info()` returns a sensible `FileInfo`.
- Whether `download()` updates `TaskStatus` correctly.

Example:

```cpp
TEST(MyProtocolHandlerTest, CanHandleValidUrl) {
    MyProtocolHandler handler;
    EXPECT_TRUE(handler.can_handle("myproto://example.com/file"));
}

TEST(MyProtocolHandlerTest, ProtocolName) {
    MyProtocolHandler handler;
    EXPECT_EQ(handler.protocol_name(), "myproto");
}
```

## Plugin metadata

If you maintain a standalone plugin repository, a manifest like this can be used:

```json
{
  "name": "MyProtocol",
  "id": "falcon-myproto-plugin",
  "version": "1.0.0",
  "description": "MyProtocol protocol support for Falcon",
  "author": "Your Name",
  "license": "Apache-2.0",
  "protocols": ["myproto"],
  "homepage": "https://github.com/example/falcon-myproto-plugin",
  "repository": "https://github.com/example/falcon-myproto-plugin",
  "falcon_version": ">=1.0.0"
}
```

## Scope

The current public headers do not expose every protocol-specific configuration struct as a stable API. New plugin documentation therefore should not assume that `DownloadOptions` can carry `HttpOptions`- or `FtpOptions`-style fields unless those fields have actually entered the public interface.
