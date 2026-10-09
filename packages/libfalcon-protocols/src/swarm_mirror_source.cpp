/**
 * @file swarm_mirror_source.cpp
 * @brief 外部镜像源注入缝实现（P2SP §10.4）。恒编译——不挂
 *        FALCON_ENABLE_METALINK 门控：缝本身零依赖，metalink 只是
 *        消费方之一。
 */

#include <falcon/protocols/mirror_source.hpp>

#include <mutex>

namespace falcon {

namespace {

std::mutex& mirror_source_mutex() {
    static std::mutex m;
    return m;
}

SwarmMirrorSourceFn& mirror_source_storage() {
    static SwarmMirrorSourceFn fn;
    return fn;
}

}  // namespace

void set_swarm_mirror_source_provider(SwarmMirrorSourceFn provider) {
    std::lock_guard<std::mutex> lock(mirror_source_mutex());
    mirror_source_storage() = std::move(provider);
}

SwarmMirrorSourceFn swarm_mirror_source_snapshot() {
    std::lock_guard<std::mutex> lock(mirror_source_mutex());
    return mirror_source_storage();
}

}  // namespace falcon
