/**
 * @file swarm_announcer.cpp
 * @brief SwarmAnnouncer 状态机实现（设计文档 §16.4）
 *
 * 线程模型：on_completed 在引擎事件线程只做过滤 + 入队（零哈希零网络）；
 * 哈希与网关调用全部在自有工作线程的锁外区执行。apply_share/disable /
 * stop 只置标志 + 唤醒，重活（全量 retract）由工作线程轮次执行。
 *
 * 竞速收口：公告结果回写（store_back_after_announce）在锁内复核
 * retract_all_pending_/accepting_——retract-all 已轮到或共享已翻关时
 * 丢弃回写，并对刚上线成功的公告补一次 retract 回滚（服务器 announce
 * 是 upsert，迟到回写会复活刚被 retract 的条目）。
 *
 * 锁纪律：renewal_period/backoff_delay/build_items/store_back 自行加锁，
 * 任何持 mutex_ 的代码路径绝不可调用它们（std::mutex 非递归）。
 */

#include "daemon/swarm_announcer.hpp"

#include "daemon/swarm_source_provider.hpp"

#include <falcon/logger.hpp>
#include <falcon/protocols/file_hash.hpp>

#include <algorithm>
#include <random>
#include <system_error>
#include <utility>

#ifdef FALCON_HAS_SWARM
#include "client/swarm_client.hpp"
#include "client/swarm_key_store.hpp"
#endif

namespace falcon::daemon {

namespace {

/// ASCII 大小写不敏感前缀比较（MSVC 无 strncasecmp；仅 URL scheme /
/// magnet 前缀判定，非 UTF-8 语义）
bool iequals_prefix(const std::string& value, const char* prefix) {
    for (std::size_t i = 0; prefix[i] != '\0'; ++i) {
        if (i >= value.size()) return false;
        char a = value[i];
        char b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/// 默认哈希：流式 SHA-256（GB 级镜像恒定内存），失败/异常归一空串
std::string default_sha256(const std::string& path) {
    try {
        return FileHasher::calculate_streaming(path, HashAlgorithm::SHA256);
    } catch (...) {
        return std::string{};
    }
}

std::chrono::seconds ttl_seconds(std::uint64_t ttl_s) {
    return std::chrono::seconds(static_cast<std::chrono::seconds::rep>(ttl_s));
}

}  // namespace

SwarmAnnouncer::SwarmAnnouncer(P2spConfig config,
                               std::unique_ptr<SwarmAnnouncerGateway> gateway,
                               SwarmTaskInfoFn task_info,
                               SwarmHashFn hasher,
                               SwarmAnnouncerTiming timing,
                               ISwarmDataRegistry* data_registry)
    : timing_(timing),
      hasher_(hasher ? std::move(hasher) : SwarmHashFn(&default_sha256)),
      task_info_(std::move(task_info)),
      gateway_(std::move(gateway)),
      data_registry_(data_registry),
      share_(config.share) {
    accepting_.store(share_.enabled && !share_.one_way);
}

SwarmAnnouncer::~SwarmAnnouncer() {
    stop();
}

bool SwarmAnnouncer::start(std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (worker_started_) return true;  // 幂等
    }
    // 网关启动（两步注册）在调用方线程完成：失败在起线程之前返回
    std::string gateway_error;
    if (!gateway_->start(&gateway_error)) {
        if (error) *error = gateway_error;
        return false;
    }
    std::thread worker;
    try {
        worker = std::thread([this] { worker_loop(); });
    } catch (const std::system_error& e) {
        gateway_->stop();
        if (error) {
            *error = std::string("failed to start announcer worker: ") + e.what();
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_ = std::move(worker);
        worker_started_ = true;
    }
    return true;
}

void SwarmAnnouncer::stop() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_started_) return;
        stop_requested_ = true;
        cv_.notify_all();
        worker = std::move(worker_);
        worker_started_ = false;
    }
    if (worker.joinable()) worker.join();
    // 不 retract：停机不等于删文件，重注册后全量重公告补差；
    // 异常停机泄漏的公告由 TTL 上界（7d）自愈（§16.4）
}

void SwarmAnnouncer::disable() {
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!worker_started_) {
            accepting_.store(false);
            return;
        }
        stop_after_retract_ = true;
        retract_all_pending_ = true;
        stop_requested_ = true;
        cv_.notify_all();
        worker = std::move(worker_);
        worker_started_ = false;
    }
    if (worker.joinable()) worker.join();
    accepting_.store(false);
}

