/**
 * @file plugin_manager.cpp
 * @brief 插件管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/plugin_manager.hpp>
#include <falcon/logger.hpp>

// 包含所有插件头文件
#ifdef FALCON_ENABLE_HTTP
#include "../plugins/http/http_plugin.hpp"
#endif

#ifdef FALCON_ENABLE_FTP
#include "../plugins/ftp/ftp_plugin.hpp"
#endif

#ifdef FALCON_ENABLE_BITTORRENT
#include "../plugins/bittorrent/bittorrent_plugin.hpp"
#endif

// 新增私有协议
#ifdef FALCON_ENABLE_THUNDER
#include "../plugins/thunder/thunder_plugin.hpp"
#endif

#ifdef FALCON_ENABLE_QQDL
#include "../plugins/qqdl/qqdl_plugin.hpp"
#endif

#ifdef FALCON_ENABLE_FLASHGET
#include "../plugins/flashget/flashget_plugin.hpp"
#endif

#ifdef FALCON_ENABLE_ED2K
#include "../plugins/ed2k/ed2k_plugin.hpp"
#endif

#ifdef FALCON_ENABLE_HLS
#include "../plugins/hls/hls_plugin.hpp"
#endif

namespace falcon {

PluginManager::PluginManager() {
    FALCON_LOG_INFO("Initializing plugin manager");
}

PluginManager::~PluginManager() {
    FALCON_LOG_DEBUG("Shutting down plugin manager");
}

void PluginManager::registerPlugin(std::unique_ptr<IProtocolHandler> plugin) {
    if (!plugin) {
        FALCON_LOG_WARN("Attempted to register null plugin");
        return;
    }

    std::string protocol = plugin->getProtocolName();

    // 检查是否已注册相同协议的插件
    auto it = plugins_.find(protocol);
    if (it != plugins_.end()) {
        FALCON_LOG_WARN("Plugin for protocol '{}' already registered, replacing", protocol);
    }

    FALCON_LOG_INFO("Registering plugin for protocol: {}", protocol);
    plugins_[protocol] = std::move(plugin);
}

IProtocolHandler* PluginManager::getPlugin(const std::string& protocol) const {
    auto it = plugins_.find(protocol);
    if (it != plugins_.end()) {
        return it->second.get();
    }
    return nullptr;
}

IProtocolHandler* PluginManager::getPluginByUrl(const std::string& url) const {
    // 首先尝试从URL中提取协议
    size_t schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
        std::string scheme = url.substr(0, schemeEnd);

        // 处理一些特殊情况
        if (scheme == "http" || scheme == "https") {
            // 检查是否是特殊格式的链接
            if (url.find(".m3u8") != std::string::npos || url.find(".mpd") != std::string::npos) {
                auto hlsPlugin = getPlugin("hls");
                if (hlsPlugin && hlsPlugin->canHandle(url)) {
                    return hlsPlugin;
                }
            }
        }

        auto plugin = getPlugin(scheme);
        if (plugin) {
            return plugin;
        }
    }

    // 如果直接通过scheme找不到，遍历所有插件检查
    for (const auto& pair : plugins_) {
        if (pair.second->canHandle(url)) {
            return pair.second.get();
        }
    }

    return nullptr;
}

void PluginManager::loadAllPlugins() {
    FALCON_LOG_INFO("Loading all available plugins");

#ifdef FALCON_ENABLE_HTTP
    registerPlugin(std::make_unique<HttpPlugin>());
#endif

#ifdef FALCON_ENABLE_FTP
    registerPlugin(std::make_unique<FtpPlugin>());
#endif

#ifdef FALCON_ENABLE_BITTORRENT
    registerPlugin(std::make_unique<BitTorrentPlugin>());
#endif

// 加载私有协议插件
#ifdef FALCON_ENABLE_THUNDER
    registerPlugin(std::make_unique<ThunderPlugin>());
#endif

#ifdef FALCON_ENABLE_QQDL
    registerPlugin(std::make_unique<QQDLPlugin>());
#endif

#ifdef FALCON_ENABLE_FLASHGET
    registerPlugin(std::make_unique<FlashGetPlugin>());
#endif

#ifdef FALCON_ENABLE_ED2K
    registerPlugin(std::make_unique<ED2KPlugin>());
#endif

#ifdef FALCON_ENABLE_HLS
    registerPlugin(std::make_unique<HLSPlugin>());
#endif

    FALCON_LOG_INFO("Loaded {} plugins", plugins_.size());
}

std::vector<std::string> PluginManager::listSupportedProtocols() const {
    std::vector<std::string> protocols;
    protocols.reserve(plugins_.size());

    for (const auto& pair : plugins_) {
        protocols.push_back(pair.first);
    }

    // 排序以保证一致性
    std::sort(protocols.begin(), protocols.end());

    return protocols;
}

std::vector<std::string> PluginManager::listSupportedSchemes() const {
    std::vector<std::string> schemes;

    for (const auto& pair : plugins_) {
        auto pluginSchemes = pair.second->getSupportedSchemes();
        schemes.insert(schemes.end(), pluginSchemes.begin(), pluginSchemes.end());
    }

    // 去重并排序
    std::sort(schemes.begin(), schemes.end());
    schemes.erase(std::unique(schemes.begin(), schemes.end()), schemes.end());

    return schemes;
}

bool PluginManager::supportsUrl(const std::string& url) const {
    return getPluginByUrl(url) != nullptr;
}

void PluginManager::unloadPlugin(const std::string& protocol) {
    auto it = plugins_.find(protocol);
    if (it != plugins_.end()) {
        FALCON_LOG_INFO("Unloading plugin for protocol: {}", protocol);
        plugins_.erase(it);
    } else {
        FALCON_LOG_WARN("No plugin found for protocol: {}", protocol);
    }
}

void PluginManager::unloadAllPlugins() {
    FALCON_LOG_INFO("Unloading all plugins");
    plugins_.clear();
}

size_t PluginManager::getPluginCount() const {
    return plugins_.size();
}

} // namespace falcon