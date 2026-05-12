/**
 * @file qqdl_plugin.hpp
 * @brief QQ旋风 QQDL 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/protocol_handler.hpp>
#include <falcon/protocol_handler_extension.hpp>
#include <string>
#include <vector>
#include <memory>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>

namespace falcon {

// Forward declaration
class ProtocolRegistry;

namespace protocols {

/**
 * @class QQDLHandler
 * @brief QQ旋风 QQDL 协议处理器
 *
 * 支持 qqlink:// 格式的链接解析和下载
 * QQ旋风链接是对原始 URL 经过特定编码和加密后的结果
 */
class QQDLHandler : public IProtocolHandler, public IProtocolHandlerExtension {
public:
    /**
     * @brief 构造函数
     */
    QQDLHandler();

    /**
     * @brief 析构函数
     */
    ~QQDLHandler() override;

    // Non-copyable
    QQDLHandler(const QQDLHandler&) = delete;
    QQDLHandler& operator=(const QQDLHandler&) = delete;

    // IProtocolHandler 接口实现
    [[nodiscard]] std::string protocol_name() const override { return "qqdl"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override;

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] int priority() const override { return 35; }

    // IProtocolHandlerExtension 接口实现
    void set_protocol_registry(ProtocolRegistry* registry) override {
        registry_ = registry;
    }

private:
    /**
     * @brief 任务上下文
     */
    struct TaskContext {
        DownloadTask::Ptr task;
        IEventListener* listener = nullptr;
        std::string decodedUrl;
        std::atomic<bool> running{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> cancelled{false};
        std::thread downloadThread;
        std::mutex mutex;
        uint64_t downloadedBytes = 0;
    };

    /**
     * @brief 解析QQ旋风链接
     */
    std::string parseQQUrl(const std::string& qqUrl);

    /**
     * @brief 解码QQ旋风链接
     */
    std::string decodeQQUrl(const std::string& encoded);

    /**
     * @brief 下载线程主函数
     */
    void downloadThreadMain(std::shared_ptr<TaskContext> ctx);

    /**
     * @brief 委托给实际协议处理器下载
     */
    void delegateDownload(std::shared_ptr<TaskContext> ctx);

    // 活动任务管理
    std::map<TaskId, std::shared_ptr<TaskContext>> activeTasks_;
    std::mutex tasksMutex_;

    // ProtocolRegistry 用于委托下载
    ProtocolRegistry* registry_ = nullptr;
    std::mutex registryMutex_;
};

/// Factory function to create QQDL handler
std::unique_ptr<IProtocolHandler> create_qqdl_handler();

} // namespace protocols
} // namespace falcon
