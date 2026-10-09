// P2SP 公告组件测试（docs/p2sp_network_design.md §16.4 SwarmAnnouncer +
// §16.6 时序注入）。swarm_announcer.cpp 以 #else 分支（无 FALCON_HAS_SWARM）
// 编入本目标：真实网关经 SwarmAnnouncerGateway 接缝替换为记录型 fake，
// 时序用短间隔（poll 10ms）替代虚拟时钟。
//
// 断言形态纪律：
// - 正向等待用 wait_for（2s 预算，2ms 步进）；
// - 负向断言（「不该发生」）在实现正确时不可能变红——若实现有缺陷，
//   缺陷路径会在远小于观测窗口的时间内确定性触发（恒红），无抖动方向。

#include "daemon/swarm_announcer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

using falcon::daemon::P2spConfig;
using falcon::daemon::SwarmAnnounceItem;
using falcon::daemon::SwarmAnnouncer;
using falcon::daemon::SwarmAnnouncerGateway;
using falcon::daemon::SwarmAnnouncerTiming;
using falcon::daemon::SwarmHashFn;
using falcon::daemon::SwarmTaskInfoFn;

// ---------------------------------------------------------------------------
// 测试基建
// ---------------------------------------------------------------------------

constexpr std::size_t kFileSize = 1024;
const std::string kHashA(64, 'a');
const std::string kHashB(64, 'b');

/// 记录型网关：announce/retract 调用全量留痕（条目快照 + 结果序列），
/// 会话串与失败注入可由测试线程驱动。方法可在任意线程调用。
class FakeGateway : public SwarmAnnouncerGateway {
public:
    bool start(std::string* error) override {
        std::lock_guard<std::mutex> lock(mu_);
        ++start_calls_;
        if (!start_ok_) {
            if (error != nullptr) *error = "fake gateway start failure";
            return false;
        }
        return true;
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(mu_);
        ++stop_calls_;
    }

    std::string session() override {
        std::lock_guard<std::mutex> lock(mu_);
        return session_;
    }

    bool announce(const std::vector<SwarmAnnounceItem>& items) override {
        std::lock_guard<std::mutex> lock(mu_);
        announce_calls_.push_back(items);
        const bool ok = fail_next_announces_ <= 0;
        announce_results_.push_back(ok);
        if (fail_next_announces_ > 0) --fail_next_announces_;
        return ok;
    }

    bool retract(const std::vector<std::string>& sha256s) override {
        std::lock_guard<std::mutex> lock(mu_);
        retract_calls_.push_back(sha256s);
        return true;
    }

    // ---- 测试线程观测 / 驱动 ----
    std::size_t announce_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return announce_calls_.size();
    }

    std::vector<std::vector<SwarmAnnounceItem>> announce_calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return announce_calls_;
    }

    std::vector<bool> announce_results() const {
        std::lock_guard<std::mutex> lock(mu_);
        return announce_results_;
    }

    std::size_t retract_count() const {
        std::lock_guard<std::mutex> lock(mu_);
        return retract_calls_.size();
    }

    std::vector<std::vector<std::string>> retract_calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return retract_calls_;
    }

    int stop_calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return stop_calls_;
    }

    void set_session(const std::string& session) {
        std::lock_guard<std::mutex> lock(mu_);
        session_ = session;
    }

    void set_start_ok(bool ok) {
        std::lock_guard<std::mutex> lock(mu_);
        start_ok_ = ok;
    }

    void fail_next_announces(int count) {
        std::lock_guard<std::mutex> lock(mu_);
        fail_next_announces_ = count;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::vector<SwarmAnnounceItem>> announce_calls_;
    std::vector<bool> announce_results_;
    std::vector<std::vector<std::string>> retract_calls_;
    std::string session_ = "s-1";
    bool start_ok_ = true;
    int fail_next_announces_ = 0;
    int start_calls_ = 0;
    int stop_calls_ = 0;
};

/// 任务信息表：task_id → (url, p2sp_share)。仅测试线程访问
///（on_completed 在测试线程同步查询）。
using TaskInfoMap = std::map<falcon::TaskId, std::pair<std::string, std::string>>;

