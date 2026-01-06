# 插件开发指南

Falcon 采用插件化架构，每个下载协议都是独立的插件。本文档介绍如何开发新的协议插件。

## 插件接口

所有协议插件必须实现 `IProtocolHandler` 接口：

```cpp
namespace falcon {

class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    /// 返回协议名称
    virtual std::string protocol() const = 0;

    /// 检查是否可以处理该 URL
    virtual bool canHandle(const std::string& url) const = 0;

    /// 开始下载
    virtual std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener
    ) = 0;

    /// 可选：获取插件信息
    virtual std::string name() const { return protocol(); }
    virtual std::string version() const { return "1.0.0"; }
    virtual std::vector<std::string> supportedExtensions() const { return {}; }
};

}
```

## 创建新插件

### 步骤 1: 定义插件类

创建 `plugins/myprotocol/myprotocol_plugin.hpp`：

```cpp
#pragma once
#include <falcon/plugin_interface.hpp>

namespace falcon {
namespace myprotocol {

class MyProtocolPlugin : public IProtocolHandler {
public:
    // 协议名称
    std::string protocol() const override {
        return "myproto";
    }

    // 插件名称
    std::string name() const override {
        return "MyProtocol";
    }

    // 版本
    std::string version() const override {
        return "1.0.0";
    }

    // 检查 URL 是否支持
    bool canHandle(const std::string& url) const override {
        return url.find("myproto://") == 0;
    }

    // 开始下载
    std::shared_ptr<DownloadTask> download(
        const std::string& url,
        const DownloadOptions& options,
        IEventListener* listener
    ) override;

    // 支持的文件扩展名
    std::vector<std::string> supportedExtensions() const override {
        return {".myproto", ".mp"};
    }
};

} // namespace myprotocol
} // namespace falcon
```

### 步骤 2: 实现插件

创建 `plugins/myprotocol/myprotocol_plugin.cpp`：

```cpp
#include "myprotocol_plugin.hpp"
#include <falcon/download_task.hpp>
#include <thread>

namespace falcon {
namespace myprotocol {

std::shared_ptr<DownloadTask> MyProtocolPlugin::download(
    const std::string& url,
    const DownloadOptions& options,
    IEventListener* listener
) {
    // 创建下载任务
    auto task = std::make_shared<DownloadTask>(url);

    // 在新线程中执行下载
    std::thread([this, url, options, listener, task]() {
        try {
            // 1. 解析 URL
            std::string host = parseHost(url);
            std::string path = parsePath(url);

            if (listener) {
                listener->onStatusChanged({"Connecting..."});
            }

            // 2. 建立连接
            Connection conn = connect(host);

            // 3. 请求数据
            std::vector<uint8_t> data = requestData(conn, path);

            // 4. 保存文件
            std::string output_path = options.output_file;
            if (output_path.empty()) {
                output_path = getFilenameFromUrl(url);
            }

            saveToFile(data, output_path);

            // 5. 通知完成
            if (listener) {
                CompleteEvent event;
                event.url = url;
                event.output_path = output_path;
                event.total_bytes = data.size();
                listener->onComplete(event);
            }

            task->setStatus(TaskStatus::Completed);

        } catch (const std::exception& e) {
            if (listener) {
                ErrorEvent event;
                event.url = url;
                event.error_message = e.what();
                listener->onError(event);
            }
            task->setStatus(TaskStatus::Failed);
        }
    }).detach();

    return task;
}

// 辅助函数实现
std::string MyProtocolPlugin::parseHost(const std::string& url) {
    // myproto://host/path
    size_t start = strlen("myproto://");
    size_t end = url.find('/', start);
    return url.substr(start, end - start);
}

std::string MyProtocolPlugin::parsePath(const std::string& url) {
    size_t pos = url.find('/', strlen("myproto://"));
    if (pos == std::string::npos) {
        return "/";
    }
    return url.substr(pos);
}

Connection MyProtocolPlugin::connect(const std::string& host) {
    // 实现连接逻辑
    Connection conn;
    conn.connect(host, 1234);  // 假设协议端口是 1234
    return conn;
}

std::vector<uint8_t> MyProtocolPlugin::requestData(
    Connection& conn,
    const std::string& path
) {
    // 实现请求数据逻辑
    std::vector<uint8_t> data;
    // ... 下载逻辑 ...
    return data;
}

void MyProtocolPlugin::saveToFile(
    const std::vector<uint8_t>& data,
    const std::string& path
) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), data.size());
}

} // namespace myprotocol
} // namespace falcon
```

