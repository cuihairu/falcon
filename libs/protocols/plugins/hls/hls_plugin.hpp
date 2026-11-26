/**
 * @file hls_plugin.hpp
 * @brief HLS (HTTP Live Streaming) 和 DASH 流媒体协议插件
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
#include <condition_variable>
#include <queue>

namespace falcon {

// Forward declaration
class ProtocolRegistry;

namespace protocols {

/**
 * @class HLSHandler
 * @brief HLS 和 DASH 流媒体协议处理器
 *
 * 支持 HLS (.m3u8) 和 DASH (.mpd) 流媒体下载
 */
class HLSHandler : public IProtocolHandler, public IProtocolHandlerExtension {
public:
    /**
     * @brief 构造函数
     */
    HLSHandler();

    /**
     * @brief 析构函数
     */
    ~HLSHandler() override;

    // Non-copyable
    HLSHandler(const HLSHandler&) = delete;
    HLSHandler& operator=(const HLSHandler&) = delete;

    // IProtocolHandler 接口实现
    [[nodiscard]] std::string protocol_name() const override { return "hls"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override;

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] bool supports_segments() const override { return true; }

    [[nodiscard]] int priority() const override { return 45; }

    // IProtocolHandlerExtension 接口实现
    void set_protocol_registry(ProtocolRegistry* registry) override {
        registry_ = registry;
    }

private:
    /**
     * @brief 媒体段信息
     */
    struct MediaSegment {
        std::string url;
        double duration = 0;
        std::string title;
        uint64_t size = 0;
    };

    /**
     * @brief 任务上下文
     */
    struct TaskContext {
        DownloadTask::Ptr task;
        IEventListener* listener = nullptr;
        std::vector<MediaSegment> segments;
        std::atomic<bool> running{false};
        std::atomic<bool> paused{false};
        std::atomic<bool> cancelled{false};
        std::thread downloadThread;
        std::mutex mutex;
        uint64_t downloadedBytes = 0;
        uint64_t totalSize = 0;
    };

    /**
     * @brief 解析HLS M3U8播放列表
     */
    std::vector<MediaSegment> parseM3U8(const std::string& m3u8Content,
                                        const std::string& baseUrl);

    /**
     * @brief 解析EXTINF标签
     */
    std::pair<double, std::string> parseExtInf(const std::string& extinf);

    /**
     * @brief 解析相对URL
     */
    std::string resolveUrl(const std::string& url, const std::string& baseUrl);

    /**
     * @brief 下载线程主函数
     */
    void downloadThreadMain(std::shared_ptr<TaskContext> ctx);

    /**
     * @brief 检查是否为HLS流
     */
    bool isHLSStream(const std::string& url) const;

    /**
     * @brief 检查是否为DASH流
     */
    bool isDASHStream(const std::string& url) const;

    /**
     * @brief 下载 M3U8 播放列表内容
     */
    std::string downloadM3U8(const std::string& url);

    /**
     * @brief 下载单个媒体段
     */
    std::vector<uint8_t> downloadSegment(const std::string& url);

    /**
     * @brief 合并所有段到最终文件
     */
    bool mergeSegments(const std::vector<std::string>& segmentFiles,
                      const std::string& outputFile);

    /**
     * @brief 下载所有段的主逻辑
     */
    void downloadAllSegments(std::shared_ptr<TaskContext> ctx);

    // 活动任务管理
    std::map<TaskId, std::shared_ptr<TaskContext>> activeTasks_;
    std::mutex tasksMutex_;

    // ProtocolRegistry 用于下载播放列表和段
    ProtocolRegistry* registry_ = nullptr;
    std::mutex registryMutex_;
};

/// Factory function to create HLS handler
std::unique_ptr<IProtocolHandler> create_hls_handler();

} // namespace protocols
} // namespace falcon
