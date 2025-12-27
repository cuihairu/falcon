/**
 * @file resource_search.cpp
 * @brief 资源搜索功能实现
 * @author Falcon Team
 * @date 2025-12-21
 */

#include <falcon/resource_search.hpp>
#include <falcon/logger.hpp>
#include <curl/curl.h>
#ifdef HAVE_NLOHMANN_JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif
#include <thread>
#include <chrono>
#include <regex>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <fstream>
#include <future>

namespace falcon {
namespace search {

/**
 * @brief CURL写数据回调
 */
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

/**
 * @brief Web爬虫实现
 */
class WebCrawler {
public:
    WebCrawler() {
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to initialize CURL");
        }
    }

    ~WebCrawler() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    void set_proxy(const std::string& proxy_url,
                   const std::string& username = "",
                   const std::string& password = "",
                   const std::string& proxy_type = "") {
        if (!proxy_url.empty()) {
            curl_easy_setopt(curl_, CURLOPT_PROXY, proxy_url.c_str());

            // 设置代理类型
            curl_proxytype type = CURLPROXY_HTTP;
            if (!proxy_type.empty()) {
                if (proxy_type == "socks4") {
                    type = CURLPROXY_SOCKS4;
                } else if (proxy_type == "socks5") {
                    type = CURLPROXY_SOCKS5;
                } else if (proxy_type == "socks5h") {
                    type = CURLPROXY_SOCKS5_HOSTNAME;
                }
            } else {
                // 从URL自动检测
                if (proxy_url.find("socks5://") == 0 || proxy_url.find("socks5h://") == 0) {
                    type = CURLPROXY_SOCKS5;
                } else if (proxy_url.find("socks4://") == 0) {
                    type = CURLPROXY_SOCKS4;
                }
            }
            curl_easy_setopt(curl_, CURLOPT_PROXYTYPE, type);

            // 设置代理认证
            if (!username.empty()) {
                std::string userpwd = username;
                if (!password.empty()) {
                    userpwd += ":" + password;
                }
                curl_easy_setopt(curl_, CURLOPT_PROXYUSERPWD, userpwd.c_str());
            }
        }
    }

    void set_timeout(int timeout_seconds) {
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT, timeout_seconds);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, timeout_seconds);
    }

    void set_headers(const std::map<std::string, std::string>& headers) {
        curl_slist* chunk = nullptr;
        for (const auto& [key, value] : headers) {
            std::string header = key + ": " + value;
            chunk = curl_slist_append(chunk, header.c_str());
        }
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, chunk);

        // 保存chunk以便后续清理
        if (headers_) {
            curl_slist_free_all(headers_);
        }
        headers_ = chunk;
    }

    std::string get(const std::string& url) {
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_);

        // 启用SSL验证（可选）
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

        CURLcode res = curl_easy_perform(curl_);
        if (res != CURLE_OK) {
            FALCON_LOG_ERROR("CURL error: " << curl_easy_strerror(res));
            return "";
        }

        std::string result = response_;
        response_.clear();

        return result;
    }

private:
    CURL* curl_;
    curl_slist* headers_ = nullptr;
    std::string response_;
};

/**
 * @brief 通用搜索引擎实现
 */
class GenericSearchProvider : public ISearchProvider {
public:
    GenericSearchProvider(const SearchEngineConfig& config)
        : config_(config), crawler_(std::make_unique<WebCrawler>()) {
        crawler_->set_timeout(30);
        crawler_->set_headers(config_.headers);
    }

    std::string name() const override {
        return config_.name;
    }

    std::vector<SearchResult> search(const SearchQuery& query) override {
        std::vector<SearchResult> results;

        // 构建搜索URL
        std::string search_url = build_search_url(query);
        if (search_url.empty()) {
            return results;
        }

        // 发送请求
        FALCON_LOG_DEBUG("Searching " << config_.name << " with query: " << query.keyword);
        std::string response = crawler_->get(search_url);

        if (response.empty()) {
            FALCON_LOG_WARN("No response from " << config_.name);
            return results;
        }

        // 解析响应
        // TODO: 实现 response_format 支持
        // if (config_.response_format == "json") {
        //     results = parse_json_response(response);
        // } else {
            results = parse_html_response(response);
        // }

        // 应用过滤
        results = filter_results(results, query);

        FALCON_LOG_DEBUG("Found " << results.size() << " results from " << config_.name);
        return results;
    }

