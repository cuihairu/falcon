/**
 * @file qqdl_plugin.cpp
 * @brief QQ旋风 QQDL 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "qqdl_plugin.hpp"
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
// QQDLHandler 实现
//==============================================================================

QQDLHandler::QQDLHandler() {
    FALCON_LOG_INFO("QQDL handler initialized");
}

QQDLHandler::~QQDLHandler() {
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

std::vector<std::string> QQDLHandler::supported_schemes() const {
    return {"qqlink", "qqdl"};
}

bool QQDLHandler::can_handle(const std::string& url) const {
    return url.find("qqlink://") == 0 || url.find("qqdl://") == 0;
}

FileInfo QQDLHandler::get_file_info(const std::string& url,
                                    const DownloadOptions& options) {
    FileInfo info;
    info.url = url;
    info.supports_resume = true;

    try {
        std::string originalUrl = parseQQUrl(url);
        info.filename = "download";
        info.total_size = 0;
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse QQDL URL: {}", e.what());
        throw InvalidURLException("Invalid QQDL URL: " + url);
    }

    return info;
}

void QQDLHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    try {
        std::string originalUrl = parseQQUrl(task->url());

        FALCON_LOG_INFO("Starting QQDL download, resolved to: {}", originalUrl);

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
        FALCON_LOG_ERROR("Failed to start QQDL download: {}", e.what());
        task->set_error(e.what());
        throw;
    }
}

void QQDLHandler::pause(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(true);
        FALCON_LOG_DEBUG("QQDL download paused: {}", task->id());
    }
}

void QQDLHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(false);
        it->second->listener = listener;
        FALCON_LOG_DEBUG("QQDL download resumed: {}", task->id());
    }
}

void QQDLHandler::cancel(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->cancelled.store(true);
        it->second->running.store(false);

        if (it->second->downloadThread.joinable()) {
            it->second->downloadThread.join();
        }

        activeTasks_.erase(it);
        FALCON_LOG_INFO("QQDL download cancelled: {}", task->id());
    }
}

//==============================================================================
// 私有方法实现
//==============================================================================

std::string QQDLHandler::parseQQUrl(const std::string& qqUrl) {
    std::regex qqRegex(R"(^(qqlink|qqdl)://(.+)$)");
    std::smatch match;

    if (!std::regex_match(qqUrl, match, qqRegex)) {
        throw InvalidURLException("Invalid QQDL URL format");
    }

    std::string encoded = match[2].str();
    return decodeQQUrl(encoded);
}

std::string QQDLHandler::decodeQQUrl(const std::string& encoded) {
    // QQ旋风链接通常有以下几种格式：
    // 1. qqlink://[Base64编码的URL]
    // 2. qqlink://[GID]|[文件信息]
    // 3. qqdl://[特殊编码]

    // 检查是否包含GID分隔符
    if (encoded.find('|') != std::string::npos) {
        // GID格式解析
        std::istringstream iss(encoded);
        std::string fileInfo;

        // 跳过GID，获取文件信息
        std::getline(iss, fileInfo, '|');
        std::getline(iss, fileInfo);

        // 简化处理：假设第一个字段是URL
        std::istringstream fis(fileInfo);
        std::string url;
        std::getline(fis, url, '|');
        return url;
    } else {
        // Base64编码格式
        try {
            std::string decoded = base64_decode(encoded);

            // 检查是否是有效的URL
            if (decoded.find("http://") == 0 || decoded.find("https://") == 0 ||
                decoded.find("ftp://") == 0) {
                return decoded;
            }

            // 可能包含多个字段，取第一个作为URL
            std::istringstream iss(decoded);
            std::string url;
            std::getline(iss, url, '|');
            if (url.find("://") != std::string::npos) {
                return url;
            }

            throw InvalidURLException("Cannot extract URL from decoded data");

        } catch (const std::exception& e) {
            throw InvalidURLException("Failed to decode QQDL URL: " + std::string(e.what()));
        }
    }
}

std::string QQDLHandler::base64_decode(const std::string& encoded) {
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

void QQDLHandler::downloadThreadMain(std::shared_ptr<TaskContext> ctx) {
    FALCON_LOG_DEBUG("QQDL download thread started for: {}", ctx->decodedUrl);

    // 委托给实际协议处理器下载
    delegateDownload(ctx);

    // 清理
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        activeTasks_.erase(ctx->task->id());
    }

    FALCON_LOG_DEBUG("QQDL download thread ended");
}

void QQDLHandler::delegateDownload(std::shared_ptr<TaskContext> ctx) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    if (!registry_) {
        FALCON_LOG_ERROR("ProtocolRegistry not set for QQDL handler");
        ctx->task->set_error("ProtocolRegistry not available");
        return;
    }

    auto* targetHandler = registry_->get_handler_for_url(ctx->decodedUrl);
    if (!targetHandler) {
        FALCON_LOG_ERROR("No handler found for decoded URL: {}", ctx->decodedUrl);
        ctx->task->set_error("No handler for decoded URL: " + ctx->decodedUrl);
        return;
    }

    FALCON_LOG_INFO("Delegating QQDL download to {} handler for: {}",
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

std::unique_ptr<IProtocolHandler> create_qqdl_handler() {
    return std::make_unique<QQDLHandler>();
}

} // namespace protocols
} // namespace falcon
