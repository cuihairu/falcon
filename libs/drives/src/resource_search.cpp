/**
 * @file resource_search.cpp
 * @brief 资源搜索门面：调度/去重/排序留在门面，网络与 JSON 解析由插件提供
 *
 * 门面不链接 curl / nlohmann_json。所有跨界数据经稳定 C ABI 编组。
 */
#include <falcon/drives/resource_search.hpp>
#include <falcon/drives/plugin_loader.hpp>
#include <falcon/abi/marshal.hpp>
#include <falcon/logger.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <iterator>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace falcon {
namespace search {

namespace {

std::vector<falcon_kv> kv_from_kv_array(const falcon_kv_array& array) {
    std::vector<falcon_kv> out;
    out.reserve(array.count);
    if (array.items) {
        for (uint32_t i = 0; i < array.count; ++i) out.push_back(array.items[i]);
    }
    return out;
}

SearchEngineConfig engine_from_abi(const falcon_search_engine_config& raw) {
    SearchEngineConfig config;
    config.name = abi::copy(raw.name);
    config.base_url = abi::copy(raw.base_url);
    config.search_path = abi::copy(raw.search_path);
    if (raw.headers) {
        for (uint32_t i = 0; i < raw.header_count; ++i) {
            config.headers[abi::copy(raw.headers[i].key)] = abi::copy(raw.headers[i].value);
        }
    }
    if (raw.params) {
        for (uint32_t i = 0; i < raw.param_count; ++i) {
            config.params[abi::copy(raw.params[i].key)] = abi::copy(raw.params[i].value);
        }
    }
    config.encoding = abi::copy(raw.encoding);
    config.delay_ms = raw.delay_ms;
    config.enabled = raw.enabled;
    config.weight = raw.weight;
    config.response_format = abi::copy(raw.response_format);
    if (raw.selectors) {
        for (uint32_t i = 0; i < raw.selector_count; ++i) {
            config.selectors[abi::copy(raw.selectors[i].key)] =
                abi::copy(raw.selectors[i].value);
        }
    }
    config.path_pattern = abi::copy(raw.path_pattern);
    return config;
}

SearchResult result_from_abi(const falcon_search_result& raw) {
    SearchResult result;
    result.title = abi::copy(raw.title);
    result.url = abi::copy(raw.url);
    result.source = abi::copy(raw.source);
    result.size = static_cast<size_t>(raw.size);
    result.type = abi::copy(raw.type);
    result.seeds = raw.seeds;
    result.peers = raw.peers;
    result.confidence = raw.confidence;
    result.hash = abi::copy(raw.hash);
    result.publish_date = abi::copy(raw.publish_date);
    if (raw.metadata.items) {
        for (uint32_t i = 0; i < raw.metadata.count; ++i) {
            result.metadata[abi::copy(raw.metadata.items[i].key)] =
                abi::copy(raw.metadata.items[i].value);
        }
    }
    return result;
}

/** 由 C ABI 服务支撑的搜索提供者。 */
class PluginSearchProvider final : public ISearchProvider {
public:
    PluginSearchProvider(const falcon_search_service* service,
                         SearchEngineConfig config)
        : service_(service), config_(std::move(config)) {
        // raw_ 的字符串视图必须指向本对象持有的稳定存储（config_ 成员），
        // 不能指向调用方的临时 map——后者在 load_config 循环结束后即销毁。
        header_kv_ = abi::to_kv(config_.headers);
        param_kv_ = abi::to_kv(config_.params);
        selector_kv_ = abi::to_kv(config_.selectors);
        raw_.name = abi::view(config_.name);
        raw_.base_url = abi::view(config_.base_url);
        raw_.search_path = abi::view(config_.search_path);
        raw_.headers = header_kv_.empty() ? nullptr : header_kv_.data();
        raw_.header_count = static_cast<uint32_t>(header_kv_.size());
        raw_.params = param_kv_.empty() ? nullptr : param_kv_.data();
        raw_.param_count = static_cast<uint32_t>(param_kv_.size());
        raw_.encoding = abi::view(config_.encoding);
        raw_.delay_ms = config_.delay_ms;
        raw_.enabled = config_.enabled;
        raw_.weight = config_.weight;
        raw_.response_format = abi::view(config_.response_format);
        raw_.selectors = selector_kv_.empty() ? nullptr : selector_kv_.data();
        raw_.selector_count = static_cast<uint32_t>(selector_kv_.size());
        raw_.path_pattern = abi::view(config_.path_pattern);
    }

    std::string name() const override { return config_.name; }

    std::vector<SearchResult> search(const SearchQuery& query) override {
        if (!service_ || !service_->search_one || !service_->free_results) return {};
        falcon_search_query q{};
        q.keyword = abi::view(query.keyword);
        q.category = abi::view(query.category);
        q.min_size = query.min_size;
        q.max_size = query.max_size;
        q.min_seeds = query.min_seeds;
        q.limit = query.limit;
        q.page = query.page;
        q.sort_by = abi::view(query.sort_by);
        q.sort_desc = query.sort_desc;

        falcon_search_results raw{};
        service_->search_one(service_->service_data, &raw_, &q, &raw);
        std::vector<SearchResult> out;
        if (raw.items) {
            out.reserve(raw.count);
            for (uint32_t i = 0; i < raw.count; ++i) out.push_back(result_from_abi(raw.items[i]));
        }
        service_->free_results(service_->service_data, &raw);
        return out;
    }

