/**
 * @file spi_entry.cpp
 * @brief cloud_storage 插件稳定 C ABI 入口
 *
 * 唯一对外边界是 falcon/abi/drive_api.h 定义的 C ABI：
 *  - falcon_plugin_entry 返回静态 manifest（一个 CLOUD_STORAGE 描述符）；
 *  - falcon_plugin_set_host 注入宿主服务表，HTTP 由宿主提供；
 *  - 每个 C++ 插件实例包装成 falcon_drive_plugin，所有跨边界数据均为 POD。
 */
#include <falcon/abi/drive_api.h>
#include <falcon/abi/marshal.hpp>
#include <falcon/abi/plugin_helpers.hpp>
#include <falcon/drives/cloud_storage_plugin.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace falcon {

// cloud_storage_plugin.cpp 内部工厂与宿主服务表注入槽
std::vector<std::unique_ptr<ICloudStoragePlugin>> create_default_cloud_plugins();
void set_cloud_host_api(const falcon_host_api* host);

} // namespace falcon

namespace {

using falcon::abi::copy;

/** 与 create_default_cloud_plugins() 的返回顺序一一对应。 */
constexpr uint32_t kPluginCount = 12;

uint32_t to_abi_platform(falcon::CloudPlatform platform) {
    switch (platform) {
        case falcon::CloudPlatform::BaiduNetdisk: return FALCON_CLOUD_BAIDU;
        case falcon::CloudPlatform::LanzouCloud:  return FALCON_CLOUD_LANZOU;
        case falcon::CloudPlatform::AlibabaCloud: return FALCON_CLOUD_ALIBABA;
        case falcon::CloudPlatform::TencentWeiyun:return FALCON_CLOUD_TENCENT;
        case falcon::CloudPlatform::Cloud115:     return FALCON_CLOUD_115;
        case falcon::CloudPlatform::Quark:        return FALCON_CLOUD_QUARK;
        case falcon::CloudPlatform::TianyiCloud:  return FALCON_CLOUD_TIANYI;
        case falcon::CloudPlatform::PikPak:       return FALCON_CLOUD_PIKPAK;
        case falcon::CloudPlatform::Mega:         return FALCON_CLOUD_MEGA;
        case falcon::CloudPlatform::GoogleDrive:  return FALCON_CLOUD_GOOGLE;
        case falcon::CloudPlatform::OneDrive:     return FALCON_CLOUD_ONEDRIVE;
        case falcon::CloudPlatform::Dropbox:      return FALCON_CLOUD_DROPBOX;
        case falcon::CloudPlatform::YandexDisk:   return FALCON_CLOUD_YANDEX;
        default:                                  return FALCON_CLOUD_UNKNOWN;
    }
}

/** 每个 C ABI 实例持有的 C++ 实现与字符串缓冲。 */
struct PluginInstance {
    PluginInstance(std::unique_ptr<falcon::ICloudStoragePlugin> impl,
                   const falcon_host_api* host)
        : impl(std::move(impl)), arena(host) {}

    std::unique_ptr<falcon::ICloudStoragePlugin> impl;
    falcon::abi::StringArena arena;
};

PluginInstance* instance_of(falcon_drive_plugin* self) {
    return self ? static_cast<PluginInstance*>(self->plugin_data) : nullptr;
}

void FALCON_CALL plugin_destroy(falcon_drive_plugin* self) {
    if (!self) return;
    delete static_cast<PluginInstance*>(self->plugin_data);
    delete self;
}

falcon_str FALCON_CALL plugin_platform_name(falcon_drive_plugin* self) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return falcon_str{};
    return inst->arena.dup(inst->impl->platform_name());
}

uint32_t FALCON_CALL plugin_platform_type(falcon_drive_plugin* self) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return FALCON_CLOUD_UNKNOWN;
    return to_abi_platform(inst->impl->platform_type());
}

bool FALCON_CALL plugin_can_handle(falcon_drive_plugin* self, falcon_str url) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return false;
    return inst->impl->can_handle(copy(url));
}

void FALCON_CALL plugin_extract_share_link(falcon_drive_plugin* self,
                                           falcon_str share_url,
                                           falcon_str password,
                                           falcon_extraction_result* out) {
    if (!out) return;
    *out = falcon_extraction_result{};
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return;

    const auto result = inst->impl->extract_share_link(copy(share_url), copy(password));
    out->success = result.success;
    out->recognized = result.recognized;
    out->error_message = inst->arena.dup(result.error_message);
    out->platform_name = inst->arena.dup(result.platform_name);
    out->platform_type = to_abi_platform(result.platform_type);

    if (result.files.empty()) return;
    // files 数组也走 arena：宿主分配器出的块与字符串统一由 clear() 回收。
    auto* files = inst->arena.allocate<falcon_cloud_file>(
        static_cast<uint32_t>(result.files.size()));
    if (!files) return;
    for (size_t i = 0; i < result.files.size(); ++i) {
        const auto& src = result.files[i];
        auto& dst = files[i];
        dst = falcon_cloud_file{};
        dst.id = inst->arena.dup(src.id);
        dst.name = inst->arena.dup(src.name);
        dst.size = static_cast<uint64_t>(src.size);
        dst.type = inst->arena.dup(src.type);
        dst.md5 = inst->arena.dup(src.md5);
        dst.modified_time = inst->arena.dup(src.modified_time);
        dst.download_url = inst->arena.dup(src.download_url);
        dst.share_url = inst->arena.dup(src.share_url);
        dst.password = inst->arena.dup(src.password);
    }
    out->files = files;
    out->file_count = static_cast<uint32_t>(result.files.size());
}

