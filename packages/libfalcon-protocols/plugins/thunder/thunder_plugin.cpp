/**
 * @file thunder_plugin.cpp
 * @brief 迅雷 Thunder 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "thunder_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>
#include <regex>
#include <sstream>
#include <iomanip>
#include <vector>
#include <chrono>

#ifndef OPENSSL_IS_BORINGSSL
#include <openssl/bio.h>
#include <openssl/evp.h>
#else
#include <boringssl/bio.h>
#include <boringssl/evp.h>
#endif

namespace falcon {
namespace protocols {

//==============================================================================
// ThunderHandler 实现
//==============================================================================

ThunderHandler::ThunderHandler() {
    FALCON_LOG_INFO("Thunder handler initialized");
}

ThunderHandler::~ThunderHandler() {
    // 停止所有活动任务
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

std::vector<std::string> ThunderHandler::supported_schemes() const {
    return {"thunder", "thunderxl"};
}

bool ThunderHandler::can_handle(const std::string& url) const {
    return url.find("thunder://") == 0 || url.find("thunderxl://") == 0;
}

FileInfo ThunderHandler::get_file_info(const std::string& url,
                                        const DownloadOptions& options) {
    FileInfo info;
    info.url = url;
    info.supports_resume = true;

    try {
        std::string originalUrl = parseThunderUrl(url);
        info.filename = "download";  // 无法从迅雷链接直接获取文件名
        info.total_size = 0;  // 需要实际请求才能获取大小
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse thunder URL: {}", e.what());
        throw InvalidURLException("Invalid thunder URL: " + url);
    }

    return info;
}

void ThunderHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    try {
        // 解析迅雷链接
        std::string originalUrl = parseThunderUrl(task->url());

        FALCON_LOG_INFO("Starting thunder download, resolved to: {}", originalUrl);

        // 创建任务上下文
        auto ctx = std::make_shared<TaskContext>();
        ctx->task = task;
        ctx->listener = listener;
        ctx->decodedUrl = originalUrl;
        ctx->running.store(true);
        ctx->downloadedBytes = 0;

        // 保存任务上下文
        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            activeTasks_[task->id()] = ctx;
        }

        // 启动下载线程
        ctx->downloadThread = std::thread([this, ctx]() {
            downloadThreadMain(ctx);
        });

        // 标记任务开始
        task->mark_started();

    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to start thunder download: {}", e.what());
        task->set_error(e.what());
        throw;
    }
}

void ThunderHandler::pause(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(true);
        FALCON_LOG_DEBUG("Thunder download paused: {}", task->id());
    }
}

void ThunderHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(false);
        it->second->listener = listener;
        FALCON_LOG_DEBUG("Thunder download resumed: {}", task->id());
    }
}

void ThunderHandler::cancel(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->cancelled.store(true);
        it->second->running.store(false);

        if (it->second->downloadThread.joinable()) {
            it->second->downloadThread.join();
        }

        activeTasks_.erase(it);
        FALCON_LOG_INFO("Thunder download cancelled: {}", task->id());
    }
}

//==============================================================================
// 私有方法实现
//==============================================================================

std::string ThunderHandler::parseThunderUrl(const std::string& thunderUrl) {
    std::regex thunderRegex(R"(^(thunder|thunderxl)://(.+)$)");
    std::smatch match;

    if (!std::regex_match(thunderUrl, match, thunderRegex)) {
        throw InvalidURLException("Invalid thunder URL format");
    }

    std::string protocol = match[1].str();
    std::string encoded = match[2].str();

    if (protocol == "thunder") {
        return decodeClassicThunder(encoded);
    } else if (protocol == "thunderxl") {
        return decodeThunderXL(encoded);
    }

    throw InvalidURLException("Unknown thunder protocol: " + protocol);
}

std::string ThunderHandler::decodeClassicThunder(const std::string& encoded) {
    // 经典迅雷链接格式：thunder://[Base64编码后的URL]
    // Base64内容通常是：AA[原始URL]ZZ

    // Base64解码
    std::string decoded;
    try {
        decoded = base64_decode(encoded);
    } catch (const std::exception& e) {
        throw InvalidURLException("Failed to decode Base64: " + std::string(e.what()));
    }

    // 检查并移除 AA 和 ZZ 标记
    if (decoded.size() < 2) {
        throw InvalidURLException("Invalid decoded data length");
    }

    // 移除开头的 AA
    if (decoded.substr(0, 2) == "AA") {
        decoded = decoded.substr(2);
    }

    // 移除结尾的 ZZ
    if (decoded.size() >= 2 && decoded.substr(decoded.size() - 2) == "ZZ") {
        decoded = decoded.substr(0, decoded.size() - 2);
    }

    if (decoded.empty()) {
        throw InvalidURLException("Empty URL after decoding");
    }

    if (!isValidUrl(decoded)) {
        throw InvalidURLException("Invalid decoded URL: " + decoded);
    }

    return decoded;
}

std::string ThunderHandler::decodeThunderXL(const std::string& encoded) {
    // 迅雷离线链接格式更复杂，可能包含额外的信息
    // 这里简化处理，实际实现需要根据迅雷的离线协议文档

    try {
        std::string decoded = base64_decode(encoded);

        // XL链接可能有特殊格式，这里做基础解析
        if (decoded.find("http://") == 0 || decoded.find("https://") == 0 ||
            decoded.find("ftp://") == 0 || decoded.find("magnet:") == 0) {
            return decoded;
        }

        // 如果不是直接链接，可能是需要进一步解析的格式
        throw UnsupportedProtocolException("Thunder XL complex format not fully supported");

    } catch (const std::exception& e) {
        throw InvalidURLException("Failed to decode thunder XL URL: " + std::string(e.what()));
    }
}

bool ThunderHandler::isValidUrl(const std::string& url) {
    // 简单的URL验证
    std::regex urlRegex(R"(^(https?|ftp|magnet|ed2k)://[^\s/$.?#].[^\s]*$)");
    return std::regex_match(url, urlRegex);
}

std::string ThunderHandler::base64_decode(const std::string& encoded) {
    // 计算解码后的长度
    size_t len = encoded.size();
    size_t padding = 0;

    if (len > 0 && encoded[len - 1] == '=') {
        padding++;
        if (len > 1 && encoded[len - 2] == '=') {
            padding++;
        }
    }

    size_t decoded_len = (len * 3) / 4 - padding;
    std::string decoded;
    decoded.resize(decoded_len);

    // 使用 OpenSSL 进行 Base64 解码
    BIO* bio = BIO_new_mem_buf(encoded.c_str(), static_cast<int>(encoded.size()));
    BIO* b64 = BIO_new(BIO_f_base64());

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    int actual_len = BIO_read(bio, &decoded[0], static_cast<int>(encoded.size()));

    BIO_free_all(bio);

    if (actual_len <= 0) {
        throw std::runtime_error("Base64 decode failed");
    }

    decoded.resize(actual_len);
    return decoded;
}

void ThunderHandler::downloadThreadMain(std::shared_ptr<TaskContext> ctx) {
    FALCON_LOG_DEBUG("Thunder download thread started for: {}", ctx->decodedUrl);

    // 委托给实际协议处理器下载
    delegateDownload(ctx);

    // 清理
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        activeTasks_.erase(ctx->task->id());
    }

    FALCON_LOG_DEBUG("Thunder download thread ended");
}

void ThunderHandler::delegateDownload(std::shared_ptr<TaskContext> ctx) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    if (!registry_) {
        FALCON_LOG_ERROR("ProtocolRegistry not set for Thunder handler");
        ctx->task->set_error("ProtocolRegistry not available");
        return;
    }

    // 获取实际协议处理器
    auto* targetHandler = registry_->get_handler_for_url(ctx->decodedUrl);
    if (!targetHandler) {
        FALCON_LOG_ERROR("No handler found for decoded URL: {}", ctx->decodedUrl);
        ctx->task->set_error("No handler for decoded URL: " + ctx->decodedUrl);
        return;
    }

    FALCON_LOG_INFO("Delegating Thunder download to {} handler for: {}",
                   targetHandler->protocol_name(), ctx->decodedUrl);

    // 通知开始下载
    if (ctx->listener) {
        ctx->listener->on_progress(ctx->task->id(), 0, 0);
    }

    try {
        // 使用目标处理器进行下载
        targetHandler->download(ctx->task, ctx->listener);

        // 等待下载完成（简化的等待逻辑）
        int waitCount = 0;
        while (ctx->running.load() && !ctx->cancelled.load() && waitCount < 60000) {
            auto status = ctx->task->status();
            if (status == TaskStatus::Completed || status == TaskStatus::Failed ||
                status == TaskStatus::Cancelled) {
                break;
            }

            if (ctx->paused.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ++waitCount;

            // 定期通知进度
            if (waitCount % 10 == 0 && ctx->listener) {
                ctx->listener->on_progress(ctx->task->id(),
                                          ctx->task->downloaded_bytes(),
                                          ctx->task->total_size());
            }
        }
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Delegated download failed: {}", e.what());
        ctx->task->set_error(e.what());
    }
}

//==============================================================================
// Factory function
//==============================================================================

std::unique_ptr<IProtocolHandler> create_thunder_handler() {
    return std::make_unique<ThunderHandler>();
}

} // namespace protocols
} // namespace falcon