    bool validate_url(const std::string& url) override {
        if (url.empty()) return false;

        // 检查URL格式
        if (url.find("magnet:") == 0 ||
            url.find("http:") == 0 ||
            url.find("https:") == 0 ||
            url.find("ftp:") == 0) {
            return true;
        }

        return false;
    }

    SearchResult get_details(const std::string& url) override {
        SearchResult result;
        result.url = url;

        // 对于magnet链接，解析基本信息
        if (url.find("magnet:") == 0) {
            result = parse_magnet_link(url);
        }

        return result;
    }

    bool is_available() override {
        std::string test_url = config_.base_url;
        std::string response = crawler_->get(test_url);
        return !response.empty() && response.find("404") == std::string::npos;
    }

    int get_delay() const override {
        return config_.delay_ms;
    }

protected:
    virtual std::string build_search_url(const SearchQuery& query) {
        std::string url = config_.base_url + config_.search_path;

        // 添加查询参数
        bool first_param = url.find('?') == std::string::npos;

        // 特殊处理MagnetDL的路径模式
        // TODO: 实现 path_pattern 支持
        // if (config_.path_pattern.find("{query_letter}") != std::string::npos) {
        //     std::string pattern = config_.path_pattern;
        //     std::string query_letter = query.keyword.empty() ? "all" :
        //                              std::string(1, static_cast<char>(std::tolower(query.keyword[0])));
        //     std::string first_char = std::isalpha(query_letter[0]) ? "1" : "0";
        //
        //     replace_all(pattern, "{first_char}", first_char);
        //     replace_all(pattern, "{query_letter}", query_letter);
        //     replace_all(pattern, "{page}", std::to_string(query.page));
        //
        //     url = config_.base_url + pattern;
        //
        //     // 添加关键词作为参数
        //     if (!query.keyword.empty()) {
        //         url += "?s=" + url_encode(query.keyword);
        //     }
        // } else {
            // 构建查询参数
            std::string query_str;
            for (const auto& [key, value] : config_.params) {
                if (!query_str.empty()) query_str += "&";

                std::string param_value = value;
                if (value.empty() && key == "search" || key == "q" || key == "keyword") {
                    param_value = query.keyword;
                }

                query_str += key + "=" + url_encode(param_value);
            }

            if (!query_str.empty()) {
                url += (first_param ? "?" : "&") + query_str;
            }
        }

