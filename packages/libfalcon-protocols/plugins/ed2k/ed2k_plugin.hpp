/**
 * @file ed2k_plugin.hpp
 * @brief ED2K (eDonkey2000) 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include <falcon/protocol_handler.hpp>
#include <falcon/protocol_handler_extension.hpp>
#include <string>
#include <vector>
#include <memory>
#include <array>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>

namespace falcon {

// Forward declaration
class ProtocolRegistry;

namespace protocols {

/**
 * @class ED2KHandler
 * @brief ED2K (eDonkey2000) 协议处理器
 *
 * 支持 ed2k:// 格式的链接解析和下载
 * ED2K 是 eDonkey2000 网络使用的协议，也被称为电驴协议
 */
class ED2KHandler : public IProtocolHandler, public IProtocolHandlerExtension {
public:
    /**
     * @brief 构造函数
     */
    ED2KHandler();

    /**
     * @brief 析构函数
     */
    ~ED2KHandler() override;

    // Non-copyable
    ED2KHandler(const ED2KHandler&) = delete;
    ED2KHandler& operator=(const ED2KHandler&) = delete;

    // IProtocolHandler 接口实现
    [[nodiscard]] std::string protocol_name() const override { return "ed2k"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override;

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] int priority() const override { return 30; }

    // IProtocolHandlerExtension 接口实现
    void set_protocol_registry(ProtocolRegistry* registry) override {
        registry_ = registry;
    }

private:
    /**
     * @brief ED2K文件信息结构
     */
    struct ED2KFileInfo {
        std::string filename;     // 文件名
        uint64_t filesize = 0;    // 文件大小（字节）
        std::array<uint8_t, 16> hash{};  // MD4哈希
        std::vector<std::string> sources;  // 源地址列表
        std::string aich;         // AICH哈希（可选）
        uint32_t priority = 0;    // 优先级（可选）
    };

    /**
     * @brief 任务上下文
     */
    struct TaskContext {
        DownloadTask::Ptr task;
        IEventListener* listener = nullptr;
        ED2KFileInfo fileInfo;
        std::atomic<bool> running{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> cancelled{false};
        std::thread downloadThread;
        std::mutex mutex;
        uint64_t downloadedBytes = 0;
    };

    /**
     * @brief 解析ED2K链接
     */
    ED2KFileInfo parseED2KUrl(const std::string& ed2kUrl);

    /**
     * @brief 解析文件链接
     */
    ED2KFileInfo parseFileLink(const std::vector<std::string>& params);

    /**
     * @brief 解析服务器链接
     */
    struct ServerInfo {
        std::string host;
        uint16_t port = 0;
        std::string name;
    };
    ServerInfo parseServerLink(const std::vector<std::string>& params);

    /**
     * @brief URL解码
     */
    std::string urlDecode(const std::string& encoded);

    /**
     * @brief 解析MD4哈希
     */
    std::array<uint8_t, 16> parseMD4Hash(const std::string& hashStr);

    /**
     * @brief 解析源地址
     */
    std::vector<std::string> parseSources(const std::string& sourcesStr);

    /**
     * @brief URL编码
     */
    std::string urlEncode(const std::string& str);

    /**
     * @brief 哈希转字符串
     */
    std::string hashToString(const std::array<uint8_t, 16>& hash);

    /**
     * @brief 下载线程主函数
     */
    void downloadThreadMain(std::shared_ptr<TaskContext> ctx);

    /**
     * @brief 尝试从源地址下载（委托给 HTTP）
     */
    void downloadFromSources(std::shared_ptr<TaskContext> ctx);

    /**
     * @brief 委托给实际协议处理器下载
     */
    void delegateDownload(std::shared_ptr<TaskContext> ctx, const std::string& url);

    // 活动任务管理
    std::map<TaskId, std::shared_ptr<TaskContext>> activeTasks_;
    std::mutex tasksMutex_;

    // ProtocolRegistry 用于委托下载
    ProtocolRegistry* registry_ = nullptr;
    std::mutex registryMutex_;
};

/// Factory function to create ED2K handler
std::unique_ptr<IProtocolHandler> create_ed2k_handler();

} // namespace protocols
} // namespace falcon
