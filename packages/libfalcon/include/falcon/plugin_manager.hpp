#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

namespace falcon {

// Forward declarations
class IProtocolHandler;

/**
 * @class PluginManager
 * @brief Manages protocol plugins
 *
 * This class is responsible for registering, managing and providing access
 * to various protocol handlers (plugins) that can handle different types
 * of URLs and protocols.
 */
class PluginManager {
public:
    /**
     * @brief Constructor
     */
    PluginManager();

    /**
     * @brief Destructor
     */
    ~PluginManager();

    // Non-copyable, non-movable
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;
    PluginManager(PluginManager&&) = delete;
    PluginManager& operator=(PluginManager&&) = delete;

    /**
     * @brief Register a new protocol plugin
     * @param plugin Unique pointer to the plugin instance
     */
    void registerPlugin(std::unique_ptr<IProtocolHandler> plugin);

    /**
     * @brief Get a plugin by protocol name
     * @param protocol The protocol name (e.g., "http", "ftp")
     * @return Pointer to the plugin handler, or nullptr if not found
     */
    IProtocolHandler* getPlugin(const std::string& protocol) const;

    /**
     * @brief Get a plugin that can handle the given URL
     * @param url The URL to handle
     * @return Pointer to the plugin handler, or nullptr if none can handle it
     */
    IProtocolHandler* getPluginByUrl(const std::string& url) const;

    /**
     * @brief Load all available plugins based on build configuration
     */
    void loadAllPlugins();

    /**
     * @brief Get all supported protocols
     * @return Vector of supported protocol names
     */
    std::vector<std::string> getSupportedProtocols() const;

private:
    /// Map of protocol name to plugin handler
    std::unordered_map<std::string, std::unique_ptr<IProtocolHandler>> plugins_;
};

} // namespace falcon