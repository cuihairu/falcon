/**
 * @file spi_entry.cpp
 * @brief config_storage 插件稳定 C ABI 入口
 *
 * 唯一对外边界是 falcon/abi/drive_api.h 定义的 C ABI：
 *  - falcon_plugin_entry 返回静态 manifest（一个 CONFIG_STORAGE 描述符）；
 *  - falcon_plugin_set_host 注入宿主服务表（本插件不用 HTTP，仅用于分配器）；
 *  - 所有跨界数据均为 POD；对外字符串/数组由宿主分配器经 StringArena 分配，
 *    门面调用 free_configs 时按输出地址登记的 arena 统一回收。
 *
 * SQLite / OpenSSL / nlohmann_json 依赖只存在于本 so 内部，边界不暴露。
 */
#include <falcon/abi/drive_api.h>
#include <falcon/abi/marshal.hpp>
#include <falcon/abi/plugin_helpers.hpp>
#include <falcon/drives/config_manager.hpp>
#include <falcon/detail/injection.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace falcon {

// config_manager.cpp 内的 opaque 桥接函数（实现保持不变）
void* config_storage_create();
void config_storage_destroy(void* impl);
bool config_storage_initialize(void* impl, const std::string&, const std::string&);
bool config_storage_set_master_password(void* impl, const std::string&);
bool config_storage_verify_master_password(void* impl, const std::string&);
bool config_storage_save_cloud_config(void* impl, const CloudStorageConfig&);
bool config_storage_get_cloud_config(void* impl, const std::string&, CloudStorageConfig*);
bool config_storage_delete_cloud_config(void* impl, const std::string&);
std::vector<std::string> config_storage_list_cloud_configs(void* impl);
std::vector<CloudStorageConfig> config_storage_search_configs(void* impl, const std::string&);
bool config_storage_update_cloud_config(void* impl, const std::string&, const CloudStorageConfig&);
bool config_storage_export_configs(void* impl, const std::string&, const std::string&);
bool config_storage_import_configs(void* impl, const std::string&, const std::string&);

} // namespace falcon

