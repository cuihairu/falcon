/**
 * @file spi_entry.cpp
 * @brief resource_search 插件稳定 C ABI 入口
 *
 * 唯一对外边界是 falcon/abi/drive_api.h 定义的 C ABI：
 *  - falcon_plugin_entry 返回静态 manifest（一个 RESOURCE_SEARCH 描述符）；
 *  - falcon_plugin_set_host 注入宿主服务表，HTTP 由宿主提供；
 *  - 所有跨界数据均为 POD；对外字符串/数组由宿主分配器经 StringArena 分配。
 */
#include <falcon/abi/drive_api.h>
#include <falcon/abi/marshal.hpp>
#include <falcon/abi/plugin_helpers.hpp>
#include <falcon/drives/resource_search.hpp>
#include <falcon/logger.hpp>

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace falcon {
namespace search {

// resource_search.cpp 内部工厂与宿主服务表注入槽
std::unique_ptr<ISearchProvider> make_generic_search_provider(
    const SearchEngineConfig& config);
void set_search_host_api(const falcon_host_api* host);

} // namespace search
} // namespace falcon

namespace {

namespace abi = falcon::abi;
using json = nlohmann::json;
using falcon::search::SearchEngineConfig;
using falcon::search::SearchQuery;
using falcon::search::SearchResult;

bool load_config_impl(const std::string& config_file,
                      std::vector<SearchEngineConfig>* out) {
    if (!out) return false;
    std::ifstream file(config_file);
    if (!file.is_open()) {
        FALCON_LOG_WARN_STREAM("Failed to open search engine config file: " << config_file);
        return false;
    }

    try {
        const json config = json::parse(file);
        if (config.contains("search_engines")) {
            for (const auto& engine : config["search_engines"]) {
                SearchEngineConfig ec;
                ec.name = engine.value("name", "");
                ec.base_url = engine.value("base_url", "");
                ec.search_path = engine.value("search_path", "");
                ec.enabled = engine.value("enabled", true);
                ec.weight = engine.value("weight", 1.0);
                ec.encoding = engine.value("encoding", "utf-8");
                ec.delay_ms = engine.value("delay_ms", 2000);

                if (engine.contains("headers")) {
                    for (auto& [key, value] : engine["headers"].items()) {
                        ec.headers[key] = value.get<std::string>();
                    }
                }
                if (engine.contains("params")) {
                    for (auto& [key, value] : engine["params"].items()) {
                        ec.params[key] = value.get<std::string>();
                    }
                }
                if (engine.contains("response_format")) {
                    ec.response_format = engine["response_format"].get<std::string>();
                }
                if (engine.contains("selectors")) {
                    for (auto& [key, value] : engine["selectors"].items()) {
                        ec.selectors[key] = value.get<std::string>();
                    }
                }
                if (engine.contains("path_pattern")) {
                    ec.path_pattern = engine["path_pattern"].get<std::string>();
                }

                if (!ec.name.empty()) out->push_back(std::move(ec));
            }
        }
        return true;
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR_STREAM("Failed to parse search engine config: " << e.what());
        return false;
    }
}

SearchEngineConfig engine_from_abi(const falcon_search_engine_config& raw) {
    SearchEngineConfig config;
    config.name = abi::copy(raw.name);
    config.base_url = abi::copy(raw.base_url);
    config.search_path = abi::copy(raw.search_path);
    config.headers = abi::from_kv(raw.headers, raw.header_count);
    config.params = abi::from_kv(raw.params, raw.param_count);
    config.encoding = abi::copy(raw.encoding);
    config.delay_ms = raw.delay_ms;
    config.enabled = raw.enabled;
    config.weight = raw.weight;
    config.response_format = abi::copy(raw.response_format);
    config.selectors = abi::from_kv(raw.selectors, raw.selector_count);
    config.path_pattern = abi::copy(raw.path_pattern);
    return config;
}

SearchQuery query_from_abi(const falcon_search_query& raw) {
    SearchQuery query;
    query.keyword = abi::copy(raw.keyword);
    query.category = abi::copy(raw.category);
    query.min_size = raw.min_size;
    query.max_size = raw.max_size;
    query.min_seeds = raw.min_seeds;
    query.limit = raw.limit;
    query.page = raw.page;
    query.sort_by = abi::copy(raw.sort_by);
    query.sort_desc = raw.sort_desc;
    return query;
}

// ==========================================================================
// falcon_search_service 的 5 个回调（签名全部为 C ABI POD）
// ==========================================================================

bool FALCON_CALL service_load_config(void*, falcon_str config_file,
                                     falcon_search_engines* out) {
    if (!out) return false;
    *out = falcon_search_engines{};

    std::vector<SearchEngineConfig> configs;
    if (!load_config_impl(abi::copy(config_file), &configs)) return false;
    if (configs.empty()) return true;

    abi::StringArena* arena = abi::ArenaRegistry::acquire(out);
    auto* items = arena->allocate<falcon_search_engine_config>(
        static_cast<uint32_t>(configs.size()));
    if (!items) {
        // 返回 false 时门面不会调用 free_engines，需自行回收登记项
        abi::ArenaRegistry::release(out);
        return false;
    }

    for (size_t i = 0; i < configs.size(); ++i) {
        const auto& src = configs[i];
        auto& dst = items[i];
        dst = falcon_search_engine_config{};
        dst.name = arena->dup(src.name);
        dst.base_url = arena->dup(src.base_url);
        dst.search_path = arena->dup(src.search_path);
        dst.headers = abi::fill_kv_array(arena, src.headers, dst.header_count);
        dst.params = abi::fill_kv_array(arena, src.params, dst.param_count);
        dst.encoding = arena->dup(src.encoding);
        dst.delay_ms = src.delay_ms;
        dst.enabled = src.enabled;
        dst.weight = src.weight;
        dst.response_format = arena->dup(src.response_format);
        dst.selectors = abi::fill_kv_array(arena, src.selectors, dst.selector_count);
        dst.path_pattern = arena->dup(src.path_pattern);
    }
    out->items = items;
    out->count = static_cast<uint32_t>(configs.size());
    return true;
}

void FALCON_CALL service_free_engines(void*, falcon_search_engines* engines) {
    if (!engines) return;
    abi::ArenaRegistry::release(engines);
    *engines = falcon_search_engines{};
}

void FALCON_CALL service_search_one(void*, const falcon_search_engine_config* config,
                                    const falcon_search_query* query,
                                    falcon_search_results* out) {
    if (!out) return;
    *out = falcon_search_results{};
    if (!config || !query) return;

    std::vector<SearchResult> results;
    try {
        auto provider = falcon::search::make_generic_search_provider(
            engine_from_abi(*config));
        if (provider) results = provider->search(query_from_abi(*query));
    } catch (const std::exception& e) {
        FALCON_LOG_ERROR_STREAM("search_one failed: " << e.what());
        return;
    } catch (...) {
        return;
    }
    if (results.empty()) return;

    abi::StringArena* arena = abi::ArenaRegistry::acquire(out);
    auto* items = arena->allocate<falcon_search_result>(
        static_cast<uint32_t>(results.size()));
    if (!items) return;

    for (size_t i = 0; i < results.size(); ++i) {
        const auto& src = results[i];
        auto& dst = items[i];
        dst = falcon_search_result{};
        dst.title = arena->dup(src.title);
        dst.url = arena->dup(src.url);
        dst.source = arena->dup(src.source);
        dst.size = static_cast<uint64_t>(src.size);
        dst.type = arena->dup(src.type);
        dst.seeds = src.seeds;
        dst.peers = src.peers;
        dst.confidence = src.confidence;
        dst.hash = arena->dup(src.hash);
        dst.publish_date = arena->dup(src.publish_date);
        dst.metadata.items = abi::fill_kv_array(arena, src.metadata, dst.metadata.count);
    }
    out->items = items;
    out->count = static_cast<uint32_t>(results.size());
}

void FALCON_CALL service_free_results(void*, falcon_search_results* results) {
    if (!results) return;
    abi::ArenaRegistry::release(results);
    *results = falcon_search_results{};
}

bool FALCON_CALL service_is_available(void*, const falcon_search_engine_config* config) {
    if (!config) return false;
    const auto engine = engine_from_abi(*config);
    if (!engine.enabled) return false;
    try {
        auto provider = falcon::search::make_generic_search_provider(engine);
        return provider && provider->is_available();
    } catch (...) {
        return false;
    }
}

// ==========================================================================
// 静态 manifest / descriptor / service（生命周期与 so 相同）
// ==========================================================================

const falcon_search_service kSearchService = {
    FALCON_PLUGIN_ABI_VERSION,
    nullptr, // service_data
    service_load_config,
    service_free_engines,
    service_search_one,
    service_free_results,
    service_is_available
};

const falcon_plugin_descriptor kSearchDescriptor = {
    FALCON_PLUGIN_ABI_VERSION,
    static_cast<uint32_t>(FALCON_SERVICE_RESOURCE_SEARCH),
    "builtin_search",
    "0.1.0",
    &kSearchService
};

const falcon_plugin_descriptor* const kDescriptors[] = {&kSearchDescriptor};

const falcon_plugin_manifest kManifest = {
    FALCON_PLUGIN_ABI_VERSION,
    "resource_search",
    "0.1.0",
    1u,
    kDescriptors
};

} // namespace

FALCON_PLUGIN_DEFINE_ENTRY_WITH_HOST(
    kManifest, ::falcon::search::set_search_host_api(host);)
