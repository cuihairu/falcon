/**
 * @file swarm_data_service.hpp
 * @brief P2SP 阶段 2 增量 1：入站只读 HTTP 数据服务（设计文档
 *        docs/p2sp_network_design.md §10.3）
 *
 * 进程内只读 HTTP 端点 `GET /by-sha256/<hex>`：圈内节点凭已知 sha256
 * 直连本机拉取成品文件字节。支持 Range（V2 P2SP 拉取方的分段请求），
 * 数据出口为成品文件的直接文件流（sendfile 语义）。
 *
 * 关键约束（§10.3 / §6.1）：
 *   - **只服务完整持有的资源**：注册表由「已完成 → 已哈希 → 已公告」链
 *     填充，半成品永不注册（文件名同样只以 sha256 索引对外暴露，不暴露
 *     本地路径）；
 *   - 只读、方法白名单：仅 GET/HEAD，其余 405；路径非 /by-sha256/ 前缀
 *     404；非法 hex 400；未注册 sha256 404；
 *   - 监听端口沿用 json_rpc_server 的 getsockname 回读先例（port=0 时
 *     由 OS 分配随机端口，`port()` 访问器对外）；绑定地址默认私网可达
 *     （配置化）。
 *
 * 线程模型：start() 起一个 accept 线程；每连接一个 detached 连接线程
 * （沿用 daemon RPC / RawWsServer 的会话线程 + 台账 + 有界等待归零收口
 * 形态，见 json_rpc_server 的 ws_clients_ 与 2026-10-01 detached 线程
 * 生命周期教训）。stop() 唤醒 accept 与全部会话线程后等待归零。
 */

#pragma once

#include "daemon/swarm_announcer.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace falcon::daemon {

/// 数据服务配置（daemon.json 的 p2sp.rendezvous 节派生）。
struct SwarmDataServiceConfig {
    std::string bind_address = "0.0.0.0";  ///< 默认私网可达（对齐「绑定地址默认私网」）
    std::uint16_t listen_port = 0;         ///< 0 = OS 分配随机端口
};

/// 从 p2sp.rendezvous.advertise_addr 派生数据服务绑定点（e2e 确定性）：
/// "ip:port" → 按 advertise 声明的端口绑定（公告与实听一致）；空串或
/// 解析失败 → 0.0.0.0:0（OS 分配，port() 回读）。
SwarmDataServiceConfig make_data_service_config(
    const std::string& advertise_addr);

class SwarmDataService final : public ISwarmDataRegistry {
public:
    explicit SwarmDataService(SwarmDataServiceConfig config);
    ~SwarmDataService();

    SwarmDataService(const SwarmDataService&) = delete;
    SwarmDataService& operator=(const SwarmDataService&) = delete;

    /// 起监听 + accept 线程。失败返回 false（错误经 error 回传）。
    bool start(std::string* error = nullptr);

    /// 停机：唤醒 accept 与全部会话线程，等待归零（幂等）。
    void stop();

    /// 注册一个可服务资源（已完成成品：sha256 → 本地路径）。幂等。
    void register_resource(const std::string& sha256_hex,
                           const std::string& path) override;

    /// 撤销注册（文件被删除/共享关闭）。
    void unregister_resource(const std::string& sha256_hex) override;

    /// 清空注册表（共享整体关闭）。
    void clear() override;

    // ---- 观测 ---------------------------------------------------------
    std::uint16_t port() const { return port_; }
    bool running() const { return running_.load(); }
    std::size_t resource_count() const;
    std::uint64_t served_requests() const { return served_requests_.load(); }

private:
    void accept_loop();
    /// 单个连接线程体：解析请求行/头，路由 /by-sha256/<hex>，回响应。
    /// 返回后连接线程自行注销台账（最后一次 this 访问是计数递减）。
    void handle_connection(int fd);

    /// 查询注册表（返回本地路径副本；未注册返回 false）。
    bool lookup(const std::string& sha256_hex, std::string* path) const;

    SwarmDataServiceConfig config_;
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};

    mutable std::mutex resources_mutex_;
    std::map<std::string, std::string> resources_;  ///< sha256 → 本地路径

    std::thread accept_thread_;

    // 会话线程台账（台账纪律对齐 2026-10-01 detached 线程教训：spawn 前
    // 登记 fd、线程体收尾注销、stop() 对存量 fd shutdown 唤醒后有界轮询归零）
    mutable std::mutex conns_mutex_;
    std::condition_variable conns_cv_;
    std::set<int> conn_fds_;
    std::atomic<int> active_conns_{0};

    std::atomic<std::uint64_t> served_requests_{0};
};

}  // namespace falcon::daemon
