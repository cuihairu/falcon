/**
 * @file swarm_source_provider.cpp
 * @brief P2SP 查询源 provider 实现（§10.4 / §10.5 / §11 降级）。
 */

#include "swarm_source_provider.hpp"

#include <algorithm>

namespace falcon::daemon {

namespace {

// 退避节奏：首次失败 30s，之后指数翻倍，封顶 300s（§11：rdv 失效
// 不放大会话级重试风暴；300s 内自然恢复的会合服务按退避表回落）。
constexpr std::chrono::seconds kFirstBackoff{30};
constexpr std::chrono::seconds kMaxBackoff{300};

bool has_http_scheme(const std::string& url) {
    const std::string http = "http://";
    const std::string https = "https://";
    if (url.size() >= http.size() &&
        url.compare(0, http.size(), http) == 0) {
        return true;
    }
    return url.size() >= https.size() &&
           url.compare(0, https.size(), https) == 0;
}

}  // namespace

SwarmSourceProvider::SwarmSourceProvider(QueryFn query, std::string own_node_id)
    : query_(std::move(query)), own_node_id_(std::move(own_node_id)) {}

std::vector<std::string> SwarmSourceProvider::sources_for(
    const std::string& sha256_hex) {
    nlohmann::json result;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::chrono::steady_clock::now() < suppressed_until_) {
            return {};
        }
    }
    if (!query_) return {};
    if (query_(sha256_hex, &result) != 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++consecutive_failures_;
        const auto n = consecutive_failures_;
        const auto shift = std::min<std::uint32_t>(n - 1, 8);
        const auto wait =
            std::min(kFirstBackoff * (std::uint32_t{1} << shift), kMaxBackoff);
        suppressed_until_ = std::chrono::steady_clock::now() + wait;
        return {};
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        consecutive_failures_ = 0;
        suppressed_until_ = {};
    }
    return parse_sources(result, sha256_hex, own_node_id_);
}

std::chrono::steady_clock::time_point SwarmSourceProvider::suppressed_until() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return suppressed_until_;
}

std::uint32_t SwarmSourceProvider::consecutive_failures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return consecutive_failures_;
}

bool SwarmSourceProvider::node_source_usable(const nlohmann::json& source) {
    const auto adv = source.find("advertise");
    if (adv == source.end() || !adv->is_object()) return false;
    const auto addr = adv->find("addr");
    if (addr == adv->end() || !addr->is_string()) return false;
    const auto addr_str = addr->get<std::string>();
    if (addr_str.empty() || addr_str.find(':') == std::string::npos) return false;
    const auto direct = adv->find("direct");
    if (direct == adv->end() || !direct->is_boolean()) return false;
    return direct->get<bool>();
}

std::vector<std::string> SwarmSourceProvider::parse_sources(
    const nlohmann::json& result, const std::string& sha256_hex,
    const std::string& own_node_id) {
    std::vector<std::string> urls;
    if (!result.is_object()) return urls;
    const auto sources = result.find("sources");
    if (sources == result.end() || !sources->is_array()) return urls;

    for (const auto& entry : *sources) {
        if (!entry.is_object()) continue;
        const auto type = entry.find("type");
        if (type == entry.end() || !type->is_string()) continue;
        const auto t = type->get<std::string>();

        std::string url;
        if (t == "node") {
            if (!node_source_usable(entry)) continue;
            const auto node_id = entry.find("node_id");
            if (node_id == entry.end() || !node_id->is_string()) continue;
            if (!own_node_id.empty() && node_id->get<std::string>() == own_node_id) {
                continue;  // 不从自己拉数据
            }
            const auto adv_addr =
                entry.at("advertise").at("addr").get<std::string>();
            url = "http://" + adv_addr + "/by-sha256/" + sha256_hex;
        } else if (t == "url") {
            const auto u = entry.find("url");
            if (u == entry.end() || !u->is_string()) continue;
            if (!has_http_scheme(u->get<std::string>())) continue;
            url = u->get<std::string>();
        } else {
            continue;  // 未知类型静默跳过（前向兼容）
        }
        if (std::find(urls.begin(), urls.end(), url) == urls.end()) {
            urls.push_back(std::move(url));
        }
    }
    return urls;
}

}  // namespace falcon::daemon
