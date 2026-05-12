/**
 * @file flashget_plugin.cpp
 * @brief 快车 FlashGet 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "flashget_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>
#include <regex>
#include <sstream>
#include <iomanip>
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
// FlashGetHandler 实现
//==============================================================================

FlashGetHandler::FlashGetHandler() {
    FALCON_LOG_INFO("FlashGet handler initialized");
}

FlashGetHandler::~FlashGetHandler() {
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

std::vector<std::string> FlashGetHandler::supported_schemes() const {
    return {"flashget", "fg"};
}

bool FlashGetHandler::can_handle(const std::string& url) const {
    return url.find("flashget://") == 0 || url.find("fg://") == 0;
}

FileInfo FlashGetHandler::get_file_info(const std::string& url,
                                        const DownloadOptions& options) {
    FileInfo info;
    info.url = url;
    info.supports_resume = true;

    try {
        std::string originalUrl = parseFlashGetUrl(url);
        info.filename = "download";
        info.total_size = 0;
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse FlashGet URL: {}", e.what());
        throw InvalidURLException("Invalid FlashGet URL: " + url);
    }

    return info;
}

void FlashGetHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    try {
        std::string originalUrl = parseFlashGetUrl(task->url());

        FALCON_LOG_INFO("Starting FlashGet download, resolved to: {}", originalUrl);

        auto ctx = std::make_shared<TaskContext>();
        ctx->task = task;
        ctx->listener = listener;
        ctx->decodedUrl = originalUrl;
        ctx->running.store(true);
        ctx->downloadedBytes = 0;

        {
            std::lock_guard<std::mutex> lock(tasksMutex_);
            activeTasks_[task->id()] = ctx;
        }

        ctx->downloadThread = std::thread([this, ctx]() {
            downloadThreadMain(ctx);
        });

        task->mark_started();

    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to start FlashGet download: {}", e.what());
        task->set_error(e.what());
        throw;
    }
}

void FlashGetHandler::pause(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(true);
        FALCON_LOG_DEBUG("FlashGet download paused: {}", task->id());
    }
}

void FlashGetHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(false);
        it->second->listener = listener;
        FALCON_LOG_DEBUG("FlashGet download resumed: {}", task->id());
    }
}

void FlashGetHandler::cancel(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->cancelled.store(true);
        it->second->running.store(false);

        if (it->second->downloadThread.joinable()) {
            it->second->downloadThread.join();
        }

        activeTasks_.erase(it);
        FALCON_LOG_INFO("FlashGet download cancelled: {}", task->id());
    }
}

//==============================================================================
// 私有方法实现
//==============================================================================

std::string FlashGetHandler::parseFlashGetUrl(const std::string& flashgetUrl) {
    std::regex flashgetRegex(R"(^(flashget|fg)://(.+)$)");
    std::smatch match;

    if (!std::regex_match(flashgetUrl, match, flashgetRegex)) {
        throw InvalidURLException("Invalid FlashGet URL format");
    }

    std::string protocol = match[1].str();
    std::string encoded = match[2].str();

    // 如果是短格式 fg://，直接返回URL
    if (protocol == "fg") {
        return urlDecode(encoded);
    }

    // 处理引用页面参数
    size_t refPos = encoded.find("&ref=");
    if (refPos != std::string::npos) {
        encoded = encoded.substr(0, refPos);
    }

    return decodeFlashGetUrl(encoded);
}

std::string FlashGetHandler::decodeFlashGetUrl(const std::string& encoded) {
    // 尝试直接解码（如果URL没有编码）
    if (encoded.find("://") != std::string::npos) {
        return urlDecode(encoded);
    }

    // 尝试Base64解码
    try {
        std::string decoded = base64_decode(encoded);

        // FlashGet的Base64可能有特殊前缀
        if (decoded.find("[FLASHGET]") == 0) {
            decoded = decoded.substr(10);
        }

        return urlDecode(decoded);

    } catch (const std::exception& e) {
        // 如果Base64解码失败，尝试直接URL解码
        try {
            return urlDecode(encoded);
        } catch (...) {
            throw InvalidURLException("Failed to decode FlashGet URL");
        }
    }
}

std::string FlashGetHandler::urlDecode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            int hex;
            std::istringstream iss(encoded.substr(i + 1, 2));
            if (iss >> std::hex >> hex) {
                decoded += static_cast<char>(hex);
                i += 2;
            } else {
                decoded += encoded[i];
            }
        } else if (encoded[i] == '+') {
            decoded += ' ';
        } else {
            decoded += encoded[i];
        }
    }
    return decoded;
}

std::string FlashGetHandler::base64_decode(const std::string& encoded) {
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

void FlashGetHandler::downloadThreadMain(std::shared_ptr<TaskContext> ctx) {
    FALCON_LOG_DEBUG("FlashGet download thread started for: {}", ctx->decodedUrl);

    // 委托给实际协议处理器下载
    delegateDownload(ctx);

    // 清理
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        activeTasks_.erase(ctx->task->id());
    }

    FALCON_LOG_DEBUG("FlashGet download thread ended");
}

void FlashGetHandler::delegateDownload(std::shared_ptr<TaskContext> ctx) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    if (!registry_) {
        FALCON_LOG_ERROR("ProtocolRegistry not set for FlashGet handler");
        ctx->task->set_error("ProtocolRegistry not available");
        return;
    }

    auto* targetHandler = registry_->get_handler_for_url(ctx->decodedUrl);
    if (!targetHandler) {
        FALCON_LOG_ERROR("No handler found for decoded URL: {}", ctx->decodedUrl);
        ctx->task->set_error("No handler for decoded URL: " + ctx->decodedUrl);
        return;
    }

    FALCON_LOG_INFO("Delegating FlashGet download to {} handler for: {}",
                   targetHandler->protocol_name(), ctx->decodedUrl);

    // 通知开始下载
    if (ctx->listener) {
        ctx->listener->on_progress(ctx->task->id(), 0, 0);
    }

    try {
        // 使用目标处理器进行下载
        targetHandler->download(ctx->task, ctx->listener);

        // 等待下载完成
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

std::unique_ptr<IProtocolHandler> create_flashget_handler() {
    return std::make_unique<FlashGetHandler>();
}

} // namespace protocols
} // namespace falcon
