/**
 * @file plugin_loader.cpp
 * @brief Drives 插件动态加载器（进程级单例）
 *
 * 唯一插件边界是 falcon/abi/drive_api.h 定义的稳定 C ABI。
 * 只依赖 STL + 平台动态库 API，无第三方库。
 *
 * 插件发现顺序（首个存在的目录生效，可叠加多个）：
 *  1. 环境变量 FALCON_PLUGIN_DIR（分号/冒号分隔，显式覆盖）
 *  2. 可执行文件同级的 falcon/plugins 目录（绿色便携分发）
 *  3. 编译期 FALCON_PLUGIN_DEFAULT_DIR（系统安装分发）
 *
 * 加载失败不抛异常，只记录到 errors()，门面层据此给出明确诊断。
 */
#include <falcon/drives/plugin_loader.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#if defined(__APPLE__)
#  include <mach-o/dyld.h>
#endif

namespace falcon {
namespace drives {

namespace fs = std::filesystem;

namespace {

const falcon_host_api*& host_api_slot() {
    static const falcon_host_api* api = nullptr;
    return api;
}

#if defined(_WIN32)
const char kLibrarySuffix[] = ".dll";
using LibraryHandle = HMODULE;
#else
#  if defined(__APPLE__)
const char kLibrarySuffix[] = ".dylib";
#  else
const char kLibrarySuffix[] = ".so";
#  endif
using LibraryHandle = void*;
#endif

LibraryHandle open_library(const fs::path& path) {
#if defined(_WIN32)
    return LoadLibraryW(path.wstring().c_str());
#else
    return dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* lookup_symbol(LibraryHandle handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
    return dlsym(handle, name);
#endif
}

std::string last_error() {
#if defined(_WIN32)
    return "Windows error " + std::to_string(GetLastError());
#else
    const char* err = dlerror();
    return err ? err : "unknown dynamic loader error";
#endif
}

void close_library(LibraryHandle handle) {
    if (!handle) return;
#if defined(_WIN32)
    FreeLibrary(handle);
#else
    dlclose(handle);
#endif
}

fs::path executable_dir() {
#if defined(_WIN32)
    std::vector<wchar_t> buffer(4096, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buffer.data(),
                                       static_cast<DWORD>(buffer.size()));
    if (n == 0 || n == buffer.size()) return {};
    return fs::path(std::wstring(buffer.data(), n)).parent_path();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (NSGetExecutablePath(buffer.data(), &size) != 0) return {};
    return fs::path(std::string(buffer.data(), size)).parent_path();
#else
    std::error_code ec;
    fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
    return self.parent_path();
#endif
}

std::vector<std::string> split_search_paths(const std::string& value) {
    std::vector<std::string> out;
    const char sep =
#if defined(_WIN32)
        ';';
#else
        ':';
#endif
    size_t begin = 0;
    while (begin <= value.size()) {
        const size_t end = value.find(sep, begin);
        const std::string part = value.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (!part.empty()) out.push_back(part);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return out;
}

} // namespace

// ============================================================================
// Impl
// ============================================================================

class PluginLoader::Impl {
public:
    Impl() { discover_and_load(); }

    ~Impl() {
        // 先清空服务指针，再卸载 so；避免悬垂指针被误用。
        cloud_ = nullptr;
        search_ = nullptr;
        config_ = nullptr;
        for (auto handle : libraries_) close_library(handle);
        libraries_.clear();
    }

    void discover_and_load() {
        for (auto handle : libraries_) close_library(handle);
        libraries_.clear();
        errors_.clear();
        loaded_.clear();
        descriptors_.clear();
        cloud_ = nullptr;
        search_ = nullptr;
        config_ = nullptr;
        paths_ = build_search_paths();

        for (const auto& dir : paths_) {
            std::error_code ec;
            if (!fs::is_directory(dir, ec)) continue;

            std::vector<fs::path> candidates;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::path& p = it->path();
                if (p.extension() == kLibrarySuffix) candidates.push_back(p);
            }
            std::sort(candidates.begin(), candidates.end());
            for (const auto& candidate : candidates) load_one(candidate);
        }

        if (libraries_.empty() && errors_.empty()) {
            errors_.push_back("未发现任何 Falcon 插件；请检查 FALCON_PLUGIN_DIR 或安装目录");
        }
    }

    std::vector<std::string> build_search_paths() const {
        std::vector<std::string> out;
        if (const char* env = std::getenv("FALCON_PLUGIN_DIR")) {
            for (auto& p : split_search_paths(env)) out.push_back(p);
        }
        const fs::path exe_dir = executable_dir();
        if (!exe_dir.empty()) {
            out.push_back((exe_dir / "falcon" / "plugins").string());
            out.push_back((exe_dir / "plugins").string());
        }
#ifdef FALCON_PLUGIN_DEFAULT_DIR
        out.push_back(FALCON_PLUGIN_DEFAULT_DIR);
#endif
        return out;
    }

    void load_one(const fs::path& path) {
        LibraryHandle handle = open_library(path);
        if (!handle) {
            errors_.push_back(path.string() + ": 加载失败（" + last_error() + "）");
            return;
        }

        auto entry = reinterpret_cast<falcon_plugin_entry_fn>(
            lookup_symbol(handle, FALCON_PLUGIN_ENTRY_SYMBOL));
        if (!entry) {
            // 与本 ABI 无关的第三方 so：静默跳过。
            close_library(handle);
            return;
        }

        const falcon_plugin_manifest* manifest = nullptr;
        try {
            manifest = entry(FALCON_PLUGIN_ABI_VERSION);
        } catch (...) {
            manifest = nullptr;
        }
        if (!manifest || !FALCON_PLUGIN_ABI_COMPATIBLE(manifest->abi_version)) {
            errors_.push_back(path.string() + ": ABI 主版本不兼容");
            close_library(handle);
            return;
        }
        if (!manifest->descriptors || !manifest->name || manifest->descriptor_count == 0) {
            errors_.push_back(path.string() + ": manifest 不完整");
            close_library(handle);
            return;
        }

        // 注入宿主服务表（含内核 HTTP）。插件无此符号时自行降级。
        if (auto set_host = reinterpret_cast<falcon_plugin_set_host_fn>(
                lookup_symbol(handle, FALCON_PLUGIN_SET_HOST_SYMBOL))) {
            set_host(host_api_slot());
        }

        bool accepted = false;
        for (uint32_t i = 0; i < manifest->descriptor_count; ++i) {
            const falcon_plugin_descriptor* desc = manifest->descriptors[i];
            if (!desc || !FALCON_PLUGIN_ABI_COMPATIBLE(desc->abi_version) ||
                    !desc->service || !desc->name) {
                continue;
            }
            switch (static_cast<falcon_service_kind>(desc->kind)) {
                case FALCON_SERVICE_CLOUD_STORAGE: {
                    auto* service = static_cast<const falcon_cloud_service*>(desc->service);
                    if (service->plugin_count && service->create_plugin) {
                        cloud_ = service;
                        accepted = true;
                    }
                    break;
                }
                case FALCON_SERVICE_RESOURCE_SEARCH: {
                    auto* service = static_cast<const falcon_search_service*>(desc->service);
                    if (service->load_config && service->search_one) {
                        search_ = service;
                        accepted = true;
                    }
                    break;
                }
                case FALCON_SERVICE_CONFIG_STORAGE: {
                    auto* service = static_cast<const falcon_config_service*>(desc->service);
                    if (service->create && service->destroy && service->initialize) {
                        config_ = service;
                        accepted = true;
                    }
                    break;
                }
                default:
                    break;
            }
            if (accepted) {
                descriptors_.push_back(desc);
                loaded_.push_back(std::string(desc->name) + "@" +
                                  (desc->version ? desc->version : "0"));
            }
        }

        if (!accepted) {
            errors_.push_back(path.string() + ": 无可用服务描述符");
            close_library(handle);
            return;
        }
        libraries_.push_back(handle);
    }

    std::vector<LibraryHandle> libraries_;
    std::vector<std::string> loaded_;
    std::vector<std::string> errors_;
    std::vector<std::string> paths_;
    std::vector<const falcon_plugin_descriptor*> descriptors_;

    const falcon_cloud_service* cloud_ = nullptr;
    const falcon_search_service* search_ = nullptr;
    const falcon_config_service* config_ = nullptr;
};

// ============================================================================
// PluginLoader facade
// ============================================================================

PluginLoader::PluginLoader() : impl_(std::make_unique<Impl>()) {}
PluginLoader::~PluginLoader() = default;

PluginLoader& PluginLoader::instance() {
    static PluginLoader loader;
    return loader;
}

const falcon_cloud_service* PluginLoader::cloud_storage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->cloud_;
}

const falcon_search_service* PluginLoader::resource_search() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->search_;
}

const falcon_config_service* PluginLoader::config_storage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->config_;
}

std::vector<const falcon_plugin_descriptor*> PluginLoader::descriptors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->descriptors_;
}

std::vector<std::string> PluginLoader::loaded_plugins() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->loaded_;
}

std::vector<std::string> PluginLoader::errors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->errors_;
}

std::vector<std::string> PluginLoader::search_paths() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->paths_;
}

void PluginLoader::reload() {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->discover_and_load();
}

void PluginLoader::set_host_api(const falcon_host_api* host) {
    host_api_slot() = host;
}

const falcon_host_api* PluginLoader::host_api() {
    return host_api_slot();
}

} // namespace drives
} // namespace falcon
