/**
 * @file protocol_registry.cpp
 * @brief Protocol registry implementation
 * @author Falcon Team
 * @date 2026-04-22
 */

#include <falcon/protocol_registry.hpp>
#include <algorithm>
#include <shared_mutex>

namespace falcon {

ProtocolRegistry::ProtocolRegistry() = default;

ProtocolRegistry::~ProtocolRegistry() = default;

void ProtocolRegistry::register_handler(std::unique_ptr<IProtocolHandler> handler) {
    if (!handler) {
        return;
    }

    const std::string protocol = handler->protocol_name();

    // Check if handler for this protocol already exists
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto it = handlers_.find(protocol);
    if (it != handlers_.end()) {
        // Replace existing handler
        handlers_.erase(it);
    }

    handlers_[protocol] = std::move(handler);
}

IProtocolHandler* ProtocolRegistry::find_handler_unlocked(const std::string& protocol) const {
    auto it = handlers_.find(protocol);
    if (it != handlers_.end()) {
        return it->second.get();
    }
    return nullptr;
}

IProtocolHandler* ProtocolRegistry::get_handler(const std::string& protocol) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return find_handler_unlocked(protocol);
}

IProtocolHandler* ProtocolRegistry::get_handler_for_url(const std::string& url) const {
    // First, try to extract scheme from URL
    const size_t scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        std::string scheme = url.substr(0, scheme_end);

        // Handle special cases
        if (scheme == "http" || scheme == "https") {
            // Check for HLS streams
            if (url.find(".m3u8") != std::string::npos ||
                url.find(".mpd") != std::string::npos) {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                auto* hls_handler = find_handler_unlocked("hls");
                if (hls_handler && hls_handler->can_handle(url)) {
                    return hls_handler;
                }
            }
        }

        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto* handler = find_handler_unlocked(scheme);
            if (handler) {
                return handler;
            }
        }
    }

    // If scheme-based lookup fails, try all handlers
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& pair : handlers_) {
        if (pair.second->can_handle(url)) {
            return pair.second.get();
        }
    }

    return nullptr;
}

void ProtocolRegistry::load_builtin_handlers() {
    register_builtin_protocol_handlers(*this);
}

std::vector<std::string> ProtocolRegistry::supported_protocols() const {
    std::vector<std::string> protocols;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        protocols.reserve(handlers_.size());
        for (const auto& pair : handlers_) {
            protocols.push_back(pair.first);
        }
    }

    std::sort(protocols.begin(), protocols.end());
    return protocols;
}

std::vector<std::string> ProtocolRegistry::supported_schemes() const {
    std::vector<std::string> schemes;

    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (const auto& pair : handlers_) {
            auto handler_schemes = pair.second->supported_schemes();
            schemes.insert(schemes.end(),
                          handler_schemes.begin(),
                          handler_schemes.end());
        }
    }

    // Sort and deduplicate
    std::sort(schemes.begin(), schemes.end());
    schemes.erase(std::unique(schemes.begin(), schemes.end()),
                  schemes.end());

    return schemes;
}

bool ProtocolRegistry::supports_url(const std::string& url) const {
    return get_handler_for_url(url) != nullptr;
}

size_t ProtocolRegistry::handler_count() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return handlers_.size();
}

} // namespace falcon
