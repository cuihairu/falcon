// P2SP 公告组件测试（docs/developer/p2sp-network.md §16.4 SwarmAnnouncer +
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

using falcon::daemon::ISwarmDataRegistry;
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

    std::string node_id() override {
        std::lock_guard<std::mutex> lock(mu_);
        return node_id_;
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
        const bool ok = fail_next_retracts_ <= 0;
        if (fail_next_retracts_ > 0) --fail_next_retracts_;
        return ok;
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

    void set_node_id(const std::string& node_id) {
        std::lock_guard<std::mutex> lock(mu_);
        node_id_ = node_id;
    }

    void set_start_ok(bool ok) {
        std::lock_guard<std::mutex> lock(mu_);
        start_ok_ = ok;
    }

    void fail_next_announces(int count) {
        std::lock_guard<std::mutex> lock(mu_);
        fail_next_announces_ = count;
    }

    void fail_next_retracts(int count) {
        std::lock_guard<std::mutex> lock(mu_);
        fail_next_retracts_ = count;
    }

private:
    mutable std::mutex mu_;
    std::vector<std::vector<SwarmAnnounceItem>> announce_calls_;
    std::vector<bool> announce_results_;
    std::vector<std::vector<std::string>> retract_calls_;
    std::string session_ = "s-1";
    std::string node_id_ = "0123456789abcdef0123456789abcdef";
    bool start_ok_ = true;
    int fail_next_announces_ = 0;
    int fail_next_retracts_ = 0;
    int start_calls_ = 0;
    int stop_calls_ = 0;
};

/// 任务信息表：task_id → (url, p2sp_share)。仅测试线程访问
///（on_completed 在测试线程同步查询）。
using TaskInfoMap = std::map<falcon::TaskId, std::pair<std::string, std::string>>;


TEST_F(SwarmAnnouncerTest, ContentChangeSwapsRegistration) {
    timing_.renewal_cap = std::chrono::milliseconds(60);  // 变化检测随续租节拍
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string name = "falcon-ann-regchg.bin";
    const std::string path = write_temp_file(name, std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    // 先改哈希表、后改文件（同 FileChangedRehashAnnouncesNewThenRetractsOld）
    hashes_.set(path, kHashB);
    write_temp_file(name, std::string(200, 'B'));

    // 换绑先于新公告：观测到新哈希公告即 swap 完成
    ASSERT_TRUE(wait_for([this] {
        for (const auto& call : gw_->announce_calls()) {
            const SwarmAnnounceItem* file_item = find_item(call, true);
            if (file_item != nullptr && file_item->sha256 == kHashB) return true;
        }
        return false;
    }));
    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));

    const auto regs = registry_.register_calls();
    ASSERT_EQ(regs.size(), 2u);
    EXPECT_EQ(regs[0].first, kHashA);
    EXPECT_EQ(regs[0].second, path);
    EXPECT_EQ(regs[1].first, kHashB);
    EXPECT_EQ(regs[1].second, path);
    const auto unregs = registry_.unregister_calls();
    ASSERT_EQ(unregs.size(), 1u);
    EXPECT_EQ(unregs[0], kHashA);
    const auto state = registry_.state();
    ASSERT_EQ(state.size(), 1u);
    EXPECT_EQ(state.at(kHashB), path);
}

TEST_F(SwarmAnnouncerTest, DisableClearsRegistry) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-regdis.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    // 阻塞至全量 retract 完成（retract-all 无条件 clear 台账）
    announcer_->disable();
    const auto retracts = gw_->retract_calls();
    ASSERT_EQ(retracts.size(), 1u);
    EXPECT_EQ(retracts[0], std::vector<std::string>{kHashA});
    EXPECT_EQ(registry_.clear_count(), 1);
    EXPECT_TRUE(registry_.state().empty());
}

TEST_F(SwarmAnnouncerTest, ApplyShareOffClearsRegistry) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-regoff1.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    P2spConfig::Share off;
    off.enabled = false;
    announcer_->apply_share(off);

    ASSERT_TRUE(wait_for([this] { return registry_.clear_count() >= 1; }));
    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));
    EXPECT_TRUE(registry_.state().empty());
    EXPECT_EQ(announcer_->active_count(), 0u);
}

