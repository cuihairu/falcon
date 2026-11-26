/**
 * @file resource_search.cpp
 * @brief 资源搜索插件具体实现（从 libs/drives 搬迁）
 *
 * nlohmann_json 只存在于本插件 so 中；HTTP 统一走宿主注入的
 * falcon_host_api，插件不再自带网络栈。detail::* 纯解析函数由
 * 共享源文件 search_detail.cpp 提供。
 */
#include <falcon/abi/marshal.hpp>
#include <falcon/drives/resource_search.hpp>
#include <falcon/detail/injection.hpp>
#include <falcon/logger.hpp>
#ifdef HAVE_NLOHMANN_JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;
#endif
#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace falcon {
namespace search {

namespace {

/** 由 falcon_plugin_set_host 注入；生命周期归宿主，插件只读取。 */
const falcon_host_api*& search_host_slot() {
    static const falcon_host_api* api = nullptr;
    return api;
}

std::vector<falcon_http_header> to_http_headers(
    const std::map<std::string, std::string>& headers) {
    std::vector<falcon_http_header> out;
    out.reserve(headers.size());
    for (const auto& entry : headers) {
        falcon_http_header header{};
        header.name = falcon::abi::view(entry.first);
        header.value = falcon::abi::view(entry.second);
        out.push_back(header);
    }
    return out;
}

/** 复制 body 后按契约用宿主分配器释放 body/error，避免跨 CRT 释放。 */
std::string take_body(falcon_http_response& response, const falcon_host_api* host) {
    std::string body = falcon::abi::copy(response.body);
    if (host->deallocate) {
        if (response.body.data) {
            host->deallocate(host->host_data, const_cast<char*>(response.body.data));
        }
        if (response.error.data) {
            host->deallocate(host->host_data, const_cast<char*>(response.error.data));
        }
    }
    return body;
}

} // namespace

void set_search_host_api(const falcon_host_api* host) {
    search_host_slot() = host;
}

const falcon_host_api* search_host_api() {
    return search_host_slot();
}

/**
 * @brief Web爬虫实现（HTTP 由宿主服务表提供）
 */
class WebCrawler {
public:
    WebCrawler() {
        // 旧实现里 CurlEasyInit 注入命中会短路句柄创建并使构造失败；
        // 现在网络栈归宿主，改由「宿主服务表未注入」走同一条短路/失败
        // 路径，保持故障注入测试语义（provider 构造失败）。
        if (::falcon::detail::inject_failure(
                ::falcon::detail::InjectPoint::CurlEasyInit) ||
            !search_host_api() || !search_host_api()->http_get) {
            throw std::runtime_error("Search host API is not available");
        }
    }

    ~WebCrawler() = default;

    // 代理交由宿主网络栈处理；保留接口签名，插件侧忽略
    void set_proxy(const std::string&, const std::string& = "",
                   const std::string& = "", const std::string& = "") {}

    void set_timeout(int timeout_seconds) { timeout_ = timeout_seconds; }

    void set_headers(const std::map<std::string, std::string>& headers) {
        headers_ = headers;
    }

    std::string get(const std::string& url) {
        // 状态先清零：上次请求的残留不得混入本次
        last_status_ = 0;
        const falcon_host_api* host = search_host_api();
        // 宿主未注入（或运行中失效）时短路：状态码 0、返回空串
        if (!host || !host->http_get) return "";

        const auto header_array = to_http_headers(headers_);
        falcon_http_response response{};
        host->http_get(host->host_data, falcon::abi::view(url),
                       header_array.empty() ? nullptr : header_array.data(),
                       static_cast<uint32_t>(header_array.size()),
                       timeout_, &response);
        last_status_ = response.status_code;
        return take_body(response, host);
    }

    /// 最近一次 get() 的 HTTP 状态码（网络失败 = 0；供调用方区分
    /// 「站点不存在」与「站点活着但内容不是预期」——比正文嗅探可靠）
    long last_status() const { return last_status_; }

private:
    std::map<std::string, std::string> headers_;
    int timeout_ = 30;
    long last_status_ = 0;
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
        FALCON_LOG_DEBUG_STREAM("Searching " << config_.name << " with query: " << query.keyword);
        std::string response = crawler_->get(search_url);

