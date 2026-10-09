/**
 * @file swarm_announcer.hpp
 * @brief P2SP 阶段 1 增量 3：完成后公告 + TTL 续租组件（设计文档
 *        docs/p2sp_network_design.md §16.4）
 *
 * SwarmAnnouncer 挂引擎事件监听（on_completed 入后台哈希队列），哈希与
 * 网络全部在自有工作线程——完成回调路径零新增延迟。活跃公告表由工作
 * 线程独占驱动：TTL 续租（min(5min, ttl/3)）/文件消失 retract/内容变化
 * 重哈希重公告/session 变化全量补差/announce 失败指数退避。
 *
 * 与 SwarmClient 的边界经 SwarmAnnouncerGateway 接缝隔离：真实网关
 * （FALCON_HAS_SWARM，链 falcon_swarm_client）与测试 fake 共用同一
 * 状态机。本头文件不引入 swarmd 侧类型。
 */

#pragma once

#include "daemon/config.hpp"

#include <falcon/download_engine.hpp>
#include <falcon/event_listener.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace falcon::daemon {

/// 公告条目（daemon 侧形状，经网关适配为线上 AnnounceResource）。
/// R1 = file（本机源，name/size 资源级元数据）；R2 = mirror（任务 URL）。
struct SwarmAnnounceItem {
    bool is_file = true;
    std::string sha256;             ///< 64 位小写 hex
    std::string name;               ///< R1：资源名（hash_only 模式省略）
    bool has_size = false;
    std::uint64_t size = 0;         ///< R1：standard 模式上送
    std::string url;                ///< R2：镜像 URL（http/https 且无 ?/#）
    std::chrono::seconds ttl_s{0};  ///< 服务器侧钳 [3600, 604800]
};

/// Swarm 网关接缝（测试注入点）。start() 在注册线程调用一次；其余方法
/// 仅在 announcer 工作线程串行调用（真实 SwarmClient 满足该线程模型）。
class SwarmAnnouncerGateway {
public:
    virtual ~SwarmAnnouncerGateway() = default;
    virtual bool start(std::string* error) = 0;
    virtual void stop() = 0;
    virtual std::string session() = 0;
    virtual std::string node_id() = 0;
    virtual bool announce(const std::vector<SwarmAnnounceItem>& items) = 0;
    virtual bool retract(const std::vector<std::string>& sha256s) = 0;
};

/// 可注入时序（§16.6：测试用短间隔替代虚拟时钟）。
struct SwarmAnnouncerTiming {
    std::chrono::milliseconds poll_interval{200};   ///< 工作线程唤醒粒度
    std::chrono::milliseconds renewal_cap{300000};  ///< 续租周期上限（5min）
    std::chrono::milliseconds backoff_base{1000};   ///< announce 失败退避起点
    std::chrono::milliseconds backoff_cap{300000};  ///< 退避上限（5min）
};

/// 任务信息查询接缝：返回 url 与 options.p2sp_share 三态值；查无任务返 false。
using SwarmTaskInfoFn =
    std::function<bool(falcon::TaskId, std::string& url, std::string& share_flag)>;

/// announcer 快照（falcon.swarm.status 载荷，§16.5）。session/node_id 取
/// 网关侧现值（工作线程私有的 last_session_ 不读）；registered = !session.empty()。
struct SwarmAnnouncerStatus {
    bool running = false;             ///< 工作线程在跑
    bool enabled = false;             ///< share.enabled
    bool registered = false;          ///< 会合服务会话有效
    std::string node_id;              ///< 节点指纹
    std::string session;              ///< 会话 id
    std::size_t announced_count = 0;  ///< 活跃公告数
    std::size_t queue_depth = 0;      ///< 待哈希队列深度
};

/// 文件哈希接缝：path → sha256 hex；失败返回空串。
using SwarmHashFn = std::function<std::string(const std::string& path)>;

class SwarmAnnouncer : public falcon::IEventListener {
public:
    SwarmAnnouncer(P2spConfig config,
                   std::unique_ptr<SwarmAnnouncerGateway> gateway,
                   SwarmTaskInfoFn task_info,
                   SwarmHashFn hasher = nullptr,
                   SwarmAnnouncerTiming timing = {});
    ~SwarmAnnouncer() override;

    SwarmAnnouncer(const SwarmAnnouncer&) = delete;
    SwarmAnnouncer& operator=(const SwarmAnnouncer&) = delete;

