/**
 * @file search_detail.cpp
 * @brief 资源搜索纯计算工具（无网络依赖）
 *
 * 该文件同时编译进 libs/drives 门面与 resource_search 插件。
 */
#include <falcon/drives/resource_search.hpp>
#include <falcon/logger.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace falcon {
namespace search {
namespace detail {

double calculate_confidence(const SearchResult& result) {
    double confidence = 0.5;
    if (result.seeds > 100) confidence += 0.3;
    else if (result.seeds > 50) confidence += 0.2;
    else if (result.seeds > 10) confidence += 0.1;
    if (result.size > 100 * 1024 * 1024 && result.size < 10ULL * 1024 * 1024 * 1024) {
        confidence += 0.1;
    }
    return (std::min)(confidence, 1.0);
}

size_t parse_size(const std::string& size_str) {
    std::regex size_regex("(\\d+(?:\\.\\d+)?)\\s*(B|KB|MB|GB|TB)", std::regex::icase);
    std::smatch match;
    if (std::regex_search(size_str, match, size_regex)) {
        double value = std::stod(match[1].str());
        std::string unit = match[2].str();
        std::transform(unit.begin(), unit.end(), unit.begin(), ::toupper);
        if (unit == "B") return static_cast<size_t>(value);
        if (unit == "KB") return static_cast<size_t>(value * 1024);
        if (unit == "MB") return static_cast<size_t>(value * 1024 * 1024);
        if (unit == "GB") return static_cast<size_t>(value * 1024 * 1024 * 1024);
        if (unit == "TB") return static_cast<size_t>(value * 1024ULL * 1024 * 1024 * 1024);
    }
    return 0;
}

void apply_selector_field(SearchResult& result, const std::string& key,
                          const std::string& value) {
    if (key == "title") {
        result.title = value;
    } else if (key == "url") {
        result.url = value;
    } else if (key == "magnet") {
        result.url = value;
        if (result.type.empty()) result.type = "magnet";
    } else if (key == "hash") {
        result.hash = value;
    } else if (key == "size") {
        result.size = parse_size(value);
    } else if (key == "seeds") {
        try { result.seeds = std::stoi(value); } catch (...) {}
    } else if (key == "peers") {
        try { result.peers = std::stoi(value); } catch (...) {}
    } else if (key == "leeches") {
        try { result.peers = std::stoi(value); } catch (...) {}
    } else if (key == "date") {
        result.publish_date = value;
    } else if (key == "type") {
        result.type = value;
    } else {
        result.metadata[key] = value;
    }
}

std::vector<SearchResult> parse_html_by_selectors(const std::string& html,
                                                  const SearchEngineConfig& config) {
    std::vector<SearchResult> results;
    if (html.empty()) return results;

    const auto it_item = config.selectors.find("item");
    if (it_item == config.selectors.end() || it_item->second.empty()) {
        return results;
    }

    std::regex item_regex;
    try {
        item_regex = std::regex(it_item->second, std::regex::ECMAScript);
    } catch (const std::regex_error& e) {
        FALCON_LOG_ERROR_STREAM("[" << config.name << "] 无效的 item 正则 '"
            << it_item->second << "': " << e.what());
        return results;
    }

    struct FieldRegex {
        std::string key;
        std::regex regex;
    };
    std::vector<FieldRegex> field_regexes;
    for (const auto& sel : config.selectors) {
        if (sel.first == "item") continue;
        if (sel.second.empty()) continue;
        try {
            field_regexes.push_back({sel.first, std::regex(sel.second, std::regex::ECMAScript)});
        } catch (const std::regex_error& e) {
            FALCON_LOG_ERROR_STREAM("[" << config.name << "] 无效的 "
                << sel.first << " 正则 '" << sel.second << "': " << e.what());
        }
    }

    std::sregex_iterator iter(html.begin(), html.end(), item_regex);
    const std::sregex_iterator end;

    for (; iter != end; ++iter) {
        const std::string item_html = iter->str(0);
        if (item_html.empty()) continue;

        SearchResult result;
        for (const auto& fr : field_regexes) {
            std::smatch m;
            if (!std::regex_search(item_html, m, fr.regex) || m.size() < 2) continue;
            apply_selector_field(result, fr.key, m.str(1));
        }

        if (!result.title.empty() && !result.url.empty()) {
            result.source = config.name;
            result.confidence = calculate_confidence(result);
            results.push_back(result);
        }
    }

    return results;
}

std::string url_decode(const std::string& str) {
    std::string result;
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            int value;
            std::istringstream is(str.substr(i + 1, 2));
            if (is >> std::hex >> value) {
                result += char(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else if (str[i] == '+') {
            result += ' ';
        } else {
            result += str[i];
        }
    }
    return result;
}

SearchResult parse_magnet_link(const std::string& magnet_url) {
    SearchResult result;
    result.url = magnet_url;
    result.type = "magnet";

    std::regex hash_regex("btih:([a-fA-F0-9]{40})");
    std::regex name_regex("dn=([^&]+)");

    std::smatch match;
    if (std::regex_search(magnet_url, match, hash_regex)) {
        result.hash = match[1].str();
    }
    if (std::regex_search(magnet_url, match, name_regex)) {
        result.title = url_decode(match[1].str());
    }
    return result;
}

} // namespace detail
} // namespace search
} // namespace falcon
