# 插件开发指南

本文档基于当前公共插件接口说明如何新增协议处理器。

## 公共接口

插件需要实现 `IProtocolHandler`：

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

## 新建处理器

### 头文件

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

### 实现骨架

```cpp
void MyProtocolHandler::download(
    falcon::DownloadTask::Ptr task,
    falcon::IEventListener* listener) {
    task->set_status(falcon::TaskStatus::Preparing);

    auto info = get_file_info(task->url(), task->options());
    task->set_file_info(info);

    task->set_status(falcon::TaskStatus::Downloading);

    // 下载循环中持续更新进度
    task->update_progress(downloaded, total, speed);

    // 完成时写入输出路径并结束
    task->set_output_path(task->output_path());
    task->set_status(falcon::TaskStatus::Completed);
}
```

这里的关键点是：

- 插件接收的是已有的 `DownloadTask`
- 任务状态、文件信息、进度都应通过 `DownloadTask` 回写
- 监听器接口使用的是 `on_progress()` / `on_completed()` 这一套当前事件名

## 注册方式

### 直接注册

```cpp
falcon::DownloadEngine engine;
engine.register_handler(std::make_unique<MyProtocolHandler>());
```

### 工厂注册

```cpp
std::unique_ptr<falcon::IProtocolHandler> create_myproto_handler() {
    return std::make_unique<MyProtocolHandler>();
}

engine.register_handler_factory(&create_myproto_handler);
```

## CMake 接入

如果要把插件纳入仓库构建，参考现有插件目录组织，例如：

```text
packages/libfalcon-protocols/plugins/http/
packages/libfalcon-protocols/plugins/ftp/
```

最小示例：

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

## 测试建议

优先测试这些点：

- `protocol_name()` 是否正确
- `supported_schemes()` 是否覆盖目标 scheme
- `can_handle()` 是否只匹配预期 URL
- `get_file_info()` 是否返回合理的 `FileInfo`
- `download()` 是否正确更新 `TaskStatus`

示例：

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

## 插件元数据

如果你维护独立插件仓库，可以使用类似这样的清单：

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

## 边界说明

当前公共头文件没有把协议专属配置结构体作为稳定 API 全量公开，所以新插件文档不应默认扩展 `DownloadOptions` 去挂载 `HttpOptions` / `FtpOptions` 风格的字段，除非这些字段已经真的进入公共接口。