void SwarmAnnouncer::apply_share(const P2spConfig::Share& share) {
    const bool effective = share.enabled && !share.one_way;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool was_effective = accepting_.load();
        share_ = share;
        if (was_effective && !effective) {
            // 翻关：请求全量 retract + 丢弃待处理队列（on_completed 与此
            // 处的锁内复核竞速漏进来的队列项由工作线程 retract-all 轮次
            // 的 queue_.clear() 兜住）；accepting_ 在锁外翻转
            retract_all_pending_ = true;
            queue_.clear();
        }
    }
    accepting_.store(effective);
    cv_.notify_all();
}

void SwarmAnnouncer::on_completed(falcon::TaskId task_id,
                                  const std::string& output_path) {
    // 过滤链（§16.4）：全局开关（atomic 快路径）→ 空路径 → 任务级
    // p2sp_share 三态（"false" 跳过，""/"true" 放行）→ magnet 前缀
    if (!accepting_.load()) return;
    if (output_path.empty()) return;
    if (!task_info_) return;
    std::string url;
    std::string share_flag;
    if (!task_info_(task_id, url, share_flag)) return;
    if (share_flag == "false") return;
    if (iequals_prefix(url, "magnet:")) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_.load()) return;  // 锁内复核（与 apply_share 竞速）
    if (active_.count(task_id) > 0) return;  // 已在公告表（幂等）
    const auto ready_at = std::chrono::steady_clock::now() +
                          ttl_seconds(share_.hash_delay_s);
    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->task_id == task_id) {  // 同 id 旧排队项就地替换
            it->output_path = output_path;
            it->url = url;
            it->ready_at = ready_at;
            cv_.notify_all();
            return;
        }
    }
    queue_.push_back(QueueItem{task_id, output_path, url, ready_at});
    cv_.notify_all();
}

void SwarmAnnouncer::worker_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        const auto now = std::chrono::steady_clock::now();

        // (0) 全量 retract（disable / share 翻关）——先于一切推进
        if (retract_all_pending_) {
            std::vector<std::string> hashes;
            hashes.reserve(active_.size());
            for (const auto& [id, entry] : active_) {
                if (entry.announced_once) hashes.push_back(entry.sha256);
            }
            active_.clear();
            queue_.clear();
            retract_all_pending_ = false;
            lock.unlock();
            // 数据服务台账同步清空：全量 retract = 本机共享翻关，服务面
            // 不再对外声称持有任何资源（§11）。
            if (data_registry_) data_registry_->clear();
            if (!hashes.empty() && !gateway_->retract(hashes)) {
                FALCON_LOG_WARN_STREAM(
                    "swarm announce: retract-all failed (" << hashes.size()
                                                            << " entries)");
            }
            if (stop_after_retract_) break;
            lock.lock();
            continue;
        }

        // (1) session 变化（心跳重注册）→ 全表补差
        const std::string session = gateway_->session();
        if (session != last_session_) {
            last_session_ = session;
            for (auto& [id, entry] : active_) {
                entry.force_announce = true;
                entry.next_action = now;
            }
        }

        // (2) 到期队列项 → 哈希 + 初始公告（锁外重活）
        std::vector<QueueItem> due_queue;
        for (auto it = queue_.begin(); it != queue_.end();) {
            if (it->ready_at <= now) {
                due_queue.push_back(std::move(*it));
                it = queue_.erase(it);
            } else {
                ++it;
            }
        }
        if (!due_queue.empty()) {
            lock.unlock();
            for (const auto& item : due_queue) process_queue_item(item);
            lock.lock();
        }

        // (3) 到期活跃项 → 续租/消失/变化状态机（锁外重活）
        std::vector<falcon::TaskId> due_ids;
        for (const auto& [id, entry] : active_) {
            if (entry.next_action <= now) due_ids.push_back(id);
        }
        if (!due_ids.empty()) {
            lock.unlock();
            for (const auto id : due_ids) process_active_entry(id);
            lock.lock();
        }

        // (4) 等待；retract 请求优先于停机退出（保持 disable 语义）
        cv_.wait_for(lock, timing_.poll_interval,
                     [this] { return stop_requested_ || retract_all_pending_; });
        if (retract_all_pending_) continue;
        if (stop_requested_) break;
    }
    // disable 的 retract 分支在 unlock 后 break——此时已不持锁；
    // unique_lock::unlock() 对非持有态抛 EPERM，故按持有态收口
    if (lock.owns_lock()) lock.unlock();
    gateway_->stop();  // 单一收尾点：worker 不跑则网关不活
}