SwarmTaskInfoFn task_info_from(TaskInfoMap& map) {
    return [&map](falcon::TaskId id, std::string& url, std::string& share_flag) {
        const auto it = map.find(id);
        if (it == map.end()) return false;
        url = it->second.first;
        share_flag = it->second.second;
        return true;
    };
}

/// 哈希表：path → sha256；未登记路径返回空串（= 哈希失败）。
struct HashTable {
    std::mutex mu;
    std::map<std::string, std::string> by_path;

    void set(const std::string& path, const std::string& hash) {
        std::lock_guard<std::mutex> lock(mu);
        by_path[path] = hash;
    }

    SwarmHashFn fn() {
        return [this](const std::string& path) -> std::string {
            std::lock_guard<std::mutex> lock(mu);
            const auto it = by_path.find(path);
            return it == by_path.end() ? std::string{} : it->second;
        };
    }
};

/// 写临时文件（gtest_discover_tests 每用例独立进程，固定名安全）
std::string write_temp_file(const std::string& name, const std::string& content) {
    const std::string path = ::testing::TempDir() + name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return path;
}

/// 快时序：poll 10ms；续租上限 60s（不续租，需要续租节拍的用例显式改小）；
/// 退避 10ms 起步、50ms 封顶（jitter 0..12ms）。
SwarmAnnouncerTiming fast_timing() {
    SwarmAnnouncerTiming timing;
    timing.poll_interval = std::chrono::milliseconds(10);
    timing.renewal_cap = std::chrono::seconds(60);
    timing.backoff_base = std::chrono::milliseconds(10);
    timing.backoff_cap = std::chrono::milliseconds(50);
    return timing;
}

template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds budget = std::chrono::seconds(2)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

const SwarmAnnounceItem* find_item(const std::vector<SwarmAnnounceItem>& items,
                                   bool is_file) {
    for (const auto& item : items) {
        if (item.is_file == is_file) return &item;
    }
    return nullptr;
}

/// 两次公告逐字段一致（补差 = 同内容全量重公告；SwarmAnnounceItem 无 ==）
void expect_same_items(const std::vector<SwarmAnnounceItem>& lhs,
                       const std::vector<SwarmAnnounceItem>& rhs) {
    ASSERT_EQ(lhs.size(), rhs.size());
    for (std::size_t i = 0; i < lhs.size(); ++i) {
        EXPECT_EQ(lhs[i].is_file, rhs[i].is_file);
        EXPECT_EQ(lhs[i].sha256, rhs[i].sha256);
        EXPECT_EQ(lhs[i].name, rhs[i].name);
        EXPECT_EQ(lhs[i].has_size, rhs[i].has_size);
        EXPECT_EQ(lhs[i].size, rhs[i].size);
        EXPECT_EQ(lhs[i].url, rhs[i].url);
        EXPECT_EQ(lhs[i].ttl_s, rhs[i].ttl_s);
    }
}

// ---------------------------------------------------------------------------
// 用例
// ---------------------------------------------------------------------------

class SwarmAnnouncerTest : public ::testing::Test {
protected:
    void SetUp() override {
        gateway_ = std::make_unique<FakeGateway>();
        gw_ = gateway_.get();
    }

    /// 以当前 config/timing/tasks/hashes 组装并启动 announcer
    bool start_announcer(std::string* error = nullptr) {
        announcer_ = std::make_unique<SwarmAnnouncer>(
            config_, std::move(gateway_), task_info_from(tasks_), hashes_.fn(),
            timing_);
        return announcer_->start(error);
    }

    void TearDown() override { announcer_.reset(); }

    TaskInfoMap tasks_;
    HashTable hashes_;
    std::unique_ptr<FakeGateway> gateway_;
    FakeGateway* gw_ = nullptr;
    P2spConfig config_;
    SwarmAnnouncerTiming timing_ = fast_timing();
    std::unique_ptr<SwarmAnnouncer> announcer_;
};

//==============================================================================
// 公告内容（§16.4 公告形状 + §16.1 隐私过滤）
//==============================================================================

