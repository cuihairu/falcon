/**
 * @file thunder_plugin.hpp
 * @brief 迅雷 Thunder 协议插件
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
 * @class ThunderHandler
 * @brief 迅雷 Thunder 协议处理器
 *
 * 支持 thunder:// 和 thunderxl:// 格式的链接解析和下载
 * 迅雷链接是对原始 URL 经过 Base64 编码和特定算法处理后的结果
 */
class ThunderHandler : public IProtocolHandler, public IProtocolHandlerExtension {
public:
    /**
     * @brief 构造函数
     */
    ThunderHandler();

    /**
     * @brief 析构函数
     */
    ~ThunderHandler() override;

    // Non-copyable
    ThunderHandler(const ThunderHandler&) = delete;
    ThunderHandler& operator=(const ThunderHandler&) = delete;

    // IProtocolHandler 接口实现
    [[nodiscard]] std::string protocol_name() const override { return "thunder"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override;

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] int priority() const override { return 40; }

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
     * @brief 解析迅雷链接
     */
    std::string parseThunderUrl(const std::string& thunderUrl);

    /**
     * @brief 解码经典迅雷链接（thunder://）
     */
    std::string decodeClassicThunder(const std::string& encoded);

    /**
     * @brief 解码迅雷离线链接（thunderxl://）
     */
    std::string decodeThunderXL(const std::string& encoded);

    /**
     * @brief 验证解析后的 URL
     */
    bool isValidUrl(const std::string& url);

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

/// Factory function to create Thunder handler
std::unique_ptr<IProtocolHandler> create_thunder_handler();

} // namespace protocols
} // namespace falcon