void SwarmAnnouncer::process_queue_item(const QueueItem& item) {
    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(item.output_path, ec);
    if (ec) {
        FALCON_LOG_WARN_STREAM("swarm announce: stat failed for "
                               << item.output_path << ": " << ec.message()
                               << ", skipping");
        return;
    }
    const auto size = static_cast<std::uint64_t>(
        std::filesystem::file_size(item.output_path, ec));
    if (ec) {
        FALCON_LOG_WARN_STREAM("swarm announce: stat failed for "
                               << item.output_path << ": " << ec.message()
                               << ", skipping");
        return;
    }

    const std::string hash = hasher_(item.output_path);
    if (hash.empty()) {
        // 哈希失败丢弃不重试（§16.4：文件可能正在被改写，晚到的完成
        // 事件会重新入队）
        FALCON_LOG_WARN_STREAM("swarm announce: hash failed for "
                               << item.output_path << ", dropping");
        return;
    }

    // 数据服务台账注册（§10.3）：哈希成功即「本机完整持有」成立。注册
    // 先于公告——服务面与发现面解耦（公告失败退避重试期间数据服务照常
    // 可应答查询到本机的拉取），与「公告成功才可被远程发现」的发现面
    // 语义互不约束。
    if (data_registry_) data_registry_->register_resource(hash, item.output_path);

    ActiveEntry entry;
    entry.output_path = item.output_path;
    entry.url = item.url;
    entry.sha256 = hash;
    entry.name = std::filesystem::path(item.output_path).filename().string();
    entry.size = size;
    entry.mtime = mtime;

    const bool ok = gateway_->announce(build_items(entry));
    if (ok) {
        FALCON_LOG_INFO_STREAM("swarm announce: published "
                               << entry.sha256.substr(0, 16) << " ("
                               << entry.name << ")");
    } else {
        FALCON_LOG_WARN_STREAM("swarm announce: announce failed for "
                               << entry.name << ", will retry with backoff");
    }
    store_back_after_announce(item.task_id, std::move(entry), ok);
}

void SwarmAnnouncer::store_back_after_announce(falcon::TaskId id,
                                               ActiveEntry entry,
                                               bool announced_ok) {
    // 时序参数先行计算（自带锁，不可在持 mutex_ 时调用）
    const auto renewal = renewal_period();
    const int fail_shift = std::min(entry.backoff_shift + 1, 20);
    const auto retry_delay = backoff_delay(fail_shift);

    bool cancelled = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancelled = retract_all_pending_ || !accepting_.load();
        if (!cancelled) {
            const auto now = std::chrono::steady_clock::now();
            entry.announced_once = entry.announced_once || announced_ok;
            entry.announce_ok = announced_ok;
            if (announced_ok) {
                entry.backoff_shift = 0;
                entry.force_announce = false;
                entry.next_action = now + renewal;
            } else {
                entry.backoff_shift = fail_shift;
                entry.next_action = now + retry_delay;
            }
            active_[id] = std::move(entry);
        }
    }
    // 竞速回滚：retract-all 已轮到（活跃表已清）或共享已翻关——本次成功
    // 的 announce（无论初始还是续租）刚刚复活/续命了该条目，补 retract。
    // 额外 retract 同一哈希无害（§16.2 unknown 语义）。
    if (cancelled && announced_ok) {
        gateway_->retract({entry.sha256});
    }
}