        if (response.empty()) {
            FALCON_LOG_WARN_STREAM("No response from " << config_.name);
            return results;
        }

        // 解析响应
        if (config_.response_format == "json") {
            results = parse_json_response(response);
        } else {
            results = parse_html_response(response);
        }

        // 应用过滤
        results = filter_results(results, query);

        FALCON_LOG_DEBUG_STREAM("Found " << results.size() << " results from " << config_.name);
        return results;
    }

    bool is_available() override {
        std::string test_url = config_.base_url;
        std::string response = crawler_->get(test_url);
        long status = crawler_->last_status();

        // 可用性按 HTTP 状态判定，不再嗅探正文（旧实现「正文含 404
        // 即不可用」会把根页提到 404 字样的正常站点误杀，且对根路径
        // 真实 404 的 API 型站点反而放行）。
        if (status == 0) {
            FALCON_LOG_WARN_STREAM("Search engine " << config_.name
                                 << " unreachable: " << config_.base_url);
            return false;
        }
        if (status >= 400) {
            FALCON_LOG_WARN_STREAM("Search engine " << config_.name
                                 << " base_url returned HTTP " << status
                                 << ", skipped: " << config_.base_url);
            return false;
        }
        (void)response; // 正文不再参与判定（2xx/3xx 即视为可达）
        return true;
    }

    int get_delay() const override {
        return config_.delay_ms;
    }

protected:
    virtual std::string build_search_url(const SearchQuery& query) {
        std::string url;

        // 如果配置了 path_pattern，使用路径模式
        if (!config_.path_pattern.empty()) {
            std::string pattern = config_.path_pattern;

            // 获取查询首字母
            std::string query_letter = query.keyword.empty() ? "all" :
                                     std::string(1, static_cast<char>(std::tolower(query.keyword[0])));
            std::string first_char = std::isalpha(query_letter[0]) ? "1" : "0";

            // 替换路径模式中的占位符
            pattern = replace_all(pattern, "{query}", query.keyword);
            pattern = replace_all(pattern, "{query_letter}", query_letter);
            pattern = replace_all(pattern, "{first_char}", first_char);
            pattern = replace_all(pattern, "{page}", std::to_string(query.page));
            pattern = replace_all(pattern, "{encoded_query}", url_encode(query.keyword));

            url = config_.base_url + pattern;
        } else {
            url = config_.base_url + config_.search_path;
        }

        // 添加查询参数
        bool first_param = url.find('?') == std::string::npos;

        // 构建查询参数
        std::string query_str;
        for (const auto& [key, value] : config_.params) {
            if (!query_str.empty()) query_str += "&";

            std::string param_value = value;
            if (value.empty() && (key == "search" || key == "q" || key == "keyword")) {
                param_value = query.keyword;
            }

            query_str += key + "=" + url_encode(param_value);
        }

        if (!query_str.empty()) {
            url += (first_param ? "?" : "&") + query_str;
        }

        return url;
    }

