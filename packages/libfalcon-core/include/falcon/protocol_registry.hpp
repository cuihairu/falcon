/**
 * @file protocol_registry.hpp
 * @brief Protocol handler registry
 * @author Falcon Team
 * @date 2026-04-22
 */

#pragma once

#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include <falcon/protocol_handler.hpp>

namespace falcon {

/**
 * @class ProtocolRegistry
 * @brief Registry for protocol handlers
 *
 * This class manages protocol handlers that can handle different types
 * of URLs and protocols. Handlers are registered at compile time.
 *
 * Note: This is a static registry for built-in protocol handlers,
 * not a dynamic plugin system. All handlers are linked at compile time.
 */
class ProtocolRegistry {
public:
    /**
     * @brief Constructor
     */
    ProtocolRegistry();

    /**
     * @brief Destructor
     */
    ~ProtocolRegistry();

    // Non-copyable, non-movable
    ProtocolRegistry(const ProtocolRegistry&) = delete;
    ProtocolRegistry& operator=(const ProtocolRegistry&) = delete;
    ProtocolRegistry(ProtocolRegistry&&) = delete;
    ProtocolRegistry& operator=(ProtocolRegistry&&) = delete;

    /**
     * @brief Register a protocol handler
     * @param handler Unique pointer to the handler instance
     */
    void register_handler(std::unique_ptr<IProtocolHandler> handler);

    /**
     * @brief Get a handler by protocol name
     * @param protocol The protocol name (e.g., "http", "ftp")
     * @return Pointer to the handler, or nullptr if not found
     */
    IProtocolHandler* get_handler(const std::string& protocol) const;

    /**
     * @brief Get a handler that can handle the given URL
     * @param url The URL to handle
     * @return Pointer to the handler, or nullptr if none can handle it
     */
    IProtocolHandler* get_handler_for_url(const std::string& url) const;

    /**
     * @brief Load all built-in protocol handlers
     * @note This function is implemented in the protocols library
     */
    void load_builtin_handlers();

    /**
     * @brief Get all supported protocols
     * @return Vector of supported protocol names
     */
    std::vector<std::string> supported_protocols() const;

    /**
     * @brief List all supported schemes from all handlers
     * @return Vector of supported URL schemes
     */
    std::vector<std::string> supported_schemes() const;

    /**
     * @brief Check if URL can be handled by any registered handler
     * @param url The URL to check
     * @return true if supported
     */
    bool supports_url(const std::string& url) const;

    /**
     * @brief Get number of registered handlers
     * @return Number of handlers
     */
    size_t handler_count() const;

private:
    /// Map of protocol name to handler
    std::unordered_map<std::string, std::unique_ptr<IProtocolHandler>> handlers_;
};

/**
 * Register built-in protocol handlers.
 *
 * This function is implemented in the protocols library so that
 * falcon_core does not depend on concrete protocol implementations.
 *
 * @param registry The registry to register handlers to
 */
void register_builtin_protocol_handlers(ProtocolRegistry& registry);

} // namespace falcon
