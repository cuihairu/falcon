#pragma once

#include <falcon/protocol_handler.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace falcon {
namespace internal {

/// URL utility functions
class UrlUtils {
public:
    /// Extract scheme from URL (e.g., "https" from "https://example.com")
    /// Also handles special URLs like "magnet:?" format
    static std::string extract_scheme(const std::string& url) {
        // First try standard "://" format
        auto pos = url.find("://");
        if (pos != std::string::npos) {
            std::string scheme = url.substr(0, pos);
            // Convert to lowercase
            std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return scheme;
        }

        // Try "scheme:" format (like magnet:?)
        auto colon_pos = url.find(':');
        if (colon_pos != std::string::npos && colon_pos > 0) {
            // Ensure the scheme contains only valid characters
            std::string scheme = url.substr(0, colon_pos);
            bool valid = true;
            for (char c : scheme) {
                if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' &&
                    c != '-' && c != '.') {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                std::transform(scheme.begin(), scheme.end(), scheme.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                return scheme;
            }
        }

        return "";
    }

    /// Extract filename from URL
    static std::string extract_filename(const std::string& url) {
        // Remove query string and fragment
        std::string clean_url = url;
        auto query_pos = clean_url.find('?');
        if (query_pos != std::string::npos) {
            clean_url = clean_url.substr(0, query_pos);
        }
        auto fragment_pos = clean_url.find('#');
        if (fragment_pos != std::string::npos) {
            clean_url = clean_url.substr(0, fragment_pos);
        }

        // Remove trailing slash
        while (!clean_url.empty() && clean_url.back() == '/') {
            clean_url.pop_back();
        }

        // Remove scheme and host part
        auto scheme_end = clean_url.find("://");
        if (scheme_end != std::string::npos) {
            clean_url = clean_url.substr(scheme_end + 3);
        }

        // Find first slash (separates host from path)
        auto first_slash = clean_url.find('/');
        if (first_slash == std::string::npos) {
            // No path, just host - return default
            return "download";
        }

        // Get path part
        std::string path = clean_url.substr(first_slash + 1);
        if (path.empty()) {
            return "download";
        }

        // Find last slash in path
        auto last_slash = path.rfind('/');
        std::string filename =
            (last_slash != std::string::npos) ? path.substr(last_slash + 1) : path;

        return filename.empty() ? "download" : filename;
    }

    /// Check if URL is valid
    static bool is_valid_url(const std::string& url) {
        if (url.empty()) {
            return false;
        }
        auto scheme = extract_scheme(url);
        return !scheme.empty();
    }
};

/// Plugin manager for protocol handlers
class PluginManager {
public:
    PluginManager() = default;
    ~PluginManager() = default;

    // Non-copyable
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    /// Register a protocol handler
    void register_handler(std::unique_ptr<IProtocolHandler> handler) {
        std::lock_guard<std::mutex> lock(mutex_);

        auto schemes = handler->supported_schemes();
        for (const auto& scheme : schemes) {
            scheme_handlers_[scheme].push_back(handler.get());
        }

        handlers_.push_back(std::move(handler));
        sort_handlers();
    }

    /// Find handler for URL
    [[nodiscard]] IProtocolHandler* find_handler(const std::string& url) const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::string scheme = UrlUtils::extract_scheme(url);
        if (scheme.empty()) {
            return nullptr;
        }

        auto it = scheme_handlers_.find(scheme);
        if (it == scheme_handlers_.end() || it->second.empty()) {
            return nullptr;
        }

        // Return highest priority handler that can handle the URL
        for (auto* handler : it->second) {
            if (handler->can_handle(url)) {
                return handler;
            }
        }

        return nullptr;
    }

    /// Check if URL is supported
    [[nodiscard]] bool is_supported(const std::string& url) const {
        return find_handler(url) != nullptr;
    }

    /// Get list of supported protocols
    [[nodiscard]] std::vector<std::string> get_protocols() const {
        std::lock_guard<std::mutex> lock(mutex_);

        std::vector<std::string> protocols;
        protocols.reserve(scheme_handlers_.size());
        for (const auto& pair : scheme_handlers_) {
            protocols.push_back(pair.first);
        }
        return protocols;
    }

private:
    void sort_handlers() {
        // Sort handlers by priority (descending)
        for (auto& pair : scheme_handlers_) {
            std::sort(pair.second.begin(), pair.second.end(),
                      [](const IProtocolHandler* a, const IProtocolHandler* b) {
                          return a->priority() > b->priority();
                      });
        }
    }

    mutable std::mutex mutex_;
    std::vector<std::unique_ptr<IProtocolHandler>> handlers_;
    std::unordered_map<std::string, std::vector<IProtocolHandler*>> scheme_handlers_;
};

}  // namespace internal
}  // namespace falcon