    /// 网关启动（注册线程调用）+ 起工作线程。失败返回 false。
    bool start(std::string* error = nullptr);

    /// 停工作线程与网关；**不 retract**——停机不等于删文件，重注册后
    /// 全量重公告补差，异常停机泄漏的公告由 TTL 上界（7d）自愈。
    void stop();

    /// 热禁用（share.enabled → false）：全量 retract（工作线程执行）后停。
    /// 阻塞至 retract 完成（调用方为 SIGHUP/停机路径）。
    void disable();

    /// 热更新 share 参数。生效公告位（enabled && !one_way）翻关 →
    /// 全量 retract + 清活跃表；翻开无动作（新完成事件自然进入）。
    void apply_share(const P2spConfig::Share& share);

    /// 运行期共享开关（falcon.swarm.setShare，§16.5）：只翻 share.enabled，
    /// 其余字段不动。生效位翻关 → 请求全量 retract + 清待处理队列；
    /// announcer 保持存活——与 disable() 的「停机」语义不同。
    void set_share_enabled(bool enabled);

    /// 快照（falcon.swarm.status）。非 const：网关访问器非 const。
    SwarmAnnouncerStatus status();

    /// 引擎完成回调（事件线程）：过滤链 + 入队，零哈希零网络。
    void on_completed(falcon::TaskId task_id,
                      const std::string& output_path) override;

    // 观测（测试）
    std::size_t active_count() const;
    std::size_t queue_depth() const;

private:
    struct QueueItem {
        falcon::TaskId task_id;
        std::string output_path;
        std::string url;
        std::chrono::steady_clock::time_point ready_at;  // 入队时刻 + hash_delay
    };
    struct ActiveEntry {
        std::string output_path;
        std::string url;
        std::string sha256;
        std::string name;             ///< 文件名（R1 元数据）
        std::uint64_t size = 0;       ///< 公告时快照（变化检测）
        std::filesystem::file_time_type mtime{};  ///< 同上
        bool announced_once = false;  ///< 曾成功公告 → 欠 retract 义务
        bool announce_ok = false;     ///< 最近一次 announce 成功
        bool force_announce = false;  ///< session 变化等触发的补差
        int backoff_shift = 0;
        std::chrono::steady_clock::time_point next_action{};
    };

    void worker_loop();
    /// 处理单条队列项（哈希 + 初始公告）；锁外执行重活。
    void process_queue_item(const QueueItem& item);
    /// 活跃表单条状态机（续租/消失/变化/补差）；锁外执行重活。
    void process_active_entry(falcon::TaskId id);
    /// 公告结果回写活跃表；与 retract-all/共享翻关竞速时丢弃回写并
    /// 对刚上线成功的公告补 retract 回滚（announce 是 upsert）。
    /// 内部自行加锁——绝不可在持 mutex_ 时调用。
    void store_back_after_announce(falcon::TaskId id, ActiveEntry entry,
                                   bool announced_ok);
    /// 组装 R1(+R2) 公告条目（§16.4 公告内容 + §16.1 隐私过滤）。
    std::vector<SwarmAnnounceItem> build_items(const ActiveEntry& entry) const;
    std::chrono::milliseconds renewal_period() const;
    std::chrono::milliseconds backoff_delay(int shift) const;

    SwarmAnnouncerTiming timing_;
    SwarmHashFn hasher_;
    SwarmTaskInfoFn task_info_;
    std::unique_ptr<SwarmAnnouncerGateway> gateway_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    P2spConfig::Share share_;            // 热更目标（mutex_ 守护）
    std::atomic<bool> accepting_{false}; // share.enabled && !one_way
    bool stop_requested_ = false;        // mutex_ 守护
    bool stop_after_retract_ = false;    // disable() 语义
    bool retract_all_pending_ = false;   // apply_share/disable 的 retract 请求
    std::deque<QueueItem> queue_;
    std::map<falcon::TaskId, ActiveEntry> active_;
    std::thread worker_;
    bool worker_started_ = false;
    // 工作线程私有（无锁访问）
    std::string last_session_;
};

/// 生产工厂：组装 SwarmClient 网关 + 引擎任务查询 + FileHasher 默认哈希。
/// FALCON_HAS_SWARM 缺席（无 CURL/falcon-swarmd）时返回 nullptr 并告警。
std::unique_ptr<SwarmAnnouncer> make_swarm_announcer(P2spConfig config,
                                                     falcon::DownloadEngine* engine);

}  // namespace falcon::daemon