namespace {

namespace abi = falcon::abi;
using falcon::CloudStorageConfig;

/** list_cloud_configs 门面不提供 free 回调，用固定 key 复用同一 arena。 */
const void* list_arena_key() {
    static const char key = 0;
    return &key;
}

/** extra：kv 数组块与每项 key/value 都走 arena，free_configs 时统一回收。 */
falcon_kv* fill_extra(abi::StringArena* arena,
                      const std::map<std::string, std::string>& extra,
                      uint32_t& count) {
    return abi::fill_kv_array(arena, extra, count);
}

/** C++ 配置 -> POD；所有字符串与 extra 由 arena 分配。 */
void fill_config(const CloudStorageConfig& cfg, abi::StringArena* arena,
                 falcon_cloud_config* out) {
    out->name = arena->dup(cfg.name);
    out->provider = arena->dup(cfg.provider);
    out->access_key = arena->dup(cfg.access_key);
    out->secret_key = arena->dup(cfg.secret_key);
    out->region = arena->dup(cfg.region);
    out->bucket = arena->dup(cfg.bucket);
    out->endpoint = arena->dup(cfg.endpoint);
    out->custom_domain = arena->dup(cfg.custom_domain);
    out->extra.items = fill_extra(arena, cfg.extra, out->extra.count);
    out->created_at = static_cast<int64_t>(cfg.created_at);
    out->updated_at = static_cast<int64_t>(cfg.updated_at);
}

/** POD -> C++ 配置（仅读取，不分配对外内存）。 */
CloudStorageConfig config_from_abi(const falcon_cloud_config& raw) {
    CloudStorageConfig cfg;
    cfg.name = abi::copy(raw.name);
    cfg.provider = abi::copy(raw.provider);
    cfg.access_key = abi::copy(raw.access_key);
    cfg.secret_key = abi::copy(raw.secret_key);
    cfg.region = abi::copy(raw.region);
    cfg.bucket = abi::copy(raw.bucket);
    cfg.endpoint = abi::copy(raw.endpoint);
    cfg.custom_domain = abi::copy(raw.custom_domain);
    cfg.extra = abi::from_kv(raw.extra.items, raw.extra.count);
    cfg.created_at = static_cast<time_t>(raw.created_at);
    cfg.updated_at = static_cast<time_t>(raw.updated_at);
    return cfg;
}

/**
 * 故障注入桥接：当前 config_manager.cpp 未单独导出该符号，因此在此完成等价
 * 转发。注入掩码是 per-DSO 的 inline static，必须在插件 so 内置位才能命中
 * 内部 EVP 防御分支（exe 侧副本对 so 不可见）。
 */
void config_storage_set_injection(uint32_t point, bool enabled) {
    falcon::detail::set_injection(static_cast<falcon::detail::InjectPoint>(point), enabled);
}

// ==========================================================================
// falcon_config_service 的全部回调：签名均为 C ABI POD，异常不外泄
// ==========================================================================

void* FALCON_CALL service_create(void*) {
    return abi::invoke_or([] { return falcon::config_storage_create(); },
                          static_cast<void*>(nullptr));
}

void FALCON_CALL service_destroy(void*, void* handle) {
    if (!handle) return;
    abi::invoke_void([&] { falcon::config_storage_destroy(handle); });
}

bool FALCON_CALL service_initialize(void* handle, falcon_str db_path,
                                    falcon_str master_password) {
    if (!handle) return false;
    return abi::invoke_or(
        [&] {
            return falcon::config_storage_initialize(handle, abi::copy(db_path),
                                                     abi::copy(master_password));
        },
        false);
}

bool FALCON_CALL service_set_master_password(void* handle, falcon_str password) {
    if (!handle) return false;
    return abi::invoke_or(
        [&] { return falcon::config_storage_set_master_password(handle, abi::copy(password)); },
        false);
}

bool FALCON_CALL service_verify_master_password(void* handle, falcon_str password) {
    if (!handle) return false;
    return abi::invoke_or(
        [&] { return falcon::config_storage_verify_master_password(handle, abi::copy(password)); },
        false);
}

bool FALCON_CALL service_save_cloud_config(void* handle, const falcon_cloud_config* config) {
    if (!handle || !config) return false;
    return abi::invoke_or(
        [&] { return falcon::config_storage_save_cloud_config(handle, config_from_abi(*config)); },
        false);
}

bool FALCON_CALL service_get_cloud_config(void* handle, falcon_str name,
                                          falcon_cloud_config* out) {
    if (!handle || !out) return false;
    *out = falcon_cloud_config{};
    try {
        CloudStorageConfig cfg;
        if (!falcon::config_storage_get_cloud_config(handle, abi::copy(name), &cfg)) {
            return false;
        }
        abi::StringArena* arena = abi::ArenaRegistry::acquire(out);
        fill_config(cfg, arena, out);
        return true;
    } catch (...) {
        abi::ArenaRegistry::release(out);
        *out = falcon_cloud_config{};
        return false;
    }
}

bool FALCON_CALL service_delete_cloud_config(void* handle, falcon_str name) {
    if (!handle) return false;
    return abi::invoke_or(
        [&] { return falcon::config_storage_delete_cloud_config(handle, abi::copy(name)); },
        false);
}

uint32_t FALCON_CALL service_list_cloud_configs(void* handle, falcon_str* out,
                                                uint32_t capacity) {
    if (!handle) return 0;
    return abi::invoke_or(
        [&] {
            const auto names = falcon::config_storage_list_cloud_configs(handle);
            const uint32_t total = static_cast<uint32_t>(names.size());
            if (!out || capacity == 0) return total; // 两阶段：先取数量

            // 门面只读复制、不调用 free_configs；替换同 key 的旧 arena 即回收上一批。
            abi::StringArena* arena = abi::ArenaRegistry::acquire(list_arena_key());
            const uint32_t written = std::min(capacity, total);
            for (uint32_t i = 0; i < written; ++i) {
                out[i] = arena->dup(names[i]);
            }
            return written;
        },
        0u);
}

void FALCON_CALL service_free_configs(void*, falcon_cloud_configs* configs) {
    if (!configs) return;
    // search_configs 批次以 configs 自身为 key；get_cloud_config 单项以
    // configs->items（即门面构造的 &raw）为 key。二者互不冲突。
    abi::ArenaRegistry::release(configs);
    if (configs->items) abi::ArenaRegistry::release(configs->items);
    *configs = falcon_cloud_configs{};
}

uint32_t FALCON_CALL service_search_configs(void* handle, falcon_str provider,
                                            falcon_cloud_configs* out) {
    if (!handle || !out) return 0;
    *out = falcon_cloud_configs{};
    try {
        const auto configs = falcon::config_storage_search_configs(handle, abi::copy(provider));
        if (configs.empty()) return 0;

        abi::StringArena* arena = abi::ArenaRegistry::acquire(out);
        auto* items = arena->allocate<falcon_cloud_config>(
            static_cast<uint32_t>(configs.size()));
        if (!items) {
            abi::ArenaRegistry::release(out);
            return 0;
        }
        for (size_t i = 0; i < configs.size(); ++i) {
            items[i] = falcon_cloud_config{};
            fill_config(configs[i], arena, &items[i]);
        }
        out->items = items;
        out->count = static_cast<uint32_t>(configs.size());
        return out->count;
    } catch (...) {
        abi::ArenaRegistry::release(out);
        *out = falcon_cloud_configs{};
        return 0;
    }
}

bool FALCON_CALL service_update_cloud_config(void* handle, falcon_str name,
                                             const falcon_cloud_config* config) {
    if (!handle || !config) return false;
    return abi::invoke_or(
        [&] {
            return falcon::config_storage_update_cloud_config(
                handle, abi::copy(name), config_from_abi(*config));
        },
        false);
}

bool FALCON_CALL service_export_configs(void* handle, falcon_str path, falcon_str password) {
    if (!handle) return false;
    return abi::invoke_or(
        [&] {
            return falcon::config_storage_export_configs(handle, abi::copy(path),
                                                         abi::copy(password));
        },
        false);
}

bool FALCON_CALL service_import_configs(void* handle, falcon_str path, falcon_str password) {
    if (!handle) return false;
    return abi::invoke_or(
        [&] {
            return falcon::config_storage_import_configs(handle, abi::copy(path),
                                                         abi::copy(password));
        },
        false);
}

void FALCON_CALL service_set_injection(void*, uint32_t point, bool enabled) {
    abi::invoke_void([&] { config_storage_set_injection(point, enabled); });
}

// ==========================================================================
// 静态 manifest / descriptor / service（生命周期与 so 相同）
// ==========================================================================

const falcon_config_service kConfigService = {
    FALCON_PLUGIN_ABI_VERSION,
    nullptr, // service_data
    service_create,
    service_destroy,
    service_initialize,
    service_set_master_password,
    service_verify_master_password,
    service_save_cloud_config,
    service_get_cloud_config,
    service_delete_cloud_config,
    service_list_cloud_configs,
    service_free_configs,
    service_search_configs,
    service_update_cloud_config,
    service_export_configs,
    service_import_configs,
    service_set_injection
};

const falcon_plugin_descriptor kConfigDescriptor = {
    FALCON_PLUGIN_ABI_VERSION,
    static_cast<uint32_t>(FALCON_SERVICE_CONFIG_STORAGE),
    "builtin_config",
    "0.1.0",
    &kConfigService
};

const falcon_plugin_descriptor* const kDescriptors[] = {&kConfigDescriptor};

const falcon_plugin_manifest kManifest = {
    FALCON_PLUGIN_ABI_VERSION,
    "config_storage",
    "0.1.0",
    1u,
    kDescriptors
};

} // namespace

FALCON_PLUGIN_DEFINE_ENTRY(kManifest)
