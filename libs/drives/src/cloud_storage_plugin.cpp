/**
 * @file cloud_storage_plugin.cpp
 * @brief 云盘门面：保留公开 C++ API，插件实例由稳定 C ABI 服务提供
 *
 * 门面不触碰 curl/json/SQLite；C ABI 插件实例经 CppPluginAdapter
 * 适配为 ICloudStoragePlugin，上层调度逻辑保持不变。
 */
#include <falcon/drives/cloud_storage_plugin.hpp>
#include <falcon/drives/plugin_loader.hpp>
#include <falcon/abi/marshal.hpp>
#include <falcon/logger.hpp>

#include <algorithm>
#include <utility>

namespace falcon {

namespace {

CloudPlatform to_cpp_platform(uint32_t value) {
    switch (static_cast<falcon_cloud_platform>(value)) {
        case FALCON_CLOUD_BAIDU:    return CloudPlatform::BaiduNetdisk;
        case FALCON_CLOUD_LANZOU:   return CloudPlatform::LanzouCloud;
        case FALCON_CLOUD_ALIBABA:  return CloudPlatform::AlibabaCloud;
        case FALCON_CLOUD_TENCENT:  return CloudPlatform::TencentWeiyun;
        case FALCON_CLOUD_115:      return CloudPlatform::Cloud115;
        case FALCON_CLOUD_QUARK:    return CloudPlatform::Quark;
        case FALCON_CLOUD_TIANYI:   return CloudPlatform::TianyiCloud;
        case FALCON_CLOUD_PIKPAK:   return CloudPlatform::PikPak;
        case FALCON_CLOUD_MEGA:     return CloudPlatform::Mega;
        case FALCON_CLOUD_GOOGLE:   return CloudPlatform::GoogleDrive;
        case FALCON_CLOUD_ONEDRIVE: return CloudPlatform::OneDrive;
        case FALCON_CLOUD_DROPBOX:  return CloudPlatform::Dropbox;
        case FALCON_CLOUD_YANDEX:   return CloudPlatform::YandexDisk;
        default:                    return CloudPlatform::Unknown;
    }
}

/** 把 C ABI 网盘插件实例适配成既有 C++ 接口。 */
class CppPluginAdapter final : public ICloudStoragePlugin {
public:
    CppPluginAdapter(falcon_drive_plugin* plugin,
                     const falcon_cloud_service* service)
        : plugin_(plugin), service_(service) {}

    ~CppPluginAdapter() override {
        if (plugin_ && plugin_->destroy) plugin_->destroy(plugin_);
    }

    std::string platform_name() const override {
        if (!plugin_ || !plugin_->platform_name) return {};
        return abi::copy(plugin_->platform_name(plugin_));
    }

    CloudPlatform platform_type() const override {
        if (!plugin_ || !plugin_->platform_type) return CloudPlatform::Unknown;
        return to_cpp_platform(plugin_->platform_type(plugin_));
    }

    bool can_handle(const std::string& url) const override {
        if (!plugin_ || !plugin_->can_handle) return false;
        return plugin_->can_handle(plugin_, abi::view(url));
    }

    CloudExtractionResult extract_share_link(
        const std::string& share_url, const std::string& password) override {
        CloudExtractionResult result;
        if (!plugin_ || !plugin_->extract_share_link) return result;

        falcon_extraction_result raw{};
        plugin_->extract_share_link(plugin_, abi::view(share_url),
                                    abi::view(password), &raw);

        result.success = raw.success;
        result.recognized = raw.recognized;
        result.error_message = abi::copy(raw.error_message);
        result.platform_name = abi::copy(raw.platform_name);
        result.platform_type = to_cpp_platform(raw.platform_type);

        if (raw.files) {
            result.files.reserve(raw.file_count);
            for (uint32_t i = 0; i < raw.file_count; ++i) {
                const auto& src = raw.files[i];
                CloudFileInfo file;
                file.id = abi::copy(src.id);
                file.name = abi::copy(src.name);
                file.size = static_cast<size_t>(src.size);
                file.type = abi::copy(src.type);
                file.md5 = abi::copy(src.md5);
                file.modified_time = abi::copy(src.modified_time);
                file.download_url = abi::copy(src.download_url);
                file.share_url = abi::copy(src.share_url);
                file.password = abi::copy(src.password);
                result.files.push_back(std::move(file));
            }
        }
        if (plugin_->free_result) plugin_->free_result(plugin_, &raw);
        return result;
    }

    std::string get_download_url(
        const std::string& file_id, const CloudDownloadOptions& options) override {
        if (!plugin_ || !plugin_->get_download_url) return {};
        falcon_download_options raw{};
        raw.auth_token = abi::view(options.auth_token);
        raw.refresh_token = abi::view(options.refresh_token);
        raw.api_key = abi::view(options.api_key);
        raw.api_secret = abi::view(options.api_secret);
        raw.use_vip = options.use_vip;
        raw.download_thread = abi::view(options.download_thread);
        raw.timeout_seconds = options.timeout_seconds;
        raw.retry_count = options.retry_count;
        return abi::copy(plugin_->get_download_url(plugin_, abi::view(file_id), &raw));
    }

    bool authenticate(const std::string& token) override {
        if (!plugin_ || !plugin_->authenticate) return false;
        return plugin_->authenticate(plugin_, abi::view(token));
    }

