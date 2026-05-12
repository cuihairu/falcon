/**
 * @file ed2k_plugin.cpp
 * @brief ED2K (eDonkey2000) 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "ed2k_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_registry.hpp>
#include <regex>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>
#include <chrono>

namespace falcon {
namespace protocols {

//==============================================================================
// ED2KHandler 实现
//==============================================================================

ED2KHandler::ED2KHandler() {
    FALCON_LOG_INFO("ED2K handler initialized");
}

ED2KHandler::~ED2KHandler() {
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

std::vector<std::string> ED2KHandler::supported_schemes() const {
    return {"ed2k"};
}

bool ED2KHandler::can_handle(const std::string& url) const {
    return url.find("ed2k://") == 0;
}

FileInfo ED2KHandler::get_file_info(const std::string& url,
                                    const DownloadOptions& options) {
    FileInfo info;
    info.url = url;
    info.supports_resume = true;

    try {
        ED2KFileInfo fileInfo = parseED2KUrl(url);
        info.filename = fileInfo.filename;
        info.total_size = fileInfo.filesize;
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse ED2K URL: {}", e.what());
        throw InvalidURLException("Invalid ED2K URL: " + url);
    }

    return info;
}

void ED2KHandler::download(DownloadTask::Ptr task, IEventListener* listener) {
    try {
        // 解析 ED2K 链接
        ED2KFileInfo fileInfo = parseED2KUrl(task->url());

        FALCON_LOG_INFO("Starting ED2K download: {} ({} bytes)",
                       fileInfo.filename, fileInfo.filesize);

        // 创建任务上下文
        auto ctx = std::make_shared<TaskContext>();
        ctx->task = task;
        ctx->listener = listener;
        ctx->fileInfo = std::move(fileInfo);
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
        FALCON_LOG_ERROR("Failed to start ED2K download: {}", e.what());
        task->set_error(e.what());
        throw;
    }
}

void ED2KHandler::pause(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(true);
        FALCON_LOG_DEBUG("ED2K download paused: {}", task->id());
    }
}

void ED2KHandler::resume(DownloadTask::Ptr task, IEventListener* listener) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->paused.store(false);
        it->second->listener = listener;
        FALCON_LOG_DEBUG("ED2K download resumed: {}", task->id());
    }
}

void ED2KHandler::cancel(DownloadTask::Ptr task) {
    std::lock_guard<std::mutex> lock(tasksMutex_);
    auto it = activeTasks_.find(task->id());
    if (it != activeTasks_.end()) {
        it->second->cancelled.store(true);
        it->second->running.store(false);

        if (it->second->downloadThread.joinable()) {
            it->second->downloadThread.join();
        }

        activeTasks_.erase(it);
        FALCON_LOG_INFO("ED2K download cancelled: {}", task->id());
    }
}

//==============================================================================
// 私有方法实现
//==============================================================================

ED2KHandler::ED2KFileInfo ED2KHandler::parseED2KUrl(const std::string& ed2kUrl) {
    std::regex ed2kRegex(R"(^ed2k://(\w+)\|(.+))");
    std::smatch match;

    if (!std::regex_match(ed2kUrl, match, ed2kRegex)) {
        throw InvalidURLException("Invalid ED2K URL format");
    }

    std::string type = match[1].str();
    std::string paramsStr = match[2].str();

    // 分割参数
    std::vector<std::string> params;
    std::istringstream iss(paramsStr);
    std::string param;
    while (std::getline(iss, param, '|')) {
        params.push_back(param);
    }

    if (type == "file") {
        return parseFileLink(params);
    } else if (type == "server") {
        throw UnsupportedProtocolException("ED2K server links not supported in this context");
    } else {
        throw InvalidURLException("Unsupported ED2K link type: " + type);
    }
}

ED2KHandler::ED2KFileInfo ED2KHandler::parseFileLink(const std::vector<std::string>& params) {
    if (params.size() < 4) {
        throw InvalidURLException("Insufficient ED2K file link parameters");
    }

    ED2KFileInfo info;

    // 解析文件名
    info.filename = urlDecode(params[0]);

    // 解析文件大小
    try {
        info.filesize = std::stoull(params[1]);
    } catch (const std::exception& e) {
        throw InvalidURLException("Invalid file size in ED2K link");
    }

    // 解析MD4哈希
    info.hash = parseMD4Hash(params[2]);

    // 解析其他可选参数
    for (size_t i = 3; i < params.size(); ++i) {
        const std::string& param = params[i];

        if (param.find("s=") == 0) {
            // 源地址
            std::string sources = param.substr(2);
            info.sources = parseSources(sources);
        } else if (param.find("h=") == 0) {
            // AICH哈希
            info.aich = param.substr(2);
        } else if (param.find("p=") == 0) {
            // 优先级
            try {
                info.priority = std::stoul(param.substr(2));
            } catch (...) {
                info.priority = 0;  // 默认优先级
            }
        }
    }

    return info;
}

ED2KHandler::ServerInfo ED2KHandler::parseServerLink(const std::vector<std::string>& params) {
    ServerInfo info;

    if (params.size() < 2) {
        throw InvalidURLException("Insufficient ED2K server link parameters");
    }

    // 解析主机和端口
    size_t colonPos = params[0].find(':');
    if (colonPos == std::string::npos) {
        throw InvalidURLException("Invalid server format, expected host:port");
    }

    info.host = params[0].substr(0, colonPos);
    try {
        info.port = static_cast<uint16_t>(std::stoul(params[0].substr(colonPos + 1)));
    } catch (...) {
        throw InvalidURLException("Invalid server port");
    }

    // 解析服务器名称（可选）
    if (params.size() > 2) {
        info.name = urlDecode(params[1]);
    }

    return info;
}

std::string ED2KHandler::urlDecode(const std::string& encoded) {
    std::string decoded;
    for (size_t i = 0; i < encoded.length(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.length()) {
            // 解码 %XX 格式
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

std::array<uint8_t, 16> ED2KHandler::parseMD4Hash(const std::string& hashStr) {
    if (hashStr.length() != 32) {
        throw InvalidURLException("Invalid ED2K hash length");
    }

    std::array<uint8_t, 16> hash;
    for (size_t i = 0; i < 16; ++i) {
        std::string byteStr = hashStr.substr(i * 2, 2);
        try {
            hash[i] = static_cast<uint8_t>(std::stoul(byteStr, nullptr, 16));
        } catch (...) {
            throw InvalidURLException("Invalid ED2K hash format");
        }
    }

    return hash;
}

std::vector<std::string> ED2KHandler::parseSources(const std::string& sourcesStr) {
    std::vector<std::string> sources;

    // ED2K源格式：host:port|host:port|...
    std::istringstream iss(sourcesStr);
    std::string source;
    while (std::getline(iss, source, '|')) {
        // 验证源格式
        size_t colonPos = source.find(':');
        if (colonPos != std::string::npos) {
            // 简单验证端口号
            std::string portStr = source.substr(colonPos + 1);
            try {
                auto port = std::stoul(portStr);
                if (port > 0 && port <= 65535) {
                    sources.push_back(source);
                }
            } catch (...) {
                // 忽略无效源
            }
        }
    }

    return sources;
}

std::string ED2KHandler::urlEncode(const std::string& str) {
    std::string encoded;
    for (char c : str) {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            encoded += '%';
            encoded += "0123456789ABCDEF"[static_cast<unsigned char>(c) >> 4];
            encoded += "0123456789ABCDEF"[static_cast<unsigned char>(c) & 0x0F];
        }
    }
    return encoded;
}

std::string ED2KHandler::hashToString(const std::array<uint8_t, 16>& hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

void ED2KHandler::downloadThreadMain(std::shared_ptr<TaskContext> ctx) {
    FALCON_LOG_DEBUG("ED2K download thread started for: {}", ctx->fileInfo.filename);

    auto& fileInfo = ctx->fileInfo;

    // 通知开始下载
    if (ctx->listener) {
        ctx->listener->on_progress(ctx->task->id(),
                                   ctx->downloadedBytes,
                                   fileInfo.filesize);
    }

    // 尝试从源地址下载
    if (!fileInfo.sources.empty()) {
        downloadFromSources(ctx);
    } else {
        FALCON_LOG_WARN("ED2K link has no source addresses, attempting direct download if URL available");

        // 如果 ED2K 链接中包含 HTTP 源，尝试直接下载
        // 否则提示用户需要源地址
        FALCON_LOG_ERROR("ED2K download requires source addresses or eDonkey network connection");
        ctx->task->set_error("No source addresses available in ED2K link");
    }

    // 清理
    {
        std::lock_guard<std::mutex> lock(tasksMutex_);
        activeTasks_.erase(ctx->task->id());
    }

    FALCON_LOG_DEBUG("ED2K download thread ended for: {}", fileInfo.filename);
}

void ED2KHandler::downloadFromSources(std::shared_ptr<TaskContext> ctx) {
    auto& fileInfo = ctx->fileInfo;

    FALCON_LOG_INFO("Attempting to download from {} ED2K sources", fileInfo.sources.size());

    // 尝试从每个源下载
    for (const auto& source : fileInfo.sources) {
        if (ctx->cancelled.load()) {
            break;
        }

        // ED2K 源格式通常是 host:port
        // 需要构造 HTTP URL（这需要知道服务器的 HTTP 端口配置）
        // 简化实现：假设源服务器提供 HTTP 访问
        std::string httpUrl = "http://" + source + "/get/" + hashToString(fileInfo.hash);

        FALCON_LOG_DEBUG("Trying source: {}", httpUrl);

        delegateDownload(ctx, httpUrl);

        // 检查是否成功
        if (ctx->task->status() == TaskStatus::Completed) {
            FALCON_LOG_INFO("Successfully downloaded from source: {}", source);
            break;
        }
    }
}

void ED2KHandler::delegateDownload(std::shared_ptr<TaskContext> ctx, const std::string& url) {
    std::lock_guard<std::mutex> lock(registryMutex_);

    if (!registry_) {
        FALCON_LOG_ERROR("ProtocolRegistry not set for ED2K handler");
        return;
    }

    auto* targetHandler = registry_->get_handler_for_url(url);
    if (!targetHandler) {
        FALCON_LOG_ERROR("No handler found for URL: {}", url);
        return;
    }

    FALCON_LOG_INFO("Delegating ED2K download to {} handler for: {}",
                   targetHandler->protocol_name(), url);

    try {
        // 使用目标处理器进行下载
        targetHandler->download(ctx->task, ctx->listener);

        // 等待下载完成
        int waitCount = 0;
        while (ctx->running.load() && !ctx->cancelled.load() && waitCount < 30000) {
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
    }
}

//==============================================================================
// Factory function
//==============================================================================

std::unique_ptr<IProtocolHandler> create_ed2k_handler() {
    return std::make_unique<ED2KHandler>();
}

} // namespace protocols
} // namespace falcon