        return url;
    }

    virtual std::vector<SearchResult> parse_html_response(const std::string& html) {
        std::vector<SearchResult> results;

        // TODO: 实现 selectors 支持
        // 简单的HTML解析（实际项目中应使用专门的HTML解析库）
        // if (config_.selectors.empty()) {
        //     return results;
        // }
        //
        // // 这里使用正则表达式作为简单实现
        // // 实际项目中建议使用Gumbo或类似的HTML解析库
        //
        // std::string item_selector = config_.selectors.at("item");

        // 示例：解析1337x的HTML结构
        if (config_.name == "1337x") {
            std::regex item_regex("<tr>(.*?)</tr>", std::regex::ECMAScript);
            std::sregex_iterator iter(html.begin(), html.end(), item_regex);
            std::sregex_iterator end;

            for (; iter != end; ++iter) {
                std::string item_html = iter->str();
                SearchResult result = parse_1337x_item(item_html);
                if (!result.title.empty()) {
                    result.source = config_.name;
                    result.confidence = calculate_confidence(result);
                    results.push_back(result);
                }
            }
        }
        // 其他网站的解析逻辑...

        return results;
    }

    virtual std::vector<SearchResult> parse_json_response(const std::string& json_str) {
        std::vector<SearchResult> results;

        try {
            json data = json::parse(json_str);

            // 解析ThePirateBay的API响应
            if (config_.name == "ThePirateBay") {
                for (const auto& item : data) {
                    SearchResult result;
                    result.title = item["name"].get<std::string>();
                    result.url = "magnet:?xt=urn:btih:" + item["info_hash"].get<std::string>();
                    result.size = parse_size(item["size"].get<std::string>());
                    result.seeds = item["seeders"].get<int>();
                    result.peers = item["leechers"].get<int>();
                    result.hash = item["info_hash"].get<std::string>();
                    result.source = config_.name;
                    result.confidence = calculate_confidence(result);

                    results.push_back(result);
                }
            }
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR("Failed to parse JSON response: " << e.what());
        }

        return results;
    }

    std::vector<SearchResult> filter_results(const std::vector<SearchResult>& results,
                                            const SearchQuery& query) {
        std::vector<SearchResult> filtered;

        for (const auto& result : results) {
            // 大小过滤
            if (query.min_size > 0 && result.size < query.min_size) continue;
            if (query.max_size > 0 && result.size > query.max_size) continue;

            // 种子数过滤
            if (query.min_seeds > 0 && result.seeds < query.min_seeds) continue;

            filtered.push_back(result);
        }

        // 排序
        if (!query.sort_by.empty()) {
            std::sort(filtered.begin(), filtered.end(),
                [&](const SearchResult& a, const SearchResult& b) {
                    if (query.sort_by == "size") {
                        return query.sort_desc ? a.size > b.size : a.size < b.size;
                    } else if (query.sort_by == "seeds") {
                        return query.sort_desc ? a.seeds > b.seeds : a.seeds < b.seeds;
                    }
                    return query.sort_desc ? a.confidence > b.confidence : a.confidence < b.confidence;
                });
        }

        // 限制结果数量
        if (filtered.size() > query.limit) {
            filtered.resize(query.limit);
        }

        return filtered;
    }

private:
    SearchEngineConfig config_;
    std::unique_ptr<WebCrawler> crawler_;

    SearchResult parse_1337x_item(const std::string& html) {
        SearchResult result;

        std::regex title_regex("<a href=\"([^\"]+)\">([^<]+)</a>");
        std::regex seeds_regex("<td[^>]*>(\\d+)</td>");

        std::smatch match;
        if (std::regex_search(html, match, title_regex)) {
            result.url = config_.base_url + match[1].str();
            result.title = match[2].str();
        }

        // 提取其他字段...

        return result;
    }

    SearchResult parse_magnet_link(const std::string& magnet_url) {
        SearchResult result;
        result.url = magnet_url;
        result.type = "magnet";

        // 解析magnet链接中的信息
        std::regex hash_regex("btih:([a-fA-F0-9]{40})");
        std::regex name_regex("dn=([^&]+)");
        std::regex tr_regex("tr=([^&]+)");

        std::smatch match;
        if (std::regex_search(magnet_url, match, hash_regex)) {
            result.hash = match[1].str();
        }

        if (std::regex_search(magnet_url, match, name_regex)) {
            result.title = url_decode(match[1].str());
        }

        return result;
    }

    double calculate_confidence(const SearchResult& result) {
        double confidence = 0.5;

        // 根据种子数调整置信度
        if (result.seeds > 100) confidence += 0.3;
        else if (result.seeds > 50) confidence += 0.2;
        else if (result.seeds > 10) confidence += 0.1;

        // 根据文件大小调整
        if (result.size > 100 * 1024 * 1024 && result.size < 10ULL * 1024 * 1024 * 1024) {
            confidence += 0.1;
        }

        return std::min(confidence, 1.0);
    }

    std::string url_encode(const std::string& str) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : str) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << int((unsigned char)c);
                escaped << std::nouppercase;
            }
        }

        return escaped.str();
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

    void replace_all(std::string& str, const std::string& from, const std::string& to) {
        size_t pos = 0;
        while ((pos = str.find(from, pos)) != std::string::npos) {
            str.replace(pos, from.length(), to);
            pos += to.length();
        }
    }

    size_t parse_size(const std::string& size_str) {
        std::regex size_regex("(\\d+(?:\\.\\d+)?)\\s*(B|KB|MB|GB|TB)", std::regex::icase);
        std::smatch match;

        if (std::regex_search(size_str, match, size_regex)) {
            double value = std::stod(match[1].str());
            std::string unit = match[2].str();

            std::transform(unit.begin(), unit.end(), unit.begin(), ::toupper);

            if (unit == "KB") value *= 1024;
            else if (unit == "MB") value *= 1024 * 1024;
            else if (unit == "GB") value *= 1024 * 1024 * 1024;
            else if (unit == "TB") value *= 1024ULL * 1024 * 1024 * 1024;

            return static_cast<size_t>(value);
        }

        return 0;
    }
};