TEST_F(SwarmAnnouncerTest, StandardModeAnnouncesFileAndMirror) {
    tasks_[1] = {"http://mirror.example/files/video.bin", "true"};
    const std::string path =
        write_temp_file("falcon-ann-standard.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    const auto& items = calls.front();
    ASSERT_EQ(items.size(), 2u);  // R1 文件 + R2 镜像

    const SwarmAnnounceItem* file_item = find_item(items, true);
    const SwarmAnnounceItem* mirror = find_item(items, false);
    ASSERT_NE(file_item, nullptr);
    ASSERT_NE(mirror, nullptr);

    EXPECT_TRUE(file_item->is_file);
    EXPECT_EQ(file_item->sha256, kHashA);
    EXPECT_EQ(file_item->name, "falcon-ann-standard.bin");
    EXPECT_TRUE(file_item->has_size);
    EXPECT_EQ(file_item->size, kFileSize);
    EXPECT_TRUE(file_item->url.empty());
    // TTL 按配置原样上送（钳制在服务器侧）
    EXPECT_EQ(file_item->ttl_s, std::chrono::seconds(86400));

    EXPECT_FALSE(mirror->is_file);
    EXPECT_EQ(mirror->sha256, kHashA);
    EXPECT_EQ(mirror->url, "http://mirror.example/files/video.bin");
    EXPECT_FALSE(mirror->has_size);
    EXPECT_EQ(mirror->ttl_s, std::chrono::seconds(86400));

    EXPECT_EQ(announcer_->active_count(), 1u);
    EXPECT_TRUE(gw_->retract_calls().empty());
}

TEST_F(SwarmAnnouncerTest, HashOnlyModeOmitsNameAndSize) {
    tasks_[1] = {"", ""};  // 镜像关闭时 URL 不参与公告
    const std::string path =
        write_temp_file("falcon-ann-hashonly.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;
    config_.share.mode = "hash_only";
    config_.share.announce_mirrors = false;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    const auto& items = calls.front();
    ASSERT_EQ(items.size(), 1u);
    EXPECT_TRUE(items.front().is_file);
    EXPECT_EQ(items.front().sha256, kHashA);
    // hash_only：资源名与尺寸不上送（防文件名关联）
    EXPECT_TRUE(items.front().name.empty());
    EXPECT_FALSE(items.front().has_size);
    EXPECT_EQ(items.front().size, 0u);
}

TEST_F(SwarmAnnouncerTest, TaskOptOutSkipsAnnouncement) {
    tasks_[1] = {"http://m.example/f.bin", "false"};  // 该任务关闭共享
    const std::string path =
        write_temp_file("falcon-ann-optout.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    // 实现正确时无任何动作；缺陷实现会在远小于窗口内入队/公告
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(gw_->announce_count(), 0u);
    EXPECT_EQ(announcer_->queue_depth(), 0u);
    EXPECT_EQ(announcer_->active_count(), 0u);
}

TEST_F(SwarmAnnouncerTest, MagnetUrlSkippedCaseInsensitive) {
    tasks_[1] = {"MAGNET:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567", ""};
    const std::string path =
        write_temp_file("falcon-ann-magnet.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    // 大写前缀同样被过滤（iequals_prefix）
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(gw_->announce_count(), 0u);
    EXPECT_EQ(announcer_->queue_depth(), 0u);
    EXPECT_EQ(announcer_->active_count(), 0u);
}

TEST_F(SwarmAnnouncerTest, MirrorUrlWithQueryDropped) {
    tasks_[1] = {"http://m.example/f.bin?token=secret", ""};
    const std::string path =
        write_temp_file("falcon-ann-query.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    // 带 query 的 URL 可能含凭据：只公告 R1 文件条目
    ASSERT_EQ(calls.front().size(), 1u);
    EXPECT_TRUE(calls.front().front().is_file);
}

TEST_F(SwarmAnnouncerTest, MirrorUrlWithFragmentDropped) {
    tasks_[1] = {"https://m.example/f.bin#frag", ""};
    const std::string path =
        write_temp_file("falcon-ann-frag.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    ASSERT_EQ(calls.front().size(), 1u);
    EXPECT_TRUE(calls.front().front().is_file);
}

TEST_F(SwarmAnnouncerTest, MirrorUrlNonHttpSchemeDropped) {
    tasks_[1] = {"ftp://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-ftp.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    // 仅 http/https 镜像可公告
    ASSERT_EQ(calls.front().size(), 1u);
    EXPECT_TRUE(calls.front().front().is_file);
}

TEST_F(SwarmAnnouncerTest, AnnounceMirrorsOffDropsMirror) {
    tasks_[1] = {"http://m.example/f.bin", ""};  // 干净 URL：关闸才可验证
    const std::string path =
        write_temp_file("falcon-ann-nomirror.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;
    config_.share.announce_mirrors = false;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    ASSERT_EQ(calls.front().size(), 1u);
    EXPECT_TRUE(calls.front().front().is_file);
}

//==============================================================================
// 队列处理与哈希
//==============================================================================

TEST_F(SwarmAnnouncerTest, HashFailureDropsWithoutRetry) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-hashfail.bin", std::string(kFileSize, 'x'));
    // 不登记哈希 → 注入哈希器返回空串
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->queue_depth() == 0; }));
    // 取出即弃：无重试、无活跃条目
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(gw_->announce_count(), 0u);
    EXPECT_EQ(announcer_->active_count(), 0u);
}

TEST_F(SwarmAnnouncerTest, HashDelayDefersProcessing) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-delay.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;
    config_.share.hash_delay_s = 1;  // 用例只观测 1s 延迟窗口内不处理

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    EXPECT_EQ(announcer_->queue_depth(), 1u);  // 同步入队
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(gw_->announce_count(), 0u);      // 未到 ready_at 不处理
    EXPECT_EQ(announcer_->queue_depth(), 1u);  // 队列保留（不丢弃）
    EXPECT_EQ(announcer_->active_count(), 0u);
}

//==============================================================================
// 活跃表状态机（续租 / 消失 / 变化 / 补差 / 退避）
//==============================================================================

TEST_F(SwarmAnnouncerTest, RenewalPeriodReannouncesSameContent) {
    timing_.renewal_cap = std::chrono::milliseconds(60);
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-renew.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 2; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 2u);
    expect_same_items(calls[0], calls[1]);
    // 文件仍在：只续租不 retract
    EXPECT_TRUE(gw_->retract_calls().empty());
}

TEST_F(SwarmAnnouncerTest, FileGoneRetractsAndDropsEntry) {
    timing_.renewal_cap = std::chrono::milliseconds(60);  // 消失检测随续租节拍
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-gone.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    std::error_code ec;
    fs::remove(path, ec);
    ASSERT_FALSE(ec) << ec.message();

    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));
    const auto retracts = gw_->retract_calls();
    ASSERT_EQ(retracts.size(), 1u);
    EXPECT_EQ(retracts[0], std::vector<std::string>{kHashA});
    // 终态摘除：不再重试
    EXPECT_EQ(announcer_->active_count(), 0u);
}

