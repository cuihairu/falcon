/**
 * @file ed2k_plugin.cpp
 * @brief ED2K (eDonkey2000) 协议插件实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include "ed2k_plugin.hpp"
#include <falcon/logger.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/download_task.hpp>
#include <regex>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cstring>

namespace falcon {
namespace plugins {

ED2KPlugin::ED2KPlugin() {
    FALCON_LOG_INFO("ED2K plugin initialized");
}

std::vector<std::string> ED2KPlugin::getSupportedSchemes() const {
    return {"ed2k"};
}

bool ED2KPlugin::canHandle(const std::string& url) const {
    return url.find("ed2k://") == 0;
}

std::unique_ptr<IDownloadTask> ED2KPlugin::createTask(const std::string& url,
                                                      const DownloadOptions& options) {
    FALCON_LOG_DEBUG("Creating ED2K task for: {}", url);

    // 解析ED2K链接
    ED2KFileInfo fileInfo;
    try {
        fileInfo = parseED2KUrl(url);
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("Failed to parse ED2K URL: {}", e.what());
        throw InvalidURLException("Invalid ED2K URL: " + url);
    }

    FALCON_LOG_DEBUG("ED2K file: {} ({} bytes)", fileInfo.filename, fileInfo.filesize);

    // 创建下载任务
    return createDownloadTask(fileInfo, options);
}

ED2KPlugin::ED2KFileInfo ED2KPlugin::parseED2KUrl(const std::string& ed2kUrl) {
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

ED2KPlugin::ED2KFileInfo ED2KPlugin::parseFileLink(const std::vector<std::string>& params) {
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

ED2KPlugin::ServerInfo ED2KPlugin::parseServerLink(const std::vector<std::string>& params) {
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

std::string ED2KPlugin::urlDecode(const std::string& encoded) {
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

std::array<uint8_t, 16> ED2KPlugin::parseMD4Hash(const std::string& hashStr) {
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

std::vector<std::string> ED2KPlugin::parseSources(const std::string& sourcesStr) {
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

void* ED2KPlugin::connectToNetwork(const std::vector<ServerInfo>& servers) {
    // 实际实现需要连接到ED2K网络
    // 这里是简化实现
    FALCON_LOG_DEBUG("Connecting to ED2K network with {} servers", servers.size());

    // 返回连接句柄（实际实现中应该是网络连接对象）
    return nullptr;
}

std::vector<std::string> ED2KPlugin::searchSources(const ED2KFileInfo& fileInfo) {
    // 实际实现需要搜索ED2K网络中的源
    FALCON_LOG_DEBUG("Searching for sources of file: {}", fileInfo.filename);

    std::vector<std::string> allSources = fileInfo.sources;

    // 如果已有源，添加到列表
    if (!allSources.empty()) {
        FALCON_LOG_DEBUG("Found {} direct sources", allSources.size());
    }

    // 搜索更多源（实际实现需要网络搜索）
    // 这里只是示例

    return allSources;
}

std::unique_ptr<IDownloadTask> ED2KPlugin::createDownloadTask(const ED2KFileInfo& fileInfo,
                                                             const DownloadOptions& options) {
    // 创建下载任务
    auto task = std::make_unique<DownloadTask>();

    // 设置任务信息
    task->setUrl("ed2k://file|" + urlEncode(fileInfo.filename) + "|" +
                 std::to_string(fileInfo.filesize) + "|" +
                 hashToString(fileInfo.hash));

    task->setFilename(fileInfo.filename);
    task->setFileSize(fileInfo.filesize);

    // ED2K下载需要特殊的处理逻辑
    // 这里简化为使用多个HTTP源（如果有的话）
    auto sources = searchSources(fileInfo);

    if (!sources.empty()) {
        FALCON_LOG_INFO("Using {} sources for ED2K download", sources.size());
        // 实际实现中需要创建ED2K专用的下载逻辑
    } else {
        FALCON_LOG_WARN("No sources found for ED2K download, will search network");
    }

    return std::move(task);
}

std::string ED2KPlugin::urlEncode(const std::string& str) {
    std::string encoded;
    for (char c : str) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            encoded += '%';
            encoded += "0123456789ABCDEF"[static_cast<unsigned char>(c) >> 4];
            encoded += "0123456789ABCDEF"[static_cast<unsigned char>(c) & 0x0F];
        }
    }
    return encoded;
}

std::string ED2KPlugin::hashToString(const std::array<uint8_t, 16>& hash) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (uint8_t byte : hash) {
        oss << std::setw(2) << static_cast<int>(byte);
    }
    return oss.str();
}

} // namespace plugins
} // namespace falcon