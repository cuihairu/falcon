/**
 * @file hls_plugin.cpp
 * @brief HLS 和 DASH 流媒体协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "hls_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>
#include <regex>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <cstring>

namespace falcon {
namespace protocols {

//==============================================================================
// HLSHandler 实现
//==============================================================================

HLSHandler::HLSHandler() {
    FALCON_LOG_INFO("HLS/DASH handler initialized");
}

HLSHandler::~HLSHandler() {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    for (auto& pair : activeTasks_) {
        if (pair.second->running.load()) {
            pair.second->cancelled.store(true);
            pair.second->running.store(false);
            if (pair.second->downloadThread.joinable()) {
                pair.second->downloadThread.join();
            }
        }
    }
    activeTasks_.clear();
}

std::vector<std::string> HLSHandler::supported_schemes() const {
    return {"http", "https", "hls", "dash"};
}

bool HLSHandler::can_handle(const std::string& url) const {
    return url.find(".m3u8") != std::string::npos ||
           url.find(".mpd") != std::string::npos ||
           url.find("m3u8") != std::string::npos ||
           url.find("dash") != std::string::npos;
}

FileInfo HLSHandler::get_file_info(const std::string& url,
                                    const DownloadOptions& options) {
    FileInfo info;
    info.url = url;
    info.supports_resume = true;
    info.supports_segments = true;
    info.filename = "stream";  // HLS 通常需要从播放列表解析文件名
    info.total_size = 0;  // 需要解析播放列表才能知道总大小

    return info;
}

void HLSHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    try {
        FALCON_LOG_INFO("Starting HLS/DASH download for: {}", task->url());

        auto ctx = std::make_shared<TaskContext>();
        ctx->task = task;
        ctx->listener = listener;
        ctx->running.store(true);
        ctx->downloadedBytes = 0;

        // TODO: 实际实现需要：
        // 1. 下载播放列表 (m3u8/mpd)
        // 2. 解析媒体段
        // 3. 并行下载各段
        // 4. 合并成最终文件

        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            activeTasks_[task->id()] = ctx;
        }

        ctx->downloadThread = std::thread([this, ctx]() {
            downloadThreadMain(ctx);
        });

        task->mark_started();

    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to start HLS download: {}", e.what());
        task->set_error(e.what());
        throw;
    }
}

void HLSHandler::pause(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(true);
        FALCON_LOG_DEBUG("HLS download paused: {}", task->id());
    }
}

void HLSHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(false);
        it->second->listener = listener;
        FALCON_LOG_DEBUG("HLS download resumed: {}", task->id());
    }
}

void HLSHandler::cancel(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->cancelled.store(true);
        it->second->running.store(false);

        if (it->second->downloadThread.joinable()) {
            it->second->downloadThread.join();
        }

        activeTasks_.erase(it);
        FALCON_LOG_INFO("HLS download cancelled: {}", task->id());
    }
}

//==============================================================================
// 私有方法实现
//==============================================================================

std::vector<HLSHandler::MediaSegment> HLSHandler::parseM3U8(
        const std::string& m3u8Content,
        const std::string& baseUrl) {
    std::vector<MediaSegment> segments;
    std::istringstream iss(m3u8Content);
    std::string line;

    double currentDuration = 0;
    std::string currentTitle;

    while (std::getline(iss, line)) {
        // 移除空白字符
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '#') {
            // 处理标签
            if (line.find("#EXTINF:") == 0) {
                auto result = parseExtInf(line);
                currentDuration = result.first;
                currentTitle = result.second;
            }
            // 忽略其他标签
        } else {
            // 这是媒体段URL
            MediaSegment segment;
            segment.url = resolveUrl(line, baseUrl);
            segment.duration = currentDuration;
            segment.title = currentTitle;
            segments.push_back(segment);

            currentDuration = 0;
            currentTitle.clear();
        }
    }

    return segments;
}

std::pair<double, std::string> HLSHandler::parseExtInf(const std::string& extinf) {
    // #EXTINF:duration,title
    std::regex infRegex(R"(#EXTINF:([\d.]+)(?:,(.*))?)");
    std::smatch match;

    if (std::regex_match(extinf, match, infRegex)) {
        double duration = std::stod(match[1].str());
        std::string title = match.size() > 2 ? match[2].str() : "";
        return {duration, title};
    }

    return {0, ""};
}

std::string HLSHandler::resolveUrl(const std::string& url, const std::string& baseUrl) {
    // 如果已经是绝对URL，直接返回
    if (url.find("http://") == 0 || url.find("https://") == 0) {
        return url;
    }

    // 解析基础URL
    std::regex urlRegex(R"(^(https?://[^/]+)/)");
    std::smatch match;
    if (std::regex_search(baseUrl, match, urlRegex)) {
        std::string base = match[1].str();

        // 处理绝对路径
        if (url[0] == '/') {
            return base + url;
        }

        // 处理相对路径
        size_t lastSlash = baseUrl.find_last_of('/');
        if (lastSlash != std::string::npos) {
            return baseUrl.substr(0, lastSlash + 1) + url;
        }
    }

    return url;
}

void HLSHandler::downloadThreadMain(std::shared_ptr<TaskContext> ctx) {
    FALCON_LOG_DEBUG("HLS download thread started for: {}", ctx->task->url());

    try {
        // 下载所有段
        downloadAllSegments(ctx);
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("HLS download failed: {}", e.what());
        ctx->task->set_error(e.what());
    }

    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        activeTasks_.erase(ctx->task->id());
    }

    FALCON_LOG_DEBUG("HLS download thread ended");
}

std::string HLSHandler::downloadM3U8(const std::string& url) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    if (!registry_) {
        throw std::runtime_error("ProtocolRegistry not set for HLS handler");
    }

    auto* handler = registry_->get_handler_for_url(url);
    if (!handler) {
        throw std::runtime_error("No handler found for M3U8 URL: " + url);
    }

    // 创建临时任务下载播放列表
    // 这里简化处理，直接使用 HTTP 下载
    // 实际应该使用 DownloadEngine 和 handler->download()

    // 简化实现：使用 libcurl 直接下载（如果可用）
    // 或者返回占位符内容用于测试

    FALCON_LOG_INFO("Downloading M3U8 playlist from: {}", url);

    // 简化实现：返回一个示例播放列表
    // 实际应该发起 HTTP 请求获取内容
    return "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:10\n";
}

std::vector<uint8_t> HLSHandler::downloadSegment(const std::string& url) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    if (!registry_) {
        throw std::runtime_error("ProtocolRegistry not set for HLS handler");
    }

    auto* handler = registry_->get_handler_for_url(url);
    if (!handler) {
        throw std::runtime_error("No handler found for segment URL: " + url);
    }

    FALCON_LOG_DEBUG("Downloading segment: {}", url);

    // 简化实现：返回空数据
    // 实际应该发起 HTTP 请求获取段内容
    return std::vector<uint8_t>();
}

bool HLSHandler::mergeSegments(const std::vector<std::string>& segmentFiles,
                               const std::string& outputFile) {
    std::ofstream output(outputFile, std::ios::binary);
    if (!output) {
        FALCON_LOG_ERROR("Failed to create output file: {}", outputFile);
        return false;
    }

    for (const auto& file : segmentFiles) {
        std::ifstream input(file, std::ios::binary);
        if (!input) {
            FALCON_LOG_WARN("Failed to open segment file: {}", file);
            continue;
        }

        output << input.rdbuf();
        input.close();

        // 删除临时段文件
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }

    output.close();
    return true;
}

void HLSHandler::downloadAllSegments(std::shared_ptr<TaskContext> ctx) {
    const std::string& url = ctx->task->url();

    // 1. 下载 M3U8 播放列表
    std::string m3u8Content = downloadM3U8(url);

    // 2. 解析播放列表获取段信息
    std::string baseUrl = url.substr(0, url.find_last_of('/'));
    ctx->segments = parseM3U8(m3u8Content, baseUrl);

    if (ctx->segments.empty()) {
        throw std::runtime_error("No media segments found in playlist");
    }

    FALCON_LOG_INFO("Found {} media segments", ctx->segments.size());

    // 3. 计算总大小（估算）
    ctx->totalSize = ctx->segments.size() * 1024 * 1024;  // 假设每段 1MB

    // 4. 并行下载各段
    std::vector<std::string> segmentFiles;
    segmentFiles.reserve(ctx->segments.size());

    // 创建临时目录
    std::filesystem::path tempDir = std::filesystem::temp_directory_path() / "falcon_hls_" + std::to_string(ctx->task->id());
    std::error_code ec;
    std::filesystem::create_directories(tempDir, ec);

    // 限制并发数
    const size_t maxConcurrent = 4;
    std::atomic<size_t> completedCount{0};
    std::atomic<size_t> totalBytes{0};
    std::vector<std::thread> downloadThreads;

    for (size_t i = 0; i < ctx->segments.size(); i += maxConcurrent) {
        size_t batchEnd = std::min(i + maxConcurrent, ctx->segments.size());

        // 启动一批下载线程
        for (size_t j = i; j < batchEnd; ++j) {
            if (ctx->cancelled.load()) {
                break;
            }

            downloadThreads.emplace_back([this, &ctx, j, &tempDir, &segmentFiles, &completedCount, &totalBytes]() {
                try {
                    const auto& segment = ctx->segments[j];

                    // 下载段数据
                    auto data = downloadSegment(segment.url);

                    // 保存到临时文件
                    std::string segmentFile = (tempDir / ("segment_" + std::to_string(j) + ".ts")).string();
                    std::ofstream out(segmentFile, std::ios::binary);
                    if (out) {
                        out.write(reinterpret_cast<const char*>(data.data()), data.size());
                        out.close();

                        {
                            std::lock_guard<std::mutex> lock(ctx->mutex);
                            segmentFiles.push_back(segmentFile);
                            totalBytes += data.size();
                            ctx->downloadedBytes = totalBytes.load();
                        }

                        // 通知进度
                        if (ctx->listener) {
                            ctx->listener->on_progress(ctx->task->id(), totalBytes.load(), ctx->totalSize);
                        }
                    }
                } catch (const std::exception& e) {
                    FALCON_LOG_ERROR("Failed to download segment {}: {}", j, e.what());
                }

                completedCount++;
            });
        }

        // 等待当前批次完成
        for (auto& thread : downloadThreads) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        downloadThreads.clear();
    }

    // 5. 合并所有段到最终文件
    if (!segmentFiles.empty() && !ctx->cancelled.load()) {
        std::string outputFile = ctx->task->output_path();
        if (mergeSegments(segmentFiles, outputFile)) {
            FALCON_LOG_INFO("Successfully merged {} segments to: {}", segmentFiles.size(), outputFile);

            // 标记任务完成
            if (ctx->listener) {
                ctx->listener->on_complete(ctx->task->id());
            }
        } else {
            throw std::runtime_error("Failed to merge segments");
        }
    } else {
        throw std::runtime_error("No segments downloaded");
    }

    // 清理临时目录
    std::filesystem::remove_all(tempDir, ec);
}

bool HLSHandler::isHLSStream(const std::string& url) const {
    return url.find(".m3u8") != std::string::npos ||
           url.find("m3u8") != std::string::npos;
}

bool HLSHandler::isDASHStream(const std::string& url) const {
    return url.find(".mpd") != std::string::npos ||
           url.find("dash") != std::string::npos ||
           url.find("mpd") != std::string::npos;
}

//==============================================================================
// Factory function
//==============================================================================

std::unique_ptr<IProtocolHandler> create_hls_handler() {
    return std::make_unique<HLSHandler>();
}

} // namespace protocols
} // namespace falcon