TEST_F(SwarmAnnouncerTest, SetShareEnabledFalseClearsRegistry) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    tasks_[2] = {"http://m.example/g.bin", ""};
    const std::string path1 =
        write_temp_file("falcon-ann-regoff2.bin", std::string(kFileSize, 'x'));
    const std::string path2 =
        write_temp_file("falcon-ann-regoff3.bin", std::string(32, 'y'));
    hashes_.set(path1, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path1);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    announcer_->set_share_enabled(false);
    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));
    ASSERT_TRUE(wait_for([this] { return registry_.clear_count() >= 1; }));
    EXPECT_TRUE(registry_.state().empty());

    // 翻关后完成事件被拒：台账不新增注册
    announcer_->on_completed(2, path2);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(registry_.register_count(), 1u);
    EXPECT_EQ(gw_->announce_count(), 1u);
}

TEST_F(SwarmAnnouncerTest, StopKeepsRegistryIntact) {
    // 停机 ≠ 删文件：stop 不 retract 也不清台账（数据服务继续服务持有文件）
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-regstop.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return announcer_->active_count() == 1; }));

    announcer_->stop();
    EXPECT_TRUE(gw_->retract_calls().empty());
    EXPECT_EQ(registry_.clear_count(), 0);
    EXPECT_EQ(registry_.unregister_count(), 0u);
    const auto state = registry_.state();
    ASSERT_EQ(state.size(), 1u);
    EXPECT_EQ(state.at(kHashA), path);
}

// 同任务 id 再次完成：就绪队列旧项被就地替换（§16.4 替换语义）。
TEST_F(SwarmAnnouncerTest, DuplicateTaskIdReplacesQueuedEntry) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path1 =
        write_temp_file("falcon-ann-dup1.bin", std::string(kFileSize, 'x'));
    const std::string path2 =
        write_temp_file("falcon-ann-dup2.bin", std::string(kFileSize, 'x'));
    hashes_.set(path1, kHashA);
    hashes_.set(path2, kHashB);
    config_.share.enabled = true;
    config_.share.hash_delay_s = 1;  // 延迟窗口内第二次完成触发就地替换

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path1);
    announcer_->on_completed(1, path2);
    EXPECT_EQ(announcer_->queue_depth(), 1u);  // 同 id 不追加
    // 替换项到期后按 path2 处理并公告
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; },
                         std::chrono::seconds(3)));
    const auto calls = gw_->announce_calls();
    ASSERT_GE(calls.size(), 1u);
    const SwarmAnnounceItem* file_item = find_item(calls.front(), true);
    ASSERT_NE(file_item, nullptr);
    EXPECT_EQ(file_item->sha256, kHashB);
    EXPECT_EQ(file_item->name, "falcon-ann-dup2.bin");
}

// 文件消失/stat 失败：记录警告并跳过（不公告、不 retract、不重试）。
TEST_F(SwarmAnnouncerTest, QueueItemStatFailureSkipped) {
    // 队列项 stat 失败：跳过且不重试（无公告、无活跃项、无 retract）。
    tasks_[1] = {"http://m.example/f.bin", ""};
    config_.share.enabled = true;
    config_.share.hash_delay_s = 0;

    ASSERT_TRUE(start_announcer());
    // 路径指向不存在的文件：last_write_time 失败 → process_queue_item 跳过
    const std::string missing =
        ::testing::TempDir() + "falcon-ann-missing.bin";
    announcer_->on_completed(1, missing);

    ASSERT_TRUE(wait_for([this] { return announcer_->queue_depth() == 0; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(gw_->announce_count(), 0u);
    EXPECT_EQ(announcer_->active_count(), 0u);
    EXPECT_TRUE(gw_->retract_calls().empty());
}


// retract-all 失败（热禁用）：记录警告但仍清空台账与活跃表。
TEST_F(SwarmAnnouncerTest, DisableRetractAllFailureStillClears) {
    tasks_[1] = {"http://m.example/f.bin", ""};
    const std::string path =
        write_temp_file("falcon-ann-retfail.bin", std::string(kFileSize, 'x'));
    hashes_.set(path, kHashA);
    config_.share.enabled = true;

    ASSERT_TRUE(start_announcer());
    announcer_->on_completed(1, path);
    ASSERT_TRUE(wait_for([this] { return gw_->announce_count() >= 1; }));

    gw_->fail_next_retracts(1);  // 下一次 retract-all 返回 false
    announcer_->set_share_enabled(false);
    ASSERT_TRUE(wait_for([this] { return gw_->retract_count() >= 1; }));
    EXPECT_EQ(announcer_->active_count(), 0u);
    EXPECT_EQ(registry_.clear_count(), 1);
}


} // namespace