void SwarmAnnouncer::process_active_entry(falcon::TaskId id) {
    ActiveEntry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = active_.find(id);
        if (it == active_.end()) return;  // 被 retract-all/摘表收走
        entry = it->second;               // 快照处理，结果经 store_back 回写
    }

    std::error_code ec;
    const auto mtime = std::filesystem::last_write_time(entry.output_path, ec);
    if (!ec) {
        const auto size = static_cast<std::uint64_t>(
            std::filesystem::file_size(entry.output_path, ec));
        if (!ec && (mtime != entry.mtime || size != entry.size)) {
            // 内容可能变化 → 重哈希
            const std::string hash = hasher_(entry.output_path);
            if (hash.empty()) {
                const int shift = std::min(entry.backoff_shift + 1, 20);
                const auto delay = backoff_delay(shift);
                std::lock_guard<std::mutex> lock(mutex_);
                const auto it = active_.find(id);
                if (it != active_.end()) {
                    it->second.announce_ok = false;
                    it->second.backoff_shift = shift;
                    it->second.next_action =
                        std::chrono::steady_clock::now() + delay;
                }
                return;
            }
            if (hash == entry.sha256) {
                // 同哈希：元数据刷新 + 续租
                entry.mtime = mtime;
                entry.size = size;
                const bool ok = gateway_->announce(build_items(entry));
                store_back_after_announce(id, std::move(entry), ok);
                return;
            }
            // 新内容：先公告新哈希（无缝衔接），再无条件 retract 旧哈希
            // 数据服务台账同步换绑：旧哈希摘除、新哈希注册
            if (data_registry_) {
                data_registry_->unregister_resource(entry.sha256);
                data_registry_->register_resource(hash, entry.output_path);
            }
            ActiveEntry fresh;
            fresh.output_path = entry.output_path;
            fresh.url = entry.url;
            fresh.sha256 = hash;
            fresh.name = entry.name;
            fresh.size = size;
            fresh.mtime = mtime;
            const bool ok = gateway_->announce(build_items(fresh));
            if (entry.announced_once) gateway_->retract({entry.sha256});
            store_back_after_announce(id, std::move(fresh), ok);
            return;
        }
    }
    if (ec) {
        // 文件消失/不可访问 → 欠账 retract + 摘表（无重试：消失是终态）
        // 数据服务台账同步摘除（「完整持有」不再成立）
        if (data_registry_) data_registry_->unregister_resource(entry.sha256);
        if (entry.announced_once) gateway_->retract({entry.sha256});
        std::lock_guard<std::mutex> lock(mutex_);
        active_.erase(id);
        return;
    }

    // 未变化：TTL 续租 / 失败退避重试 / session 补差
    const bool ok = gateway_->announce(build_items(entry));
    store_back_after_announce(id, std::move(entry), ok);
}

std::vector<SwarmAnnounceItem> SwarmAnnouncer::build_items(
    const ActiveEntry& entry) const {
    P2spConfig::Share share;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        share = share_;
    }

    const auto ttl = ttl_seconds(share.ttl_s);

    std::vector<SwarmAnnounceItem> items;
    items.reserve(2);

    SwarmAnnounceItem file;
    file.is_file = true;
    file.sha256 = entry.sha256;
    file.ttl_s = ttl;
    if (share.mode == "standard") {  // hash_only：只上哈希（§16.1）
        file.name = entry.name;
        file.has_size = true;
        file.size = entry.size;
    }
    items.push_back(std::move(file));

    // R2 隐私过滤（§16.1）：仅 http/https 且无 query/fragment 的镜像 URL
    if (share.announce_mirrors && !entry.url.empty() &&
        (iequals_prefix(entry.url, "http://") ||
         iequals_prefix(entry.url, "https://")) &&
        entry.url.find('?') == std::string::npos &&
        entry.url.find('#') == std::string::npos) {
        SwarmAnnounceItem mirror;
        mirror.is_file = false;
        mirror.sha256 = entry.sha256;
        mirror.url = entry.url;
        mirror.ttl_s = ttl;
        items.push_back(std::move(mirror));
    }
    return items;
}

std::chrono::milliseconds SwarmAnnouncer::renewal_period() const {
    std::uint64_t ttl_s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ttl_s = share_.ttl_s;
    }
    if (ttl_s > 604800) ttl_s = 604800;  // 服务器上界（§16.5），兼防溢出
    const auto period = std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(ttl_s) * 1000 / 3);
    return std::min(std::max(period, timing_.poll_interval), timing_.renewal_cap);
}

std::chrono::milliseconds SwarmAnnouncer::backoff_delay(int shift) const {
    if (shift <= 0) return timing_.backoff_base;
    // shift 由调用方钳到 20、base 秒级毫秒——int64 移位不溢出
    const auto shifted = timing_.backoff_base.count() << shift;
    const auto capped = std::min(shifted, timing_.backoff_cap.count());
    // 0..25% 抖动打散重试风暴（§16.4）
    static thread_local std::mt19937 rng{std::random_device{}()};
    // 分布状态随调用变化（operator() 非 const），不得声明为 const
    std::uniform_int_distribution<std::int64_t> dist{0, capped / 4};
    return std::chrono::milliseconds{capped + dist(rng)};
}

