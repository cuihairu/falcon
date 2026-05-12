/**
 * @file flashget_plugin.hpp
 * @brief 快车 FlashGet 协议插件
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
 * @class FlashGetHandler
 * @brief 快车 FlashGet 协议处理器
 *
 * 支持 flashget:// 格式的链接解析和下载
 * FlashGet 链接通常包含 URL 和引用站点信息
 */
class FlashGetHandler : public IProtocolHandler, public IProtocolHandlerExtension {
public:
    /**
     * @brief 构造函数
     */
    FlashGetHandler();

    /**
     * @brief 析构函数
     */
    ~FlashGetHandler() override;

    // Non-copyable
    FlashGetHandler(const FlashGetHandler&) = delete;
    FlashGetHandler& operator=(const FlashGetHandler&) = delete;

    // IProtocolHandler 接口实现
    [[nodiscard]] std::string protocol_name() const override { return "flashget"; }

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
     * @brief 解析快车链接
     */
    std::string parseFlashGetUrl(const std::string& flashgetUrl);

    /**
     * @brief 解码快车链接
     */
    std::string decodeFlashGetUrl(const std::string& encoded);

    /**
     * @brief URL解码
     */
    std::string urlDecode(const std::string& encoded);

    /**
     * @brief Base64解码
     */
    std::string base64_decode(const std::string& encoded);

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

/// Factory function to create FlashGet handler
std::unique_ptr<IProtocolHandler> create_flashget_handler();

} // namespace protocols
} // namespace falcon