TEST_F(SwarmAnnouncerTest, FileChangedRehashAnnouncesNewThenRetractsOld) {
    timing_.renewal_cap = std::chrono::milliseconds(60);  // 变化检测随续租节拍
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string name = "falcon-ann-changed.bin";
    const std::string path = write_temp_file(name, std::string(100, 'A'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    // 先改哈希表、后改文件：状态机任何 tick 观测到「新 mtime/尺寸」时，
    // 哈希表必然已是新值——不会落进「同哈希刷新元数据」分支卡死用例
    hashes_.set(path, kHashB);
    write_temp_file(name, std::string(200, 'B'));

    ASSERT_TRUE(wait_for([this] {
        for (const auto& call : gw_->announce_calls()) {
            const SwarmAnnounceItem* file_item = find_item(call, true);
            if (file_item != nullptr && file_item->sha256 == kHashB) return true;
        }
        return false;
    }));
    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));

    // 换源不重叠：retract 的恒为旧哈希，新哈希绝不 retract
    const auto retracts = gw_->retract_calls();
    for (const auto& call : retracts) {
        EXPECT_EQ(call, std::vector<std::string>{kHashA});
    }
    EXPECT_EQ(announcer_->active_count(), 1u);
}

TEST_F(SwarmAnnouncerTest, SessionChangeReannouncesSameContent) {
    // renewal_cap 60s：第二次公告只可能来自 session 变化补差
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-session.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    gw_->set_session("s-2");
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 2; }));

    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 2u);
    // 补差 = 同内容全量重公告
    expect_same_items(calls[0], calls[1]);
    EXPECT_TRUE(gw_->retract_calls().empty());
}