### 步骤 3: 注册插件

在 `src/plugin_manager.cpp` 中注册：

```cpp
#include "plugins/myprotocol/myprotocol_plugin.hpp"

void PluginManager::loadBuiltinPlugins() {
    // 注册内置插件
    registerPlugin(std::make_unique<http::HttpPlugin>());
    registerPlugin(std::make_unique<ftp::FtpPlugin>());
    registerPlugin(std::make_unique<myprotocol::MyProtocolPlugin>());  // 新插件
}
```

### 步骤 4: 更新 CMakeLists.txt

```cmake
# 添加新插件
add_library(falcon_myprotocol_plugin
    plugins/myprotocol/myprotocol_plugin.cpp
    plugins/myprotocol/myprotocol_plugin.hpp
)

target_link_libraries(falcon_myprotocol_plugin
    PUBLIC
        falcon_core
        ${dependency_libs}
)
```

## 下载任务实现

### 基本任务

```cpp
class DownloadTask {
public:
    DownloadTask(const std::string& url) : url_(url) {}

    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [this]() {
            return status_ == TaskStatus::Completed ||
                   status_ == TaskStatus::Failed;
        });
    }

    TaskStatus getStatus() const {
        return status_;
    }

    const std::string& getUrl() const { return url_; }

protected:
    void setStatus(TaskStatus status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = status;
        condition_.notify_all();
    }

private:
    std::string url_;
    std::atomic<TaskStatus> status_{TaskStatus::Pending};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
};
```

### 支持进度通知

```cpp
class DownloadTaskWithProgress : public DownloadTask {
public:
    void reportProgress(int64_t downloaded, int64_t total) {
        if (listener_) {
            ProgressEvent event;
            event.url = getUrl();
            event.downloaded_bytes = downloaded;
            event.total_bytes = total;
            event.download_speed = calculateSpeed();
            listener_->onProgress(event);
        }
    }

    void setListener(IEventListener* listener) {
        listener_ = listener;
    }

private:
    IEventListener* listener_ = nullptr;

    double calculateSpeed() {
        // 计算下载速度
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start_time_
        ).count();
        if (elapsed > 0) {
            return static_cast<double>(downloaded_bytes_) / elapsed * 1000;
        }
        return 0;
    }

    std::chrono::steady_clock::time_point start_time_;
    int64_t downloaded_bytes_ = 0;
};
```

## 事件监听器

### 实现监听器

```cpp
class ConsoleEventListener : public IEventListener {
public:
    void onProgress(const ProgressEvent& event) override {
        double percent = 0;
        if (event.total_bytes > 0) {
            percent = static_cast<double>(event.downloaded_bytes) /
                     event.total_bytes * 100;
        }

        std::cout << "\r["
                  << std::string(static_cast<int>(percent / 2), '=')
                  << "] "
                  << std::fixed << std::setprecision(1) << percent
                  << "% (" << formatBytes(event.downloaded_bytes)
                  << " / " << formatBytes(event.total_bytes) << ")"
                  << std::flush;
    }

    void onComplete(const CompleteEvent& event) override {
        std::cout << "\n下载完成: " << event.output_path << std::endl;
    }

    void onError(const ErrorEvent& event) override {
        std::cerr << "\n下载失败: " << event.error_message << std::endl;
    }

private:
    std::string formatBytes(int64_t bytes) {
        const char* units[] = {"B", "KB", "MB", "GB"};
        int unit = 0;
        double size = static_cast<double>(bytes);
        while (size >= 1024 && unit < 4) {
            size /= 1024;
            unit++;
        }
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << size << units[unit];
        return oss.str();
    }
};
```

