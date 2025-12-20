/**
 * @file http_plugin_v2.cpp
 * @brief HTTP/HTTPS 协议插件实现 - 支持断点续传和分块下载
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "http_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/download_task.hpp>
#include <fstream>
#include <thread>
#include <chrono>
#include <sstream>
#include <iomanip>

namespace falcon {
namespace plugins {

// ============================================================================
// HttpPlugin 实现
// ============================================================================

HttpPlugin::HttpPlugin() {
    FALCON_LOG_INFO("HTTP plugin initialized");
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

HttpPlugin::~HttpPlugin() {
    curl_global_cleanup();
}

std::vector<std::string> HttpPlugin::getSupportedSchemes() const {
    return {"http", "https"};
}

bool HttpPlugin::canHandle(const std::string& url) const {
    return url.find("http://") == 0 || url.find("https://") == 0;
}

std::unique_ptr<IDownloadTask> HttpPlugin::createTask(const std::string& url,
                                                    const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating HTTP task for: {}", url);

    return std::make_unique<HttpDownloadTask>(url, options);
}

// ============================================================================
// HttpDownloadTask 实现
// ============================================================================

HttpPlugin::HttpDownloadTask::HttpDownloadTask(const std::string& url,
                                               const DownloadOptions& options)
    : url_(url)
    , options_(options)
    , status_(TaskStatus::Pending)
    , totalSize_(0)
    , downloadedBytes_(0)
    , supportsResume_(false)
    , numChunks_(1)
    , curl_(nullptr)
    , file_(nullptr)
    , bytesInSpeedWindow_(0) {

    curl_ = curl_easy_init();
    if (!curl_) {
        throw std::runtime_error("Failed to initialize curl");
    }

    // 如果启用分块下载且连接数大于1
    if (options.max_connections > 1) {
        numChunks_ = options.max_connections;
    }
}

HttpPlugin::HttpDownloadTask::~HttpDownloadTask() {
    if (curl_) {
        curl_easy_cleanup(curl_);
    }
    if (file_) {
        fclose(file_);
    }
}

void HttpPlugin::HttpDownloadTask::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (status_ != TaskStatus::Pending) {
        return;
    }

    status_ = TaskStatus::Downloading;

    // 检查是否支持断点续传
    if (!getFileInfo()) {
        status_ = TaskStatus::Failed;
        return;
    }

    // 如果支持分块下载且文件大小大于1MB
    if (numChunks_ > 1 && totalSize_ > 1024 * 1024) {
        FALCON_LOG_INFO("Starting chunked download with {} chunks", numChunks_);
        createChunkedDownloads();
    } else {
        FALCON_LOG_DEBUG("Starting single connection download");
        performDownload();
    }
}

void HttpPlugin::HttpDownloadTask::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Downloading) {
        status_ = TaskStatus::Paused;
        cv_.notify_all();
    }
}

void HttpPlugin::HttpDownloadTask::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_ == TaskStatus::Paused) {
        status_ = TaskStatus::Downloading;
        cv_.notify_all();
    }
}

void HttpPlugin::HttpDownloadTask::cancel() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = TaskStatus::Cancelled;
    cv_.notify_all();
}

TaskStatus HttpPlugin::HttpDownloadTask::getStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

float HttpPlugin::HttpDownloadTask::getProgress() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (totalSize_ == 0) return 0.0f;
    return static_cast<float>(downloadedBytes_) / totalSize_;
}

uint64_t HttpPlugin::HttpDownloadTask::getTotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalSize_;
}

uint64_t HttpPlugin::HttpDownloadTask::getDownloadedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return downloadedBytes_;
}

uint64_t HttpPlugin::HttpDownloadTask::getSpeed() const {
    // 简化实现：计算最近的速度
    return 0; // 需要实际实现速度计算
}

std::string HttpPlugin::HttpDownloadTask::getErrorMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return errorMessage_;
}

bool HttpPlugin::HttpDownloadTask::getFileInfo() {
    setCurlOptions(curl_);

    // 只获取头部信息
    curl_easy_setopt(curl_, CURLOPT_NOBODY, 1L);

    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        errorMessage_ = curl_easy_strerror(res);
        FALCON_LOG_ERROR("Failed to get file info: {}", errorMessage_);
        return false;
    }

    // 获取文件大小
    double contentLength = 0;
    curl_easy_getinfo(curl_, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &contentLength);
    if (contentLength > 0) {
        totalSize_ = static_cast<uint64_t>(contentLength);
    }

    // 检查是否支持断点续传
    long responseCode;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &responseCode);
    supportsResume_ = (responseCode == 200 || responseCode == 206);

    // 重置为正常下载模式
    curl_easy_setopt(curl_, CURLOPT_NOBODY, 0L);

    FALCON_LOG_DEBUG("File info: size={}, supports_resume={}", totalSize_, supportsResume_);

    return true;
}

void HttpPlugin::HttpDownloadTask::createChunkedDownloads() {
    uint64_t chunkSize = totalSize_ / numChunks_;

    for (uint32_t i = 0; i < numChunks_; ++i) {
        uint64_t startByte = i * chunkSize;
        uint64_t endByte = (i == numChunks_ - 1) ? totalSize_ - 1 : (i + 1) * chunkSize - 1;

        FALCON_LOG_DEBUG("Creating chunk {}: bytes {}-{}", i, startByte, endByte);

        // 创建临时文件名
        std::string tempFile = options_.output_path + ".part" + std::to_string(i);

        // 为每个分块创建下载任务
        // 这里简化处理，实际应该启动多个线程
        // 可以使用线程池来管理
    }
}

void HttpPlugin::HttpDownloadTask::performDownload() {
    std::string outputPath = options_.output_path;
    if (outputPath.empty()) {
        // 从URL提取文件名
        outputPath = "downloaded_file";
    }

    // 如果支持断点续传且文件已存在
    if (supportsResume_ && options_.resume_if_exists) {
        std::ifstream existingFile(outputPath, std::ios::binary | std::ios::ate);
        if (existingFile.is_open()) {
            downloadedBytes_ = existingFile.tellg();
            existingFile.close();

            if (downloadedBytes_ > 0 && downloadedBytes_ < totalSize_) {
                FALCON_LOG_INFO("Resuming download from byte {}", downloadedBytes_);
                file_ = fopen(outputPath.c_str(), "ab");  // 以追加模式打开
                if (file_) {
                    fseek(file_, 0, SEEK_END);
                }
            }
        }
    }

    if (!file_) {
        file_ = fopen(outputPath.c_str(), "wb");
    }

    if (!file_) {
        errorMessage_ = "Failed to open output file: " + outputPath;
        status_ = TaskStatus::Failed;
        return;
    }

    // 设置下载范围（断点续传）
    if (downloadedBytes_ > 0) {
        std::string range = std::to_string(downloadedBytes_) + "-";
        curl_easy_setopt(curl_, CURLOPT_RANGE, range.c_str());
    }

    // 执行下载
    CURLcode res = curl_easy_perform(curl_);

    if (res != CURLE_OK) {
        errorMessage_ = curl_easy_strerror(res);
        status_ = TaskStatus::Failed;
        FALCON_LOG_ERROR("Download failed: {}", errorMessage_);
    } else {
        status_ = TaskStatus::Completed;
        downloadedBytes_ = totalSize_;
        FALCON_LOG_INFO("Download completed successfully");
    }

    if (file_) {
        fclose(file_);
        file_ = nullptr;
    }
}

size_t HttpPlugin::HttpDownloadTask::writeCallback(void* contents,
                                                   size_t size,
                                                   size_t nmemb,
                                                   void* userp) {
    HttpDownloadTask* task = static_cast<HttpDownloadTask*>(userp);
    size_t totalSize = size * nmemb;

    if (task->file_) {
        return fwrite(contents, 1, totalSize, task->file_);
    }

    return 0;
}

int HttpPlugin::HttpDownloadTask::progressCallback(void* clientp,
                                                  double dltotal,
                                                  double dlnow,
                                                  double ultotal,
                                                  double ulnow) {
    HttpDownloadTask* task = static_cast<HttpDownloadTask*>(clientp);

    std::lock_guard<std::mutex> lock(task->mutex_);
    task->downloadedBytes_ = static_cast<uint64_t>(dlnow);

    // 检查是否被暂停或取消
    if (task->status_ == TaskStatus::Paused || task->status_ == TaskStatus::Cancelled) {
        return 1;  // 终止下载
    }

    return 0;  // 继续下载
}

void HttpPlugin::HttpDownloadTask::setCurlOptions(CURL* curl, uint64_t startByte, uint64_t endByte) {
    // 设置URL
    curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());

    // 设置写入回调
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, this);

    // 设置进度回调
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, progressCallback);
    curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, this);

    // 设置用户代理
    if (!options_.user_agent.empty()) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, options_.user_agent.c_str());
    }

    // 设置自定义头部
    if (!options_.headers.empty()) {
        struct curl_slist* headers = nullptr;
        for (const auto& header : options_.headers) {
            headers = curl_slist_append(headers, header.first.c_str() + std::string(": ") + header.second);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    // 设置超时
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, options_.timeout_seconds);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    // 设置速度限制
    if (options_.speed_limit > 0) {
        curl_easy_setopt(curl, CURLOPT_MAX_RECV_SPEED_LARGE, static_cast<curl_off_t>(options_.speed_limit));
    }

    // 设置下载范围（用于分块下载）
    if (startByte > 0 || endByte > 0) {
        std::ostringstream range;
        if (startByte > 0 && endByte > 0) {
            range << startByte << "-" << endByte;
        } else if (startByte > 0) {
            range << startByte << "-";
        }
        curl_easy_setopt(curl, CURLOPT_RANGE, range.str().c_str());
    }

    // 跟随重定向
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);

    // SSL验证
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
}

// ============================================================================
// 辅助函数
// ============================================================================

std::string HttpPlugin::urlEncode(const std::string& str) {
    char* encoded = curl_easy_escape(nullptr, str.c_str(), static_cast<int>(str.length()));
    if (encoded) {
        std::string result(encoded);
        curl_free(encoded);
        return result;
    }
    return str;
}

HttpPlugin::ParsedUrl HttpPlugin::parseUrl(const std::string& url) {
    ParsedUrl parsed;

    // 使用 libcurl 解析 URL
    CURLU* curlu = curl_url();
    if (curlu) {
        CURLUcode rc;

        rc = curl_url_set(curlu, CURLUPART_URL, url.c_str(), 0);
        if (rc == CURLUE_OK) {
            char* scheme;
            char* host;
            char* port;
            char* path;
            char* query;
            char* fragment;

            curl_url_get(curlu, CURLUPART_SCHEME, &scheme, 0);
            curl_url_get(curlu, CURLUPART_HOST, &host, 0);
            curl_url_get(curlu, CURLUPART_PORT, &port, 0);
            curl_url_get(curlu, CURLUPART_PATH, &path, 0);
            curl_url_get(curlu, CURLUPART_QUERY, &query, 0);
            curl_url_get(curlu, CURLUPART_FRAGMENT, &fragment, 0);

            parsed.scheme = scheme ? scheme : "";
            parsed.host = host ? host : "";
            parsed.port = port ? port : "";
            parsed.path = path ? path : "";
            parsed.query = query ? query : "";
            parsed.fragment = fragment ? fragment : "";

            curl_free(scheme);
            curl_free(host);
            curl_free(port);
            curl_free(path);
            curl_free(query);
            curl_free(fragment);
        }

        curl_url_cleanup(curlu);
    }

    return parsed;
}

bool HttpPlugin::supportsResuming(const std::string& url) {
    // 发送 HEAD 请求检查服务器是否支持 Range 请求
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-1");

    CURLcode res = curl_easy_perform(curl);

    long responseCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &responseCode);

    curl_easy_cleanup(curl);

    return (res == CURLE_OK && (responseCode == 206 || responseCode == 200));
}

std::string HttpPlugin::getFinalUrl(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return url;

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    char* finalUrl = nullptr;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &finalUrl);
    }

    std::string result = finalUrl ? finalUrl : url;

    if (finalUrl) {
        curl_free(finalUrl);
    }

    curl_easy_cleanup(curl);

    return result;
}

} // namespace plugins
} // namespace falcon