    virtual std::vector<SearchResult> parse_html_response(const std::string& html) {
        // 优先：selectors 配置驱动（通用解析流程）
        return detail::parse_html_by_selectors(html, config_);
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

    // ---- 宽容取值助手：站点 JSON 字段类型不可控（字符串数字/浮点
    // size/null 混杂常见），单个字段类型错误只损失该字段（落缺省值），
    // 绝不连坐丢弃该引擎全部结果
    static std::string json_str(const nlohmann::json& item, const char* key) {
        if (!item.contains(key)) return {};
        const auto& v = item[key];
        if (v.is_string()) return v.get<std::string>();
        if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
        if (v.is_number_integer()) return std::to_string(v.get<long long>());
        if (v.is_number_float()) return std::to_string(v.get<double>());
        return {};
    }

    static int json_int(const nlohmann::json& item, const char* key) {
        if (!item.contains(key)) return 0;
        const auto& v = item[key];
        try {
            if (v.is_number_unsigned()) {
                uint64_t n = v.get<uint64_t>();
                return n <= 2147483647ull ? static_cast<int>(n) : 0;
            }
            if (v.is_number_integer()) return v.get<int>();
            if (v.is_number_float()) return static_cast<int>(v.get<double>());
            if (v.is_string()) return std::stoi(v.get<std::string>());
        } catch (const std::exception&) {
            // 垃圾文本 → 缺省 0
        }
        return 0;
    }

    static uint64_t json_size(const nlohmann::json& item, const char* key) {
        if (!item.contains(key)) return 0;
        const auto& v = item[key];
        if (v.is_number_unsigned()) return v.get<uint64_t>();
        if (v.is_number_integer()) {
            long long n = v.get<long long>();
            return n > 0 ? static_cast<uint64_t>(n) : 0;
        }
        if (v.is_number_float()) {
            double n = v.get<double>();
            return (n > 0 && n < 1.8e19) ? static_cast<uint64_t>(n) : 0;
        }
        // "1.2 GB" 类人类可读尺寸走 detail::parse_size（HTML 路径同款）
        if (v.is_string()) return detail::parse_size(v.get<std::string>());
        return 0;
    }

    static SearchResult extract_json_item(const nlohmann::json& item) {
        SearchResult result;
        result.title = json_str(item, "title");
        result.url = json_str(item, "url");
        result.hash = json_str(item, "hash");
        result.size = json_size(item, "size");
        result.seeds = json_int(item, "seeds");
        result.peers = json_int(item, "leeches");
        result.type = json_str(item, "type");
        result.source = json_str(item, "source");

        // magnet 覆盖 url，但仅当站点真给了非空 magnet——空串不得把
        // 已解析的 url 打掉
        const std::string magnet = json_str(item, "magnet");
        if (!magnet.empty()) {
            result.url = magnet;
        }
        return result;
    }

    std::vector<SearchResult> parse_json_response(const std::string& json_text) {
        std::vector<SearchResult> results;

        try {
            nlohmann::json j = nlohmann::json::parse(json_text);

            // 三种站点形态同一条宽容提取路径：{"results":[...]} / 顶层
            // 数组 / 单对象
            const nlohmann::json* items = nullptr;
            if (j.contains("results") && j["results"].is_array()) {
                items = &j["results"];
            } else if (j.is_array()) {
                items = &j;
            }

            if (items != nullptr) {
                for (const auto& item : *items) {
                    SearchResult result = extract_json_item(item);
                    if (!result.title.empty() && !result.url.empty()) {
                        results.push_back(std::move(result));
                    }
                }
            } else if (j.contains("title") && j.contains("url")) {
                SearchResult result = extract_json_item(j);
                if (!result.title.empty() && !result.url.empty()) {
                    results.push_back(std::move(result));
                }
            }

            // 来源缺省 = 引擎名（与 HTML 路径 result.source = config.name 同
            // 语义——发现页「来源」列按引擎显示；站点自带 source 字段仍优先）
            for (auto& r : results) {
                if (r.source.empty()) {
                    r.source = config_.name;
                }
            }
        } catch (const nlohmann::json::parse_error& e) {
            FALCON_LOG_ERROR_STREAM("JSON解析失败: " << e.what());
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR_STREAM("JSON处理失败: " << e.what());
        }

        return results;
    }

    static double calculate_confidence(const SearchResult& result) {
        return detail::calculate_confidence(result);
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

    // 辅助函数：字符串替换
    static std::string replace_all(std::string str, const std::string& from, const std::string& to) {
        size_t start_pos = 0;
        while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
            str.replace(start_pos, from.length(), to);
            start_pos += to.length();
        }
        return str;
    }

    static size_t parse_size(const std::string& size_str) {
        return detail::parse_size(size_str);
    }
};

} // namespace search
} // namespace falcon

// ============================================================================
// SPI 工厂：由 spi_entry.cpp 调用，创建单引擎搜索提供者
// ============================================================================
namespace falcon {
namespace search {

std::unique_ptr<ISearchProvider> make_generic_search_provider(
    const SearchEngineConfig& config) {
    return std::make_unique<GenericSearchProvider>(config);
}

} // namespace search
} // namespace falcon