    std::map<std::string, std::string> get_user_info() override {
        if (!plugin_ || !plugin_->get_user_info) return {};
        const uint32_t count = plugin_->get_user_info(plugin_, nullptr, 0);
        if (!count) return {};
        std::vector<falcon_kv> buffer(count);
        const uint32_t written = plugin_->get_user_info(plugin_, buffer.data(), count);
        return abi::from_kv(buffer.data(), written);
    }

    std::map<std::string, size_t> get_quota_info() override {
        std::map<std::string, size_t> out;
        if (!plugin_ || !plugin_->get_quota_info) return out;
        const uint32_t count = plugin_->get_quota_info(plugin_, nullptr, 0);
        if (!count) return out;
        std::vector<falcon_kv> buffer(count);
        const uint32_t written = plugin_->get_quota_info(plugin_, buffer.data(), count);
        for (uint32_t i = 0; i < written; ++i) {
            try {
                out[abi::copy(buffer[i].key)] =
                    static_cast<size_t>(std::stoull(abi::copy(buffer[i].value)));
            } catch (...) {
                // 非数值配额条目忽略
            }
        }
        return out;
    }

private:
    falcon_drive_plugin* plugin_ = nullptr;
    const falcon_cloud_service* service_ = nullptr;
};

} // namespace

// ============================================================================
// CloudStorageManager
// ============================================================================

class CloudStorageManager::Impl {
public:
    Impl() {
        const auto* service = drives::PluginLoader::instance().cloud_storage();
        if (!service || !service->create_plugin) {
            log_warn("云盘插件未加载，CloudStorageManager 仅接受自定义注册的插件");
            return;
        }
        const uint32_t count = service->plugin_count(service->service_data);
        for (uint32_t i = 0; i < count; ++i) {
            auto* plugin = service->create_plugin(service->service_data, i,
                                                  drives::PluginLoader::host_api());
            if (plugin) {
                plugins_.push_back(std::make_unique<CppPluginAdapter>(plugin, service));
            }
        }
    }

    std::vector<std::unique_ptr<ICloudStoragePlugin>> plugins_;

    void register_plugin(std::unique_ptr<ICloudStoragePlugin> plugin) {
        if (plugin) plugins_.push_back(std::move(plugin));
    }
};

CloudStorageManager::CloudStorageManager()
    : p_impl(std::make_unique<Impl>()) {}

CloudStorageManager::~CloudStorageManager() = default;

void CloudStorageManager::register_plugin(std::unique_ptr<ICloudStoragePlugin> plugin) {
    p_impl->register_plugin(std::move(plugin));
}

CloudExtractionResult CloudStorageManager::handle_share_link(
    const std::string& url,
    const std::string& password) {
    return handle_share_link(url, password, true);
}

CloudExtractionResult CloudStorageManager::handle_share_link(
    const std::string& url,
    const std::string& password,
    bool log_selected_plugin) {

    CloudExtractionResult result;

    auto extract_with_plugin = [&](ICloudStoragePlugin& plugin) {
        if (log_selected_plugin) {
            log_info("使用 " + plugin.platform_name() + " 处理网盘链接");
        }
        return plugin.extract_share_link(url, password);
    };

    const CloudPlatform detected_platform = CloudLinkDetector::detect_platform(url);
    if (detected_platform != CloudPlatform::Unknown) {
        for (auto& plugin : p_impl->plugins_) {
            if (plugin->platform_type() == detected_platform && plugin->can_handle(url)) {
                return extract_with_plugin(*plugin);
            }
        }
    }

    for (auto& plugin : p_impl->plugins_) {
        if (detected_platform == CloudPlatform::Unknown &&
            plugin->platform_type() != CloudPlatform::Unknown) {
            continue;
        }
        if (detected_platform != CloudPlatform::Unknown &&
            plugin->platform_type() == detected_platform) {
            continue;
        }
        if (plugin->can_handle(url)) {
            return extract_with_plugin(*plugin);
        }
    }

    if (detected_platform == CloudPlatform::Unknown) {
        for (auto& plugin : p_impl->plugins_) {
            if (plugin->platform_type() == CloudPlatform::Unknown) continue;
            if (plugin->can_handle(url)) return extract_with_plugin(*plugin);
        }
    }

    result.error_message = "未找到对应的网盘插件";
    return result;
}

std::string CloudStorageManager::get_direct_download_url(
    const std::string& url,
    const CloudDownloadOptions& /*options*/) {

    auto result = handle_share_link(url);
    if (result.success && !result.files.empty()) {
        return result.files[0].download_url;
    }
    if (result.recognized) {
        log_info("网盘 " + result.platform_name + " 需要进一步处理：" + result.error_message);
    }
    return "";
}

std::vector<std::string> CloudStorageManager::get_supported_platforms() {
    std::vector<std::string> platforms;
    for (const auto& plugin : p_impl->plugins_) platforms.push_back(plugin->platform_name());
    return platforms;
}

std::vector<CloudExtractionResult> CloudStorageManager::batch_extract(
    const std::vector<std::string>& urls,
    const std::map<std::string, std::string>& passwords) {

    std::vector<CloudExtractionResult> results;
    for (const auto& url : urls) {
        std::string password;
        auto it = passwords.find(url);
        if (it != passwords.end()) password = it->second;
        results.push_back(handle_share_link(url, password, false));
    }
    return results;
}

std::vector<ICloudStoragePlugin*> CloudStorageManager::plugins() const {
    std::vector<ICloudStoragePlugin*> view;
    view.reserve(p_impl->plugins_.size());
    for (auto& plugin : p_impl->plugins_) view.push_back(plugin.get());
    return view;
}

} // namespace falcon