    bool is_available() override {
        if (!service_ || !service_->is_available) return false;
        return config_.enabled && service_->is_available(service_->service_data, &raw_);
    }

    int get_delay() const override { return config_.delay_ms; }

    void set_config(SearchEngineConfig config) {
        config_ = std::move(config);
        raw_.enabled = config_.enabled;
    }

    const SearchEngineConfig& config() const { return config_; }

private:
    const falcon_search_service* service_ = nullptr;
    SearchEngineConfig config_;
    std::vector<falcon_kv> header_kv_;
    std::vector<falcon_kv> param_kv_;
    std::vector<falcon_kv> selector_kv_;
    falcon_search_engine_config raw_{};
};

} // namespace

// ============================================================================
// ResourceSearchManager
// ============================================================================

class ResourceSearchManager::Impl {
public:
    Impl() : service_(drives::PluginLoader::instance().resource_search()) {
        if (!service_) {
            FALCON_LOG_WARN_STREAM("资源搜索插件未加载，load_config 将返回 false");
        }
    }

    bool load_config(const std::string& config_file) {
        if (!service_ || !service_->load_config || !service_->free_engines) return false;
        falcon_search_engines raw{};
        if (!service_->load_config(service_->service_data, abi::view(config_file), &raw)) {
            return false;
        }

        providers_.clear();
        engine_configs_.clear();
        if (raw.items) {
            for (uint32_t i = 0; i < raw.count; ++i) {
                SearchEngineConfig config = engine_from_abi(raw.items[i]);
                engine_configs_[config.name] = config;
                if (config.enabled) {
                    providers_.push_back(std::make_unique<PluginSearchProvider>(
                        service_, std::move(config)));
                }
            }
        }
        service_->free_engines(service_->service_data, &raw);

        FALCON_LOG_INFO_STREAM("Loaded " << providers_.size() << " search engines");
        return true;
    }

    std::vector<std::unique_ptr<ISearchProvider>> providers_;
    std::map<std::string, SearchEngineConfig> engine_configs_;
    int global_delay_ms_ = 2000;

private:
    const falcon_search_service* service_ = nullptr;
};

ResourceSearchManager::ResourceSearchManager()
    : p_impl(std::make_unique<Impl>()) {}

ResourceSearchManager::~ResourceSearchManager() = default;

void ResourceSearchManager::register_provider(std::unique_ptr<ISearchProvider> provider) {
    if (provider) p_impl->providers_.push_back(std::move(provider));
}

bool ResourceSearchManager::load_config(const std::string& config_file) {
    return p_impl->load_config(config_file);
}

std::vector<SearchResult> ResourceSearchManager::search_all(const SearchQuery& query) {
    std::vector<SearchResult> all_results;
    std::vector<std::future<std::vector<SearchResult>>> futures;

    for (auto& provider : p_impl->providers_) {
        if (provider->is_available()) {
            auto future = std::async(std::launch::async, [&provider, query]() {
                return provider->search(query);
            });
            futures.push_back(std::move(future));
            std::this_thread::sleep_for(std::chrono::milliseconds(provider->get_delay()));
        }
    }

    for (auto& future : futures) {
        try {
            auto results = future.get();
            all_results.insert(all_results.end(),
                std::make_move_iterator(results.begin()),
                std::make_move_iterator(results.end()));
        } catch (const std::exception& e) {
            FALCON_LOG_ERROR_STREAM("Search error: " << e.what());
        }
    }

    std::unordered_set<std::string> seen_keys;
    std::vector<SearchResult> unique_results;
    for (auto& result : all_results) {
        const std::string key = result.hash.empty() ? result.url : result.hash;
        if (seen_keys.insert(key).second) unique_results.push_back(std::move(result));
    }

    std::sort(unique_results.begin(), unique_results.end(),
        [&](const SearchResult& a, const SearchResult& b) {
            if (query.sort_by == "size") {
                return query.sort_desc ? a.size > b.size : a.size < b.size;
            }
            if (query.sort_by == "seeds") {
                return query.sort_desc ? a.seeds > b.seeds : a.seeds < b.seeds;
            }
            if (query.sort_by == "date") {
                return query.sort_desc ? a.publish_date > b.publish_date
                                       : a.publish_date < b.publish_date;
            }
            return a.confidence > b.confidence;
        });

    if (unique_results.size() > query.limit) unique_results.resize(query.limit);
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
    for (const auto& provider : p_impl->providers_) names.push_back(provider->name());
    return names;
}

std::vector<std::string> ResourceSearchManager::get_suggestions(const std::string& keyword) {
    static const std::vector<std::string> common_suffixes = {
        " 1080p", " 720p", " 4K", " HDR", " BluRay", " WEB-DL",
        " HDTV", " x264", " x265", " 10bit", " movie", " series",
        " season", " complete", " subs", " english", " chinese"
    };
    std::vector<std::string> suggestions;
    for (const auto& suffix : common_suffixes) suggestions.push_back(keyword + suffix);
    return suggestions;
}

void ResourceSearchManager::set_global_delay(int delay_ms) {
    p_impl->global_delay_ms_ = delay_ms;
}

void ResourceSearchManager::enable_provider(const std::string& name, bool enabled) {
    auto it = p_impl->engine_configs_.find(name);
    if (it != p_impl->engine_configs_.end()) it->second.enabled = enabled;
}

} // namespace search
} // namespace falcon
