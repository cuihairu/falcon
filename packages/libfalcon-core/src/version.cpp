/**
 * @file version.cpp
 * @brief 版本信息实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/version.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <sstream>
#include <tuple>
#include <vector>

namespace falcon {

std::string Version::to_string() const {
    std::ostringstream oss;
    oss << major << "." << minor << "." << patch;
    return oss.str();
}

std::string Version::to_full_string() const {
    std::ostringstream oss;
    oss << "Falcon " << to_string();

#ifdef FALCON_VERSION_SUFFIX
    oss << "-" << FALCON_VERSION_SUFFIX;
#endif

#ifdef __DATE__
    oss << " (built " << __DATE__;
#ifdef __TIME__
    oss << " " << __TIME__;
#endif
    oss << ")";
#endif

    return oss.str();
}

std::optional<Version> Version::parse(const std::string& version) {
    if (version.empty()) {
        return std::nullopt;
    }

    std::string input = version;
    if (!input.empty() && (input.front() == 'v' || input.front() == 'V')) {
        input.erase(input.begin());
    }

    if (input.empty()) {
        return std::nullopt;
    }

    std::stringstream ss(input);
    std::string part;
    std::vector<int> parts;
    while (std::getline(ss, part, '.')) {
        if (part.empty()) {
            return std::nullopt;
        }
        if (!std::all_of(part.begin(), part.end(),
                         [](unsigned char ch) { return std::isdigit(ch) != 0; })) {
            return std::nullopt;
        }
        parts.push_back(std::stoi(part));
    }

    if (parts.empty() || parts.size() > 3) {
        return std::nullopt;
    }

    Version parsed;
    parsed.major = parts[0];
    parsed.minor = parts.size() > 1 ? parts[1] : 0;
    parsed.patch = parts.size() > 2 ? parts[2] : 0;
    return parsed;
}

bool Version::operator==(const Version& other) const noexcept {
    return std::tie(major, minor, patch) == std::tie(other.major, other.minor, other.patch);
}

bool Version::operator!=(const Version& other) const noexcept {
    return !(*this == other);
}

bool Version::operator<(const Version& other) const noexcept {
    return std::tie(major, minor, patch) < std::tie(other.major, other.minor, other.patch);
}

bool Version::operator>(const Version& other) const noexcept {
    return other < *this;
}

bool Version::operator<=(const Version& other) const noexcept {
    return !(*this > other);
}

bool Version::operator>=(const Version& other) const noexcept {
    return !(*this < other);
}

std::ostream& operator<<(std::ostream& os, const Version& version) {
    os << version.to_string();
    return os;
}

const Version FALCON_VERSION{0, 1, 0};

const char* get_version_string() {
    static std::string version = FALCON_VERSION.to_full_string();
    return version.c_str();
}

const char* get_build_timestamp() {
    return __DATE__ " " __TIME__;
}

} // namespace falcon