TEST_F(SwarmAnnouncerTest, AnnounceFailureBacksOffThenSucceeds) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-backoff.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;
    gw_->fail_next_announces(1);  // 首次公告失败

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    // 首败 → 指数退避（base 10ms << 1 = 20ms + jitter）→ 重试成功
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 2; }));

    const auto results = gw_->announce_results();
    ASSERT_GE(results.size(), 2u);
    EXPECT_FALSE(results[0]);
    EXPECT_TRUE(results[1]);
    EXPECT_EQ(announcer_->active_count(), 1u);
    EXPECT_TRUE(gw_->retract_calls().empty());
}

//==============================================================================
// 停机 / 热禁用 / 共享翻关 / 启动失败
//==============================================================================

TEST_F(SwarmAnnouncerTest, StopKeepsAnnouncementsWithoutRetract) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-stop.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    // 停机 ≠ 删文件：不 retract（TTL 上界 7d 自愈），网关随工作线程收尾
    announcer_->stop();
    EXPECT_TRUE(gw_->retract_calls().empty());
    EXPECT_GE(gw_->stop_calls(), 1);

    const auto count = gw_->announce_count();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(gw_->announce_count(), count);
}

TEST_F(SwarmAnnouncerTest, DisableRetractsAllAnnouncedHashes) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    tasks_[2] = {"http://m.example/g.bin", ""};
    const std::string path1 =
        write_temp_file("falcon-ann-dis1.bin", std::string(kFileSize, 'x'));
    const std::string path2 =
        write_temp_file("falcon-ann-dis2.bin", std::string(32, 'y'));
    hashes_.set(path1, kHashA);
    hashes_.set(path2, kHashB);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path1);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    // 阻塞至全量 retract 完成后才返回
    announcer_->disable();
    const auto retracts = gw_->retract_calls();
    ASSERT_EQ(retracts.size(), 1u);
    EXPECT_EQ(retracts[0], std::vector<std::string>{kHashA});
    EXPECT_EQ(announcer_->active_count(), 0u);
    EXPECT_GE(gw_->stop_calls(), 1);

    // 关闭后完成事件被拒（accepting 位）
    announcer_->on_completed(2, path2);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(gw_->announce_count(), 1u);
    EXPECT_EQ(announcer_->queue_depth(), 0u);
}

TEST_F(SwarmAnnouncerTest, ApplyShareOffRetractsAllAndIgnoresNewCompletions) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    tasks_[2] = {"http://m.example/g.bin", ""};
    const std::string path1 =
        write_temp_file("falcon-ann-off1.bin", std::string(kFileSize, 'x'));
    const std::string path2 =
        write_temp_file("falcon-ann-off2.bin", std::string(32, 'y'));
    hashes_.set(path1, kHashA);
    hashes_.set(path2, kHashB);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path1);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    P2spConfig::Share off;
    off.enabled = false;
    announcer_->apply_share(off);

    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));
    const auto retracts = gw_->retract_calls();
    ASSERT_EQ(retracts.size(), 1u);
    EXPECT_EQ(retracts[0], std::vector<std::string>{kHashA});
    EXPECT_EQ(announcer_->active_count(), 0u);

    // 翻关后完成事件被过滤
    announcer_->on_completed(2, path2);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(gw_->announce_count(), 1u);
    EXPECT_EQ(announcer_->queue_depth(), 0u);
}

TEST_F(SwarmAnnouncerTest, StartFailurePropagatesGatewayError) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-startfail.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;
    gw_->set_start_ok(false);

    std::string error;
    EXPECT_FALSE(start_announcer(&error));
    EXPECT_NE(error.find("fake gateway start failure"), std::string::npos);

    // 未起工作线程：stop/disable 安全 no-op，后续完成事件被拒
    announcer_->stop();
    announcer_->disable();
    announcer_->on_completed(1, path);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(announcer_->queue_depth(), 0u);
    EXPECT_EQ(gw_->announce_count(), 0u);
}

} // namespace
