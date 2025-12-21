/**
 * @file plugin_manager.cpp
 * @brief 插件管理器实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/plugin_manager.hpp>
#include <iostream>
#include <format>

// 包含所有插件头文件
#ifdef FALCON_ENABLE_HTTP
#include "../plugins/http/http_handler.hpp"
#endif

namespace falcon {

PluginManager::PluginManager() {
    std::cout <<"Initializing plugin manager");
}

PluginManager::~PluginManager() {
    std::cout <<("Shutting down plugin manager");
}

void PluginManager::registerPlugin(std::unique_ptr<IProtocolHandler> plugin) {
    if (!plugin) {
        std::cerr <<("Attempted to register null plugin");
        return;
    }

    std::string protocol = plugin->protocol_name();

    // 检查是否已注册相同协议的插件
    auto it = plugins_.find(protocol);
    if (it != plugins_.end()) {
        std::cerr << "Plugin for protocol '" << protocol << "' already registered, replacing" << std::endl;
    }

    std::cout << "Registering plugin for protocol: " << protocol << std::endl;
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
                if (hlsPlugin && hlsPlugin->can_handle(url)) {
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
        if (pair.second->can_handle(url)) {
            return pair.second.get();
        }
    }

    return nullptr;
}

void PluginManager::loadAllPlugins() {
    std::cout << "Loading all available plugins" << std::endl;

#ifdef FALCON_ENABLE_HTTP
    registerPlugin(std::make_unique<plugins::HttpHandler>());
#endif

#ifdef FALCON_ENABLE_FTP
#endif

#ifdef FALCON_ENABLE_BITTORRENT
#endif

// 加载私有协议插件
#ifdef FALCON_ENABLE_THUNDER
#endif

#ifdef FALCON_ENABLE_QQDL
#endif

#ifdef FALCON_ENABLE_FLASHGET
#endif

#ifdef FALCON_ENABLE_ED2K
#endif

#ifdef FALCON_ENABLE_HLS
#endif

    std::cout << "Loaded " << plugins_.size() << " plugins" << std::endl;
}

std::vector<std::string> PluginManager::getSupportedProtocols() const {
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
        std::cout << "Unloading plugin for protocol: " << protocol << std::endl;
        plugins_.erase(it);
    } else {
        std::cerr <<("No plugin found for protocol: {}", protocol);
    }
}

void PluginManager::unloadAllPlugins() {
    std::cout << "Unloading all plugins" << std::endl;
    plugins_.clear();
}

size_t PluginManager::getPluginCount() const {
    return plugins_.size();
}

} // namespace falcon