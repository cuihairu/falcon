#pragma once

#include <algorithm>
#include <string>

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

}  // namespace internal
}  // namespace falcon
