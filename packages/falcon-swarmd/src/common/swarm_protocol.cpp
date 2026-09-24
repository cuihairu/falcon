// ============================================================================
// falcon-swarmd 线协议助手实现（见 swarm_protocol.hpp 头注释）
// ============================================================================

#include "swarm_protocol.hpp"

#include <cstdio>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

namespace falcon::swarm {

std::string make_challenge_value() {
    const auto bytes = SwarmCrypto::random_bytes(16);
    if (bytes.empty()) {
        return {};
    }
    return SwarmCrypto::bytes_to_hex(bytes.data(), bytes.size());
}

std::string make_session_id() {
    const auto bytes = SwarmCrypto::random_bytes(16);
    if (bytes.empty()) {
        return {};
    }
    return "s-" + SwarmCrypto::bytes_to_hex(bytes.data(), bytes.size());
}

bool is_hex_string(const std::string& s, std::size_t exact_len) {
    if (s.size() != exact_len) {
        return false;
    }
    for (const char c : s) {
        const bool lower_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!lower_hex) {
            return false;
        }
    }
    return true;
}

std::string fingerprint_from_pubkey_hex(const std::string& pubkey_hex) {
    const auto der = SwarmCrypto::hex_to_bytes(pubkey_hex);
    if (der.empty()) {
        return {};
    }
    return SwarmCrypto::fingerprint(der);
}

std::string rfc3339_utc(std::chrono::system_clock::time_point tp) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &t) != 0) {
        return {};
    }
#else
    if (gmtime_r(&t, &tm) == nullptr) {
        return {};
    }
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        return {};
    }
    return buf;
}

}  // namespace falcon::swarm