/**
 * @brief ResourceSearchManager实现
 */
class ResourceSearchManager::Impl {
public:
    std::vector<std::unique_ptr<ISearchProvider>> providers_;
    std::map<std::string, SearchEngineConfig> engine_configs_;
    int global_delay_ms_ = 2000;
    bool proxy_enabled_ = false;
    std::string proxy_host_;
    int proxy_port_ = 8080;
    std::string proxy_type_ = "http";
    std::string proxy_username_;
    std::string proxy_password_;

    bool load_config(const std::string& config_file) {
        std::ifstream file(config_file);
        if (!file.is_open()) {
            FALCON_LOG_WARN("Failed to open search engine config file: " << config_file);
            return false;
        }

        try {
            json config = json::parse(file);

            // 加载全局设置
            if (config.contains("global_settings")) {
                auto& global = config["global_settings"];
                global_delay_ms_ = global.value("default_delay_ms", 2000);

                if (global.contains("proxy")) {
                    auto& proxy = global["proxy"];
                    proxy_enabled_ = proxy.value("enabled", false);
                    if (proxy_enabled_) {
                        proxy_host_ = proxy.value("host", "");
                        proxy_port_ = proxy.value("port", 8080);
                        proxy_type_ = proxy.value("type", "http");
                        proxy_username_ = proxy.value("username", "");
                        proxy_password_ = proxy.value("password", "");
                    }
                }
            }

            // 加载搜索引擎配置
            if (config.contains("search_engines")) {
                for (const auto& engine : config["search_engines"]) {
                    SearchEngineConfig engine_config;
                    engine_config.name = engine["name"].get<std::string>();
                    engine_config.base_url = engine["base_url"].get<std::string>();
                    engine_config.search_path = engine.value("search_path", "");
                    engine_config.enabled = engine.value("enabled", true);
                    engine_config.weight = engine.value("weight", 1.0);
                    engine_config.encoding = engine.value("encoding", "utf-8");
                    engine_config.delay_ms = engine.value("delay_ms", 2000);

                    if (engine.contains("headers")) {
                        for (auto& [key, value] : engine["headers"].items()) {
                            engine_config.headers[key] = value.get<std::string>();
                        }
                    }

                    if (engine.contains("params")) {
                        for (auto& [key, value] : engine["params"].items()) {
                            engine_config.params[key] = value.get<std::string>();
                        }
                    }

                    // TODO: 添加 response_format, selectors, path_pattern 成员
                    // if (engine.contains("response_format")) {
                    //     engine_config.response_format = engine["response_format"].get<std::string>();
                    // }
                    //
                    // if (engine.contains("selectors")) {
                    //     for (auto& [key, value] : engine["selectors"].items()) {
                    //         engine_config.selectors[key] = value.get<std::string>();
                    //     }
                    // }
                    //
                    // if (engine.contains("path_pattern")) {
                    //     engine_config.path_pattern = engine["path_pattern"].get<std::string>();
                    // }

                    engine_configs_[engine_config.name] = engine_config;

                    // 创建搜索引擎实例
                    if (engine_config.enabled) {
                        auto provider = std::make_unique<GenericSearchProvider>(engine_config);

                        // 设置代理
                        if (proxy_enabled_ && !proxy_host_.empty()) {
                            std::string proxy_url = proxy_type_ + "://" + proxy_host_ + ":" +
                                                 std::to_string(proxy_port_);
                            // provider->set_proxy(proxy_url, proxy_username_, proxy_password_, proxy_type_);
                        }

                        providers_.push_back(std::move(provider));
                    }
                }
            }

            FALCON_LOG_INFO("Loaded " << providers_.size() << " search engines");
            return true;

        } catch (const std::exception& e) {
            FALCON_LOG_ERROR("Failed to parse search engine config: " << e.what());
            return false;
        }
    }
};

