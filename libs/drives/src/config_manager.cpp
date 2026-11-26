/**
 * @file config_manager.cpp
 * @brief 加密配置门面：所有 SQLite/OpenSSL/JSON 实现由 config_storage 插件提供
 *
 * 门面本身不链接 SQLite3、OpenSSL 或 nlohmann_json，只做 C ABI 编组。
 */
#include <falcon/drives/config_manager.hpp>
#include <falcon/drives/plugin_loader.hpp>
#include <falcon/abi/marshal.hpp>

#include <utility>

namespace falcon {

namespace {

falcon_cloud_config to_abi(const CloudStorageConfig& config,
                           std::vector<falcon_kv>& extra_storage) {
    falcon_cloud_config raw{};
    raw.name = abi::view(config.name);
    raw.provider = abi::view(config.provider);
    raw.access_key = abi::view(config.access_key);
    raw.secret_key = abi::view(config.secret_key);
    raw.region = abi::view(config.region);
    raw.bucket = abi::view(config.bucket);
    raw.endpoint = abi::view(config.endpoint);
    raw.custom_domain = abi::view(config.custom_domain);
    extra_storage = abi::to_kv(config.extra);
    raw.extra.items = extra_storage.empty() ? nullptr : extra_storage.data();
    raw.extra.count = static_cast<uint32_t>(extra_storage.size());
    raw.created_at = static_cast<int64_t>(config.created_at);
    raw.updated_at = static_cast<int64_t>(config.updated_at);
    return raw;
}

CloudStorageConfig from_abi(const falcon_cloud_config& raw) {
    CloudStorageConfig config;
    config.name = abi::copy(raw.name);
    config.provider = abi::copy(raw.provider);
    config.access_key = abi::copy(raw.access_key);
    config.secret_key = abi::copy(raw.secret_key);
    config.region = abi::copy(raw.region);
    config.bucket = abi::copy(raw.bucket);
    config.endpoint = abi::copy(raw.endpoint);
    config.custom_domain = abi::copy(raw.custom_domain);
    config.extra = abi::from_kv(raw.extra.items, raw.extra.count);
    config.created_at = static_cast<time_t>(raw.created_at);
    config.updated_at = static_cast<time_t>(raw.updated_at);
    return config;
}

} // namespace

class ConfigManager::Impl {
public:
    Impl() {
        service_ = drives::PluginLoader::instance().config_storage();
        if (!service_ || !service_->create) return;
        handle_ = service_->create(service_->service_data);
    }

    ~Impl() {
        if (service_ && handle_ && service_->destroy) {
            service_->destroy(service_->service_data, handle_);
        }
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    bool ready() const { return service_ && handle_; }

    const falcon_config_service* service_ = nullptr;
    void* handle_ = nullptr;
};

ConfigManager::ConfigManager() : p_impl_(std::make_unique<Impl>()) {}
ConfigManager::~ConfigManager() = default;

bool ConfigManager::initialize(const std::string& db_path,
                               const std::string& master_password) {
    if (!p_impl_->ready() || !p_impl_->service_->initialize) return false;
    return p_impl_->service_->initialize(p_impl_->handle_, abi::view(db_path),
                                         abi::view(master_password));
}

bool ConfigManager::set_master_password(const std::string& password) {
    if (!p_impl_->ready() || !p_impl_->service_->set_master_password) return false;
    return p_impl_->service_->set_master_password(p_impl_->handle_, abi::view(password));
}

bool ConfigManager::verify_master_password(const std::string& password) {
    if (!p_impl_->ready() || !p_impl_->service_->verify_master_password) return false;
    return p_impl_->service_->verify_master_password(p_impl_->handle_, abi::view(password));
}

bool ConfigManager::save_cloud_config(const CloudStorageConfig& config) {
    if (!p_impl_->ready() || !p_impl_->service_->save_cloud_config) return false;
    std::vector<falcon_kv> extra;
    falcon_cloud_config raw = to_abi(config, extra);
    return p_impl_->service_->save_cloud_config(p_impl_->handle_, &raw);
}

bool ConfigManager::get_cloud_config(const std::string& name, CloudStorageConfig& config) {
    if (!p_impl_->ready() || !p_impl_->service_->get_cloud_config) return false;
    falcon_cloud_config raw{};
    if (!p_impl_->service_->get_cloud_config(p_impl_->handle_, abi::view(name), &raw)) {
        return false;
    }
    config = from_abi(raw);
    if (p_impl_->service_->free_configs) {
        falcon_cloud_configs owned{&raw, 1};
        p_impl_->service_->free_configs(p_impl_->handle_, &owned);
    }
    return true;
}

bool ConfigManager::delete_cloud_config(const std::string& name) {
    if (!p_impl_->ready() || !p_impl_->service_->delete_cloud_config) return false;
    return p_impl_->service_->delete_cloud_config(p_impl_->handle_, abi::view(name));
}

std::vector<std::string> ConfigManager::list_cloud_configs() {
    if (!p_impl_->ready() || !p_impl_->service_->list_cloud_configs) return {};
    const uint32_t count = p_impl_->service_->list_cloud_configs(
        p_impl_->handle_, nullptr, 0);
    if (!count) return {};
    std::vector<falcon_str> buffer(count);
    const uint32_t written = p_impl_->service_->list_cloud_configs(
        p_impl_->handle_, buffer.data(), count);
    std::vector<std::string> out;
    out.reserve(written);
    for (uint32_t i = 0; i < written; ++i) out.push_back(abi::copy(buffer[i]));
    return out;
}

std::vector<CloudStorageConfig> ConfigManager::search_configs(const std::string& provider) {
    if (!p_impl_->ready() || !p_impl_->service_->search_configs) return {};
    falcon_cloud_configs raw{};
    p_impl_->service_->search_configs(p_impl_->handle_, abi::view(provider), &raw);
    std::vector<CloudStorageConfig> out;
    out.reserve(raw.count);
    if (raw.items) {
        for (uint32_t i = 0; i < raw.count; ++i) out.push_back(from_abi(raw.items[i]));
    }
    if (p_impl_->service_->free_configs) {
        p_impl_->service_->free_configs(p_impl_->handle_, &raw);
    }
    return out;
}

bool ConfigManager::update_cloud_config(const std::string& name,
                                        const CloudStorageConfig& config) {
    if (!p_impl_->ready() || !p_impl_->service_->update_cloud_config) return false;
    std::vector<falcon_kv> extra;
    falcon_cloud_config raw = to_abi(config, extra);
    return p_impl_->service_->update_cloud_config(p_impl_->handle_, abi::view(name), &raw);
}

bool ConfigManager::export_configs(const std::string& export_path,
                                   const std::string& export_password) {
    if (!p_impl_->ready() || !p_impl_->service_->export_configs) return false;
    return p_impl_->service_->export_configs(p_impl_->handle_, abi::view(export_path),
                                             abi::view(export_password));
}

bool ConfigManager::import_configs(const std::string& import_path,
                                   const std::string& import_password) {
    if (!p_impl_->ready() || !p_impl_->service_->import_configs) return false;
    return p_impl_->service_->import_configs(p_impl_->handle_, abi::view(import_path),
                                             abi::view(import_password));
}

} // namespace falcon