std::size_t SwarmAnnouncer::active_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_.size();
}

std::size_t SwarmAnnouncer::queue_depth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

void SwarmAnnouncer::set_share_enabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (share_.enabled == enabled) return;  // 幂等
        share_.enabled = enabled;               // 其余字段不动
        const bool effective = enabled && !share_.one_way;
        if (!effective) {
            // 翻关：请求全量 retract + 丢弃待处理队列（同 apply_share）；
            // announcer 保持存活，新开启无动作（新完成事件自然进入）
            retract_all_pending_ = true;
            queue_.clear();
            accepting_.store(false);
        } else {
            accepting_.store(true);
        }
    }
    cv_.notify_all();
}

SwarmAnnouncerStatus SwarmAnnouncer::status() {
    // 互斥量成员锁内快照；session/node_id 走网关（SwarmClient 访问器
    // 自带锁，且非工作线程独占）在锁外读——last_session_ 是工作线程
    // 私有（无锁），绝不可在此读取
    SwarmAnnouncerStatus snap;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snap.running = worker_started_;
        snap.enabled = share_.enabled;
        snap.announced_count = active_.size();
        snap.queue_depth = queue_.size();
    }
    if (gateway_) {
        snap.session = gateway_->session();
        snap.node_id = gateway_->node_id();
    }
    snap.registered = !snap.session.empty();
    return snap;
}

#ifdef FALCON_HAS_SWARM

namespace {

/// SwarmAnnouncerGateway 的 SwarmClient 适配器。线程模型对齐：start()
/// 在注册线程一次；announce/retract/session/stop 仅在 announcer 工作
/// 线程串行调用（SwarmClient 自身线程安全，约束天然满足）。
/// stop() 有意 no-op：进程级共享客户端的会话生命周期归 main 持有——
/// announcer 停止/热禁用后查询面（§11）仍须存活，客户端无人 stop，
/// 析构随进程尾随 shared_ptr 归零自然收口。
class SwarmClientGateway final : public SwarmAnnouncerGateway {
public:
    explicit SwarmClientGateway(std::shared_ptr<falcon::swarm::SwarmClient> client)
        : client_(std::move(client)) {}

    bool start(std::string* error) override {
        // SwarmClient::start 非线程安全（幂等但无内部互斥）：热启用路径
        // 可能与既有 announcer 的工作线程尾随 start 竞速，进程级互斥兜底。
        static std::mutex client_start_mutex;
        std::lock_guard<std::mutex> lock(client_start_mutex);
        return client_->start(error);
    }
    void stop() override {}  // no-op——见类注释
    std::string session() override { return client_->session(); }
    std::string node_id() override { return client_->node_id(); }

    bool announce(const std::vector<SwarmAnnounceItem>& items) override {
        std::vector<falcon::swarm::AnnounceResource> resources;
        resources.reserve(items.size());
        for (const auto& item : items) {
            falcon::swarm::AnnounceResource res;
            res.kind = item.is_file ? "file" : "mirror";
            res.sha256 = item.sha256;
            if (item.is_file) {
                res.name = item.name;
                res.has_size = item.has_size;
                res.size = item.size;
            } else {
                res.url = item.url;
            }
            res.ttl_s = item.ttl_s;
            resources.push_back(std::move(res));
        }
        nlohmann::json result;
        const auto err = client_->announce(resources, &result);
        if (!err.ok()) {
            // -32003（session 竞速）等可重试错误同样返回 false——状态机
            // 指数退避重试，心跳自愈后自然恢复（§16.4）
            FALCON_LOG_WARN_STREAM("swarm announce: gateway error "
                                   << err.code << ": " << err.message);
            return false;
        }
        if (result.is_object() && result.value("rejected", 0) > 0) {
            FALCON_LOG_WARN_STREAM("swarm announce: server rejected "
                                   << result.value("rejected", 0) << " of "
                                   << resources.size() << " entries");
        }
        return true;
    }