ResourceSearchManager::ResourceSearchManager()
    : p_impl(std::make_unique<Impl>()) {}

ResourceSearchManager::~ResourceSearchManager() = default;

void ResourceSearchManager::register_provider(std::unique_ptr<ISearchProvider> provider) {
    if (provider) {
        p_impl->providers_.push_back(std::move(provider));
    }
}

bool ResourceSearchManager::load_config(const std::string& config_file) {
    return p_impl->load_config(config_file);
}

std::vector<SearchResult> ResourceSearchManager::search_all(const SearchQuery& query) {
    std::vector<SearchResult> all_results;
    std::vector<std::future<std::vector<SearchResult>>> futures;

    // 并发搜索所有启用的搜索引擎
    for (auto& provider : p_impl->providers_) {
        if (provider->is_available()) {
            auto future = std::async(std::launch::async, [&provider, query]() {
                return provider->search(query);
            });
            futures.push_back(std::move(future));

            // 添加延迟以避免被封禁
            std::this_thread::sleep_for(std::chrono::milliseconds(provider->get_delay()));
        }
    }

    // 收集结果
    for (auto& future : futures) {
        try {
            auto results = future.get();
            all_results.insert(all_results.end(),
                             std::make_move_iterator(results.begin()),
                             std::make_move_iterator(results.end()));
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR("Search error: " << e.what());
        }
    }

    // 去重（基于哈希值或URL）
    std::unordered_set<std::string> seen_hashes;
    std::vector<SearchResult> unique_results;

    for (auto& result : all_results) {
        std::string key = result.hash.empty() ? result.url : result.hash;
        if (seen_hashes.insert(key).second) {
            unique_results.push_back(std::move(result));
        }
    }

    // 按置信度排序
    std::sort(unique_results.begin(), unique_results.end(),
        [](const SearchResult& a, const SearchResult& b) {
            return a.confidence > b.confidence;
        });

    // 限制结果数量
    if (unique_results.size() > query.limit) {
        unique_results.resize(query.limit);
    }

    return unique_results;
}

std::vector<SearchResult> ResourceSearchManager::search_providers(
    const SearchQuery& query,
    const std::vector<std::string>& provider_names) {

    std::vector<SearchResult> results;

    for (auto& provider : p_impl->providers_) {
        if (std::find(provider_names.begin(), provider_names.end(),
                     provider->name()) != provider_names.end()) {
            auto provider_results = provider->search(query);
            results.insert(results.end(),
                         std::make_move_iterator(provider_results.begin()),
                         std::make_move_iterator(provider_results.end()));
        }
    }

    return results;
}

std::vector<std::string> ResourceSearchManager::get_providers() const {
    std::vector<std::string> names;
    for (const auto& provider : p_impl->providers_) {
        names.push_back(provider->name());
    }
    return names;
}

std::vector<std::string> ResourceSearchManager::get_suggestions(const std::string& keyword) {
    // 简单的自动补全实现
    // 实际项目中可以从历史记录或搜索引擎API获取建议
    std::vector<std::string> suggestions;

    // 常见搜索建议
    std::vector<std::string> common_suffixes = {
        " 1080p", " 720p", " 4K", " HDR", " BluRay", " WEB-DL",
        " HDTV", " x264", " x265", " 10bit", " movie", " series",
        " season", " complete", " subs", " english", " chinese"
    };

    for (const auto& suffix : common_suffixes) {
        suggestions.push_back(keyword + suffix);
    }

    return suggestions;
}

void ResourceSearchManager::set_global_delay(int delay_ms) {
    p_impl->global_delay_ms_ = delay_ms;
}

void ResourceSearchManager::enable_provider(const std::string& name, bool enabled) {
    auto it = p_impl->engine_configs_.find(name);
    if (it != p_impl->engine_configs_.end()) {
        it->second.enabled = enabled;
    }
}

} // namespace search
} // namespace falcon