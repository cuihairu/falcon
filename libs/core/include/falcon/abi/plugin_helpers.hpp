/**
 * @file plugin_helpers.hpp
 * @brief 插件作者侧的工具箱（header-only，非 ABI 契约）
 *
 * 只封装重复的机械工作：宿主服务表绑定、按输出地址隔离的 arena 注册表、
 * POD 数组分配、kv 数组填充、异常隔离、入口符号导出。
 *
 * 刻意**不**用宏隐藏 ABI 类型、vtable 字段或内存所有权语义；
 * 唯一的宏只用于生成两个固定的导出入口，避免每个 so 手抄一遍。
 */
#pragma once

#include <falcon/abi/drive_api.h>
#include <falcon/abi/marshal.hpp>

#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

#if defined(_WIN32)
/** Windows 侧由 CMake WINDOWS_EXPORT_ALL_SYMBOLS 或插件自己的 .def 导出。 */
#  define FALCON_PLUGIN_EXPORT
#else
#  define FALCON_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

namespace falcon {
namespace abi {

/**
 * falcon_plugin_set_host 注入的宿主服务表。
 * 生命周期归宿主；插件只保存指针，不复制、不释放。
 * 未注入时提供进程内的 malloc/free 退化实现，保证分配/释放始终在同侧。
 */
class HostApi {
public:
    static void set(const falcon_host_api* host) { slot() = host; }
    static const falcon_host_api* get() { return slot(); }

    static const falcon_host_api* effective() {
        const falcon_host_api* host = slot();
        if (host && host->allocate && host->deallocate) return host;
        static const falcon_host_api fallback = [] {
            falcon_host_api api{};
            api.abi_version = FALCON_PLUGIN_ABI_VERSION;
            api.allocate = [](void*, size_t bytes) -> void* {
                return std::malloc(bytes ? bytes : 1);
            };
            api.deallocate = [](void*, void* ptr) { std::free(ptr); };
            return api;
        }();
        return &fallback;
    }

private:
    static const falcon_host_api*& slot() {
        static const falcon_host_api* host = nullptr;
        return host;
    }
};

/**
 * 以输出结构体地址（或单项地址）为 key 管理 StringArena。
 * 解决门面并行调用时共用 arena 互相 clear 的问题：每批结果独立生命周期，
 * 宿主调用对应 free_* 回调时按同一 key 释放。
 */
class ArenaRegistry {
public:
    static StringArena* acquire(const void* key) {
        std::lock_guard<std::mutex> lock(mutex());
        auto arena = std::make_unique<StringArena>(HostApi::effective());
        auto* ptr = arena.get();
        // 同 key 旧条目析构时自行 clear()，不会泄漏上一批内存。
        arenas()[key] = std::move(arena);
        return ptr;
    }

    static void release(const void* key) {
        std::lock_guard<std::mutex> lock(mutex());
        auto& map = arenas();
        auto it = map.find(key);
        if (it == map.end()) return;
        it->second->clear();
        map.erase(it);
    }

private:
    static std::mutex& mutex() {
        static std::mutex value;
        return value;
    }

    static std::unordered_map<const void*, std::unique_ptr<StringArena>>& arenas() {
        static std::unordered_map<const void*, std::unique_ptr<StringArena>> value;
        return value;
    }
};

/** map<string,string> -> falcon_kv[]，数组块与字符串都走同一 arena。 */
inline falcon_kv* fill_kv_array(StringArena* arena,
                                const std::map<std::string, std::string>& values,
                                uint32_t& count) {
    count = 0;
    if (!arena || values.empty()) return nullptr;
    falcon_kv* items = arena->allocate<falcon_kv>(
        static_cast<uint32_t>(values.size()));
    if (!items) return nullptr;
    uint32_t i = 0;
    for (const auto& entry : values) {
        items[i].key = arena->dup(entry.first);
        items[i].value = arena->dup(entry.second);
        ++i;
    }
    count = static_cast<uint32_t>(values.size());
    return items;
}

/** C ABI 回调不允许抛异常越过 so 边界。 */
template <typename Fn, typename R>
R invoke_or(Fn&& fn, R fallback) noexcept {
    try {
        return fn();
    } catch (...) {
        return fallback;
    }
}

template <typename Fn>
void invoke_void(Fn&& fn) noexcept {
    try {
        fn();
    } catch (...) {
    }
}

} // namespace abi
} // namespace falcon

/**
 * 生成唯一的两个插件导出入口。MANIFEST 是本 TU 内的静态 manifest 对象；
 * SET_HOST_BODY 可选，用于在宿主表变化时同步插件内部实现。
 */
#define FALCON_PLUGIN_DEFINE_ENTRY_IMPL(MANIFEST, SET_HOST_BODY)                \
    extern "C" FALCON_PLUGIN_EXPORT const falcon_plugin_manifest* FALCON_CALL   \
    falcon_plugin_entry(uint32_t abi_version) {                                 \
        return FALCON_PLUGIN_ABI_COMPATIBLE(abi_version) ? &(MANIFEST)          \
                                                        : nullptr;              \
    }                                                                           \
    extern "C" FALCON_PLUGIN_EXPORT void FALCON_CALL                            \
    falcon_plugin_set_host(const falcon_host_api* host) {                        \
        ::falcon::abi::HostApi::set(host);                                       \
        SET_HOST_BODY                                                            \
    }

#define FALCON_PLUGIN_DEFINE_ENTRY(MANIFEST) \
    FALCON_PLUGIN_DEFINE_ENTRY_IMPL(MANIFEST, (void)host;)

#define FALCON_PLUGIN_DEFINE_ENTRY_WITH_HOST(MANIFEST, SET_HOST_BODY) \
    FALCON_PLUGIN_DEFINE_ENTRY_IMPL(MANIFEST, SET_HOST_BODY)
