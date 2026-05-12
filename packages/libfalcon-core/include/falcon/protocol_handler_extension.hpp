/**
 * @file protocol_handler_extension.hpp
 * @brief Protocol handler extension interface for advanced features
 * @author Falcon Team
 * @date 2026-05-07
 */

#pragma once

#include <falcon/protocol_handler.hpp>

namespace falcon {

// Forward declaration
class ProtocolRegistry;

/**
 * @brief Optional extension interface for protocol handlers
 *
 * Protocol handlers can implement this interface to access
 * additional capabilities like protocol delegation.
 */
class IProtocolHandlerExtension {
public:
    virtual ~IProtocolHandlerExtension() = default;

    /**
     * @brief Set the protocol registry for delegation
     *
     * This allows protocol handlers (like Thunder, ED2K) to delegate
     * to other protocol handlers (like HTTP, FTP) after parsing.
     *
     * @param registry Pointer to the protocol registry
     */
    virtual void set_protocol_registry(ProtocolRegistry* registry) = 0;
};

/**
 * @brief Helper to get extension interface from a protocol handler
 *
 * @param handler The protocol handler
 * @return Extension interface pointer, or nullptr if not supported
 */
inline IProtocolHandlerExtension* get_extension(IProtocolHandler* handler) {
    return dynamic_cast<IProtocolHandlerExtension*>(handler);
}

} // namespace falcon
