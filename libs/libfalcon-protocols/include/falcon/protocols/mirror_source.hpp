#pragma once

#include <functional>
#include <string>
#include <vector>

namespace falcon {

/**
 * @brief 外部镜像源注入缝（P2SP §10.4）。
 *
 * swarmd 客户端（falcon-daemon 内）在下载启动前按 sha256 查询会合服务，
 * 把命中的节点源/镜像 URL 源注入 V2 引擎镜像池参与段级换源。协议库自身
 * 不依赖 swarmd（依赖方向纪律：protocols 不反向依赖 daemon 包），经本缝
 * 回调取源；宿主未注册回调时查询返回空表，metalink 桥接行为与既有一致。
 *
 * 线程安全：setter 与 snapshot 可并发调用（内部互斥锁守护静态存储）。
 * 回调在 metalink 下载工作线程内执行，宿主侧须自行保证线程安全。
 */
using SwarmMirrorSourceFn =
    std::function<std::vector<std::string>(const std::string& sha256_hex)>;

/// 注册（或以 nullptr 摘除）查询源回调。
void set_swarm_mirror_source_provider(SwarmMirrorSourceFn provider);

/// 快照当前回调；未注册返回空 function。
SwarmMirrorSourceFn swarm_mirror_source_snapshot();

}  // namespace falcon