## 协议选项

### 定义协议特定选项

```cpp
namespace falcon {

struct MyProtocolOptions {
    // 连接选项
    int port = 1234;
    std::string username;
    std::string password;

    // 传输选项
    bool use_compression = true;
    int timeout_seconds = 30;

    // 高级选项
    bool verify_checksum = false;
    std::string custom_header;
};

}

// 在 DownloadOptions 中添加
struct DownloadOptions {
    // 通用选项
    int max_connections = 1;
    std::string output_file;

    // 协议特定选项
    std::optional<HttpOptions> http_options;
    std::optional<FtpOptions> ftp_options;
    std::optional<MyProtocolOptions> myproto_options;
};
```

## 测试插件

### 单元测试

```cpp
#include <gtest/gtest.h>
#include <falcon/plugins/myprotocol/myprotocol_plugin.hpp>

TEST(MyProtocolPluginTest, ProtocolName) {
    falcon::myprotocol::MyProtocolPlugin plugin;
    EXPECT_EQ(plugin.protocol(), "myproto");
}

TEST(MyProtocolPluginTest, CanHandleValidUrl) {
    falcon::myprotocol::MyProtocolPlugin plugin;
    EXPECT_TRUE(plugin.canHandle("myproto://example.com/file"));
}

TEST(MyProtocolPluginTest, CannotHandleInvalidUrl) {
    falcon::myprotocol::MyProtocolPlugin plugin;
    EXPECT_FALSE(plugin.canHandle("http://example.com/file"));
}

TEST(MyProtocolPluginTest, DownloadCreatesFile) {
    falcon::myprotocol::MyProtocolPlugin plugin;
    falcon::DownloadOptions options;
    options.output_file = "/tmp/test_output";

    auto task = plugin.download(
        "myproto://example.com/test",
        options,
        nullptr
    );

    task->wait();
    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Completed);
    EXPECT_TRUE(std::ifstream("/tmp/test_output").good());
}
```

### 集成测试

```cpp
TEST(MyProtocolPluginIntegration, DownloadFromTestServer) {
    // 启动测试服务器
    TestMyProtocolServer server(1234);
    server.start();

    // 创建插件和下载
    falcon::myprotocol::MyProtocolPlugin plugin;
    falcon::DownloadOptions options;
    options.output_file = "/tmp/integration_test";

    auto task = plugin.download(
        "myproto://localhost:1234/testfile",
        options,
        nullptr
    );

    task->wait();

    EXPECT_EQ(task->getStatus(), falcon::TaskStatus::Completed);
    EXPECT_TRUE(filesMatch("/tmp/integration_test", "testfiles/expected"));

    server.stop();
}
```

## 发布插件

### 打包

```cmake
# CMakeLists.txt for standalone plugin
cmake_minimum_required(VERSION 3.15)
project(falcon_myproto_plugin VERSION 1.0.0)

add_library(${PROJECT_NAME} SHARED
    src/myprotocol_plugin.cpp
)

target_include_directories(${PROJECT_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# 安装
install(TARGETS ${PROJECT_NAME}
    LIBRARY DESTINATION lib/falcon/plugins
    RUNTIME DESTINATION bin
)

install(FILES
    include/falcon/plugins/myprotocol/myprotocol_plugin.hpp
    DESTINATION include/falcon/plugins/myprotocol
)
```

### 插件清单

创建 `plugin.json`：

```json
{
    "name": "MyProtocol",
    "id": "falcon-myproto-plugin",
    "version": "1.0.0",
    "description": "MyProtocol protocol support for Falcon",
    "author": "Your Name",
    "license": "Apache-2.0",
    "protocols": ["myproto"],
    "homepage": "https://github.com/yourusername/falcon-myproto-plugin",
    "repository": "https://github.com/yourusername/falcon-myproto-plugin",
    "falcon_version": ">=1.0.0"
}
```

::: tip 最佳实践
- 保持插件接口简单
- 提供完整的错误处理
- 实现进度报告
- 编写全面的测试
- 提供使用示例
:::
