/**
 * @file marshal.hpp
 * @brief C++ 侧与 falcon ABI POD 之间的编组辅助（内部工具，非 ABI 契约）
 *
 * 门面与插件都可以使用；不属于公开 ABI，可随版本演进。
 */
#pragma once

#include <falcon/abi/drive_api.h>

#include <cstring>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace falcon {
namespace abi {

inline falcon_str view(const std::string& value) {
    falcon_str out{};
    out.data = value.data();
    out.size = static_cast<uint32_t>(value.size());
    return out;
}

inline falcon_str view(const char* value) {
    falcon_str out{};
    out.data = value;
    out.size = value ? static_cast<uint32_t>(std::strlen(value)) : 0u;
    return out;
}

inline std::string copy(falcon_str value) {
    return (value.data && value.size)
               ? std::string(value.data, value.size)
               : std::string();
}

inline std::vector<falcon_kv> to_kv(const std::map<std::string, std::string>& values) {
    std::vector<falcon_kv> out;
    out.reserve(values.size());
    for (const auto& entry : values) {
        falcon_kv kv{};
        kv.key = view(entry.first);
        kv.value = view(entry.second);
        out.push_back(kv);
    }
    return out;
}

inline std::map<std::string, std::string> from_kv(const falcon_kv* items, uint32_t count) {
    std::map<std::string, std::string> out;
    if (!items) return out;
    for (uint32_t i = 0; i < count; ++i) {
        out[copy(items[i].key)] = copy(items[i].value);
    }
    return out;
}

/**
 * 插件侧字符串缓冲。所有对外字符串用宿主分配器分配，
 * 由插件的 free_* 回调统一回收，避免跨 CRT 堆错配。
 */
class StringArena {
public:
    explicit StringArena(const falcon_host_api* host) : host_(host) {}
    ~StringArena() { clear(); }

    StringArena(const StringArena&) = delete;
    StringArena& operator=(const StringArena&) = delete;

    falcon_str dup(const std::string& value) {
        falcon_str out{};
        if (value.empty() || !host_ || !host_->allocate) return out;
        void* ptr = host_->allocate(host_->host_data, value.size());
        if (!ptr) return out;
        std::memcpy(ptr, value.data(), value.size());
        blocks_.push_back(ptr);
        out.data = static_cast<const char*>(ptr);
        out.size = static_cast<uint32_t>(value.size());
        return out;
    }

    /**
     * 分配一段零初始化的 POD 数组。用于 falcon_cloud_file[] / falcon_kv[] /
     * falcon_search_result[] 等跨界数组，避免手工拼 std::string 字节块。
     * 内存同样由宿主分配器分配、由 clear() 统一回收。
     */
    template <typename T>
    T* allocate(uint32_t count) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "Falcon ABI arrays must be trivially copyable");
        if (count == 0 || !host_ || !host_->allocate) return nullptr;
        if (static_cast<uint64_t>(count) >
            std::numeric_limits<size_t>::max() / sizeof(T)) {
            return nullptr;
        }
        const size_t bytes = sizeof(T) * static_cast<size_t>(count);
        void* ptr = host_->allocate(host_->host_data, bytes);
        if (!ptr) return nullptr;
        std::memset(ptr, 0, bytes);
        blocks_.push_back(ptr);
        return static_cast<T*>(ptr);
    }

    void clear() {
        if (host_ && host_->deallocate) {
            for (void* ptr : blocks_) host_->deallocate(host_->host_data, ptr);
        }
        blocks_.clear();
    }

private:
    const falcon_host_api* host_ = nullptr;
    std::vector<void*> blocks_;
};

} // namespace abi
} // namespace falcon
