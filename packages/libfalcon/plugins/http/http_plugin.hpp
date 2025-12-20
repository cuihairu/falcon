/**
 * @file http_plugin.hpp
 * @brief HTTP/HTTPS 协议插件
 * @author Falcon Team
 * @date 2025-12-21
 */

#pragma once

#include "../base_protocol_plugin.hpp"
#include <string>
#include <vector>
#include <memory>
#include <curl/curl.h>

namespace falcon {
namespace plugins {

/**
 * @class HttpPlugin
 * @brief HTTP/HTTPS 协议处理器
 *
 * 基于 libcurl 实现的 HTTP/HTTPS 下载插件
 * 支持断点续传、分块下载、速度控制等功能
 */
class HttpPlugin : public BaseProtocolPlugin {
public:
    /**
     * @brief 构造函数
     */
    HttpPlugin();

    /**
     * @brief 析构函数
     */
    virtual ~HttpPlugin();

    // ProtocolPlugin 接口实现
    std::string getProtocolName() const override { return "http"; }
    std::vector<std::string> getSupportedSchemes() const override;
    bool canHandle(const std::string& url) const override;
    std::unique_ptr<IDownloadTask> createTask(const std::string& url,
                                            const DownloadOptions& options) override;

private:
    /**
     * @brief HTTP下载任务实现
     */
    class HttpDownloadTask : public IDownloadTask {
    public:
        HttpDownloadTask(const std::string& url, const DownloadOptions& options);
        virtual ~HttpDownloadTask();

        // IDownloadTask 接口实现
        void start() override;
        void pause() override;
        void resume() override;
        void cancel() override;
        TaskStatus getStatus() const override;
        float getProgress() const override;
        uint64_t getTotalBytes() const override;
        uint64_t getDownloadedBytes() const override;
        uint64_t getSpeed() const override;
        std::string getErrorMessage() const override;

    private:
        /**
         * @brief 执行下载
         */
        void performDownload();

        /**
         * @brief 获取文件信息（大小、是否支持断点续传）
         */
        bool getFileInfo();

        /**
         * @brief 创建分块下载任务
         */
        void createChunkedDownloads();

        /**
         * @brief 合并分块文件
         */
        bool mergeChunks();

        /**
         * @brief libcurl 写入回调
         */
        static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);

        /**
         * @brief libcurl 进度回调
         */
        static int progressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow);

        /**
         * @brief 设置 curl 选项
         */
        void setCurlOptions(CURL* curl, uint64_t startByte = 0, uint64_t endByte = 0);

        std::string url_;
        DownloadOptions options_;
        TaskStatus status_;
        std::string errorMessage_;

        // 文件信息
        uint64_t totalSize_;
        uint64_t downloadedBytes_;
        bool supportsResume_;

        // 分块下载
        std::vector<std::unique_ptr<HttpDownloadTask>> chunkTasks_;
        uint32_t numChunks_;

        // libcurl
        CURL* curl_;
        FILE* file_;

        // 速度控制
        std::chrono::steady_clock::time_point lastSpeedCheck_;
        uint64_t bytesInSpeedWindow_;

        // 线程安全
        mutable std::mutex mutex_;
        std::condition_variable cv_;
    };

    /**
     * @brief URL编码
     */
    std::string urlEncode(const std::string& str);

    /**
     * @brief 解析URL
     */
    struct ParsedUrl {
        std::string scheme;
        std::string host;
        std::string port;
        std::string path;
        std::string query;
        std::string fragment;
    };
    ParsedUrl parseUrl(const std::string& url);

    /**
     * @brief 检查是否支持断点续传
     */
    bool supportsResuming(const std::string& url);

    /**
     * @brief 获取重定向后的最终URL
     */
    std::string getFinalUrl(const std::string& url);
};

} // namespace plugins
} // namespace falcon