void FALCON_CALL plugin_free_result(falcon_drive_plugin* self,
                                    falcon_extraction_result* result) {
    auto* inst = instance_of(self);
    if (result) *result = falcon_extraction_result{};
    if (inst) inst->arena.clear();
}

falcon_str FALCON_CALL plugin_get_download_url(
    falcon_drive_plugin* self,
    falcon_str file_id,
    const falcon_download_options* options) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return falcon_str{};

    falcon::CloudDownloadOptions converted;
    if (options) {
        converted.auth_token = copy(options->auth_token);
        converted.refresh_token = copy(options->refresh_token);
        converted.api_key = copy(options->api_key);
        converted.api_secret = copy(options->api_secret);
        converted.use_vip = options->use_vip;
        converted.download_thread = copy(options->download_thread);
        converted.timeout_seconds = options->timeout_seconds;
        converted.retry_count = options->retry_count;
    }
    return inst->arena.dup(inst->impl->get_download_url(copy(file_id), converted));
}

bool FALCON_CALL plugin_authenticate(falcon_drive_plugin* self, falcon_str token) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return false;
    return inst->impl->authenticate(copy(token));
}

uint32_t FALCON_CALL plugin_get_user_info(falcon_drive_plugin* self,
                                          falcon_kv* out, uint32_t capacity) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return 0;
    const auto info = inst->impl->get_user_info();
    const uint32_t total = static_cast<uint32_t>(info.size());
    if (!out || capacity == 0) return total;

    const uint32_t written = std::min(capacity, total);
    uint32_t i = 0;
    for (const auto& entry : info) {
        if (i >= written) break;
        // 返回值须在本次调用返回后仍有效，直到宿主 free_result / destroy。
        out[i].key = inst->arena.dup(entry.first);
        out[i].value = inst->arena.dup(entry.second);
        ++i;
    }
    return written;
}

uint32_t FALCON_CALL plugin_get_quota_info(falcon_drive_plugin* self,
                                           falcon_kv* out, uint32_t capacity) {
    auto* inst = instance_of(self);
    if (!inst || !inst->impl) return 0;
    const auto info = inst->impl->get_quota_info();
    const uint32_t total = static_cast<uint32_t>(info.size());
    if (!out || capacity == 0) return total;

    const uint32_t written = std::min(capacity, total);
    uint32_t i = 0;
    for (const auto& entry : info) {
        if (i >= written) break;
        const std::string value = std::to_string(entry.second);
        out[i].key = inst->arena.dup(entry.first);
        out[i].value = inst->arena.dup(value);
        ++i;
    }
    return written;
}

const void* FALCON_CALL plugin_extension(falcon_drive_plugin*, falcon_str) {
    return nullptr;
}

uint32_t FALCON_CALL service_plugin_count(void*) {
    return kPluginCount;
}

falcon_drive_plugin* FALCON_CALL service_create_plugin(void*, uint32_t index,
                                                       const falcon_host_api* host) {
    auto plugins = falcon::create_default_cloud_plugins();
    if (index >= plugins.size()) return nullptr;

    auto* inst = new PluginInstance(std::move(plugins[index]), host);
    auto* self = new falcon_drive_plugin{};
    self->abi_version = FALCON_PLUGIN_ABI_VERSION;
    self->plugin_data = inst;
    self->destroy = plugin_destroy;
    self->platform_name = plugin_platform_name;
    self->platform_type = plugin_platform_type;
    self->can_handle = plugin_can_handle;
    self->extract_share_link = plugin_extract_share_link;
    self->free_result = plugin_free_result;
    self->get_download_url = plugin_get_download_url;
    self->authenticate = plugin_authenticate;
    self->get_user_info = plugin_get_user_info;
    self->get_quota_info = plugin_get_quota_info;
    self->extension = plugin_extension;
    return self;
}

const falcon_cloud_service kCloudService = {
    FALCON_PLUGIN_ABI_VERSION,
    service_plugin_count,
    service_create_plugin,
    nullptr  // service_data
};

const falcon_plugin_descriptor kCloudDescriptor = {
    FALCON_PLUGIN_ABI_VERSION,
    static_cast<uint32_t>(FALCON_SERVICE_CLOUD_STORAGE),
    "builtin_cloud",
    "0.1.0",
    &kCloudService
};

const falcon_plugin_descriptor* const kDescriptors[] = {&kCloudDescriptor};

const falcon_plugin_manifest kManifest = {
    FALCON_PLUGIN_ABI_VERSION,
    "cloud_storage",
    "0.1.0",
    1u,
    kDescriptors
};

} // namespace

FALCON_PLUGIN_DEFINE_ENTRY_WITH_HOST(kManifest, ::falcon::set_cloud_host_api(host);)