    bool retract(const std::vector<std::string>& sha256s) override {
        nlohmann::json result;
        const auto err = client_->retract(sha256s, &result);
        if (!err.ok()) {
            FALCON_LOG_WARN_STREAM("swarm announce: retract error "
                                   << err.code << ": " << err.message);
            return false;
        }
        return true;
    }

private:
    std::shared_ptr<falcon::swarm::SwarmClient> client_;
};

}  // namespace

std::shared_ptr<falcon::swarm::SwarmClient> make_swarm_client(
    const P2spConfig& config) {
    const std::string key_file = get_default_config_dir() + "/swarm_key.pem";
    std::string key_error;
    const auto key = falcon::swarm::load_or_create_swarm_key(key_file, &key_error);
    if (!key.valid()) {
        FALCON_LOG_WARN_STREAM("swarm client: identity key unavailable ("
                               << key_error << "), swarm disabled");
        return nullptr;
    }

    falcon::swarm::SwarmClientConfig client_config;
    client_config.host = config.rendezvous.host;
    client_config.port = static_cast<int>(config.rendezvous.port);
    client_config.server_token = config.rendezvous.server_token;
    client_config.group_token = config.rendezvous.group_token;
    client_config.agent = "falcon-daemon";
    client_config.advertise_addr = config.rendezvous.advertise_addr;
    client_config.advertise_direct = config.rendezvous.advertise_direct;
    client_config.key_file = key_file;

    // 只创建不启动：start 由首个使用者（gateway 或查询 provider 侧）触发，
    // 幂等。stop 无人调——公告停了查询仍要活（§11），进程尾随析构收口。
    return std::make_shared<falcon::swarm::SwarmClient>(client_config, key);
}

std::unique_ptr<SwarmAnnouncer> make_swarm_announcer(
    P2spConfig config, falcon::DownloadEngine* engine,
    std::shared_ptr<falcon::swarm::SwarmClient> shared_client,
    ISwarmDataRegistry* data_registry) {
    if (!engine) return nullptr;

    auto client = shared_client ? std::move(shared_client) : make_swarm_client(config);
    if (!client) return nullptr;

    auto gateway = std::make_unique<SwarmClientGateway>(std::move(client));

    SwarmTaskInfoFn task_info = [engine](falcon::TaskId id, std::string& url,
                                         std::string& share_flag) {
        const auto task = engine->get_task(id);
        if (!task) return false;
        url = task->url();
        share_flag = task->options().p2sp_share;
        return true;
    };

    return std::make_unique<SwarmAnnouncer>(std::move(config), std::move(gateway),
                                            std::move(task_info), nullptr,
                                            SwarmAnnouncerTiming{},
                                            data_registry);
}

std::shared_ptr<SwarmSourceProvider> make_swarm_source_provider(
    std::shared_ptr<falcon::swarm::SwarmClient> client) {
    if (!client) return nullptr;
    // QueryFn 适配：SwarmClient::query 线程安全（per-call curl handle，
    // session 读经互斥），并发查询面满足。非 0 = 查询失败（含 rdv 不可达）。
    auto query = [client](const std::string& sha256_hex,
                          nlohmann::json* result) -> int {
        return client->query(sha256_hex, result).code;
    };
    return std::make_shared<SwarmSourceProvider>(std::move(query),
                                                 client->node_id());
}

#else  // FALCON_HAS_SWARM

std::unique_ptr<SwarmAnnouncer> make_swarm_announcer(
    P2spConfig /*config*/, falcon::DownloadEngine* /*engine*/,
    std::shared_ptr<falcon::swarm::SwarmClient> /*shared_client*/,
    ISwarmDataRegistry* /*data_registry*/) {
    FALCON_LOG_WARN(
        "swarm announce: swarm client support not built (CURL/falcon-swarmd "
        "unavailable), announcing disabled");
    return nullptr;
}

std::shared_ptr<falcon::swarm::SwarmClient> make_swarm_client(
    const P2spConfig& /*config*/) {
    FALCON_LOG_WARN(
        "swarm client: swarm support not built (CURL/falcon-swarmd "
        "unavailable), swarm disabled");
    return nullptr;
}

std::shared_ptr<SwarmSourceProvider> make_swarm_source_provider(
    std::shared_ptr<falcon::swarm::SwarmClient> /*client*/) {
    return nullptr;
}

#endif  // FALCON_HAS_SWARM

}  // namespace falcon::daemon
