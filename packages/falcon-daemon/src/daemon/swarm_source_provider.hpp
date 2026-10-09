#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace falcon {

namespace swarm {
class SwarmClient;  // 前向声明：构造 QueryFn 的工厂在 swarm_announcer.cpp
}  // namespace swarm

namespace daemon {

/**
 * @brief P2SP 查询源 provider（§10.4 查询注入镜像池 + §10.5 NAT 过滤）。
 *
 * 按整文件 sha256 向会合服务查询命中源，产出可注入 V2 镜像池的 URL 表：
 *   - node 源 → http://<advertise addr>/by-sha256/<hex>（§10.3 数据服务）；
 *     过滤规则：advertise 缺席 / addr 为空或不含 ':' / direct==false
 *     （NAT 后不可被动直连）一律跳过；本机自身 node_id 跳过；
 *   - url 源 → 原样透传（仅 http/https）。
 *
 * 降级（§11）：rdv 不可达不是下载失败的来源——查询连续失败按指数退避
 * 抑制（30s × 2^n，封顶 300s），期间 sources_for 直接返回空表（查询不发
 * 往网络）；任一次成功即清除抑制。零查询失败面外泄：所有错误收口为空表。
 *
 * 线程安全：sources_for 可并发调用（退避状态互斥保护）。
 */
class SwarmSourceProvider {
public:
    /// 查询函数抽象：sha256_hex 为小写 64 位 hex；返回 0 = 成功（result
    /// 携带 {sha256, sources:[...]}），非 0 = 失败。宿主用 SwarmClient::query
    /// 适配（见 make_swarm_source_provider）。
    using QueryFn =
        std::function<int(const std::string& sha256_hex, nlohmann::json* result)>;

    SwarmSourceProvider(QueryFn query, std::string own_node_id);

    /// 查询命中源并产出镜像 URL 表（已去重）。任何失败（抑制中 / 查询
    /// 出错 / 响应形状不符）返回空表。
    std::vector<std::string> sources_for(const std::string& sha256_hex);

    /// 退避状态查询（测试观测）：抑制解除时刻。0 = 未抑制。
    std::chrono::steady_clock::time_point suppressed_until() const;

    /// 退避状态查询（测试观测）：连续失败计数。0 = 无失败。
    std::uint32_t consecutive_failures() const;

    /**
     * @brief 纯函数：把 query 结果解析为镜像 URL 表（零网络零状态）。
     * @param result  query 成功时 SwarmClient 填充的 JSON（{sha256, sources}）
     * @param sha256_hex 本任务整文件哈希（node 源 URL 路径用）
     * @param own_node_id 本机节点 id（相等即跳过——不从自己拉数据）
     *
     * 形状容错：result 非 object / sources 非 array / 条目 type 未知 /
     * 字段缺失，全部静默跳过（查询面的失败已被 QueryFn 层收口，此处只
     * 做"能取多少取多少"的解析）。
     */
    static std::vector<std::string> parse_sources(const nlohmann::json& result,
                                                  const std::string& sha256_hex,
                                                  const std::string& own_node_id);

    /// 单条 node 源可用性判定（§10.5）：advertise 对象存在、addr 非空
    /// 且含 ':'（ip:port 形态）、direct==true。NAT 后节点不可作数据源。
    static bool node_source_usable(const nlohmann::json& source);

private:
    QueryFn query_;
    std::string own_node_id_;
    mutable std::mutex mutex_;
    std::uint32_t consecutive_failures_ = 0;
    std::chrono::steady_clock::time_point suppressed_until_{};
};

/// 工厂（定义在 swarm_announcer.cpp——SwarmClient 完整类型与密钥装配
/// 都在那边）：把 SwarmClient 适配成 QueryFn + node_id。
std::shared_ptr<SwarmSourceProvider> make_swarm_source_provider(
    std::shared_ptr<falcon::swarm::SwarmClient> client);

}  // namespace daemon
}  // namespace falcon
