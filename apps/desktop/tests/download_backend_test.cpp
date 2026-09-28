// IDownloadBackend 两个实现的回环测试（纯 C++，不依赖 Qt）。
//
// - DaemonRpcBackend × 真实 JsonRpcServer：add/pause/resume/优先级/删除/
//   全局设置/快照与统计解析全链路
// - InProcessBackend × 默认引擎：拒绝不支持 URL、快照/统计查询不崩溃

#include "services/download_backend.hpp"

#include "rpc/json_rpc_server.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace {

using namespace falcon::desktop;
using json = nlohmann::json;

// download() 阻塞到任务进入 Paused/终态（同 json_rpc_client_test 的 harness）
class BlockingHandler final : public falcon::IProtocolHandler {
public:
    std::string protocol_name() const override { return "test"; }

    std::vector<std::string> supported_schemes() const override { return {"test"}; }

    bool can_handle(const std::string& url) const override {
        return url.rfind("test://", 0) == 0;
    }

    falcon::FileInfo get_file_info(const std::string& url,
                                   const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "blocking.bin";
        info.total_size = 1000;
        info.supports_resume = true;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        while (!task->is_finished() && task->status() != falcon::TaskStatus::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    void pause(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Paused);
    }

    void resume(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->set_status(falcon::TaskStatus::Downloading);
    }

    void cancel(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Cancelled);
    }

    bool supports_resume() const override { return true; }
};

// 做种形态 handler（seed://）：download() 先发布做种快照（seeding_active=
// true），随后等待暂停/终态/停止做种请求——收到停止请求后发布终态做种
// 快照（数值与做种中可区分）并置 Completed（BT 插件同语义：终态由
// handler 落定，worker 只兜异常）
class SeedingHandler final : public falcon::IProtocolHandler {
public:
    static constexpr double kActiveSeconds = 125.0;
    static constexpr std::uint64_t kActiveUploaded = 2048;
    static constexpr double kFinalSeconds = 250.0;
    static constexpr std::uint64_t kFinalUploaded = 4096;

    std::string protocol_name() const override { return "seed"; }

    std::vector<std::string> supported_schemes() const override { return {"seed"}; }

    bool can_handle(const std::string& url) const override {
        return url.rfind("seed://", 0) == 0;
    }

    falcon::FileInfo get_file_info(const std::string& url,
                                   const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "seeding.bin";
        info.total_size = 1000;
        info.supports_resume = true;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        falcon::SeedInfo info;
        info.uploaded_bytes = kActiveUploaded;
        info.downloaded_bytes = 1000;
        info.total_size = 1000;
        info.seeded_seconds = kActiveSeconds;
        info.seeding_active = true;
        task->update_seed_info(info);

        while (!task->is_finished() && task->status() != falcon::TaskStatus::Paused &&
               !task->stop_seeding_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        if (task->stop_seeding_requested() && !task->is_finished()) {
            falcon::SeedInfo final_info = info;
            final_info.uploaded_bytes = kFinalUploaded;
            final_info.seeded_seconds = kFinalSeconds;
            final_info.seeding_active = false;
            task->update_seed_info(final_info);
            task->set_status(falcon::TaskStatus::Completed);
        }
    }

    void pause(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Paused);
    }

    void resume(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->set_status(falcon::TaskStatus::Downloading);
    }

    void cancel(falcon::DownloadTask::Ptr task) override {
        task->set_status(falcon::TaskStatus::Cancelled);
    }

    bool supports_resume() const override { return true; }
};

class DownloadBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.register_handler(std::make_unique<BlockingHandler>());
        engine_.register_handler(std::make_unique<SeedingHandler>());

        cfg_.listen_port = 0;
        cfg_.secret = "test-secret";
        cfg_.allow_origin_all = false;
        cfg_.bind_address = "127.0.0.1";

        server_ = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(
            &engine_, cfg_, /*storage=*/nullptr);
        ASSERT_TRUE(server_->start());
        ASSERT_NE(server_->port(), 0);

        falcon::daemon::rpc::JsonRpcClientConfig rpc_cfg;
        rpc_cfg.url = "http://127.0.0.1:" + std::to_string(server_->port()) + "/jsonrpc";
        rpc_cfg.secret = cfg_.secret;
        rpc_cfg.timeout_seconds = 5;

        daemon_backend_ = make_daemon_rpc_backend(std::move(rpc_cfg));
        ASSERT_TRUE(daemon_backend_);
    }

    void TearDown() override {
        for (const auto& task : engine_.get_all_tasks()) {
            if (task && !task->is_finished()) task->cancel();
        }
        for (int i = 0; i < 2500; ++i) {
            bool all_done = true;
            for (const auto& task : engine_.get_all_tasks()) {
                if (task && !task->is_finished()) {
                    all_done = false;
                    break;
                }
            }
            if (all_done) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (server_) server_->stop();
    }

    /// 添加一个活动任务并等待 daemon 侧进入 active
    falcon::TaskId add_active_task(const std::string& name) {
        falcon::DownloadOptions options;
        options.output_directory = "/tmp";
        auto result = daemon_backend_->add_task("test://" + name, options, true);
        EXPECT_TRUE(result.ok) << result.error;
        EXPECT_NE(result.id, 0u);
        return result.id;
    }

    falcon::DownloadEngine engine_;
    falcon::daemon::rpc::JsonRpcServerConfig cfg_;
    std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> server_;
    std::unique_ptr<IDownloadBackend> daemon_backend_;
};

TEST_F(DownloadBackendTest, DaemonAddTaskRoundTrip) {
    const auto id = add_active_task("backend-add");

    // addUri 返回与 daemon 侧任务真正进入 Downloading 之间是异步窗口
    // （TaskManager worker 出队启动）——立即断言会观察到 Pending
    // （本机实测 ~40% 命中），轮询等待状态到达后再断言其余字段
    std::vector<falcon::daemon::rpc::TaskSnapshot> tasks;
    bool downloading = false;
    for (int i = 0; i < 2500; ++i) {
        tasks = daemon_backend_->fetch_tasks();
        if (tasks.size() == 1 &&
            tasks[0].status == falcon::TaskStatus::Downloading) {
            downloading = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    ASSERT_TRUE(downloading);
    EXPECT_EQ(tasks[0].id, id);
    EXPECT_EQ(tasks[0].status, falcon::TaskStatus::Downloading);
    EXPECT_EQ(tasks[0].url, "test://backend-add");
    // daemon 的 tellStatus 路径 = dir + URL 推断文件名；
    // 未传 out 选项时 handler 的 file_info 不参与
    EXPECT_EQ(tasks[0].output_path, "/tmp/backend-add");
    EXPECT_EQ(tasks[0].total_bytes, 0u);
    EXPECT_EQ(tasks[0].priority, falcon::TaskPriority::Normal);

    auto stats = daemon_backend_->fetch_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_GE(stats->active_tasks, 1u);
}

TEST_F(DownloadBackendTest, DaemonPauseResumePriorityRemove) {
    const auto id = add_active_task("backend-ctrl");

    EXPECT_TRUE(daemon_backend_->pause_task(id));
    EXPECT_TRUE(daemon_backend_->resume_task(id));
    EXPECT_TRUE(daemon_backend_->set_priority(id, falcon::TaskPriority::High));

    // 引擎侧确认优先级已经生效
    auto task = engine_.get_task(id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->get_priority(), falcon::TaskPriority::High);

    EXPECT_TRUE(daemon_backend_->remove_task(id));

    // aria2 语义：remove 后任务转入 stopped 列表（removed 状态），purge 前仍可见
    bool still_listed = false;
    falcon::TaskStatus status_after_remove = falcon::TaskStatus::Downloading;
    for (const auto& snap : daemon_backend_->fetch_tasks()) {
        if (snap.id == id) {
            still_listed = true;
            status_after_remove = snap.status;
        }
    }
    if (still_listed) {
        EXPECT_TRUE(status_after_remove == falcon::TaskStatus::Cancelled ||
                    status_after_remove == falcon::TaskStatus::Completed ||
                    status_after_remove == falcon::TaskStatus::Failed);
    }

    // aria2 不回报清除数量，实现固定返回 0
    EXPECT_EQ(daemon_backend_->remove_finished_tasks(), 0u);
}

TEST_F(DownloadBackendTest, DaemonRejectsUnsupportedUrl) {
    falcon::DownloadOptions options;
    auto result = daemon_backend_->add_task("no-such-scheme://x", options, true);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.id, 0u);
    EXPECT_FALSE(result.error.empty());
}

TEST_F(DownloadBackendTest, DaemonApplyGlobalSettings) {
    daemon_backend_->apply_global_settings(7, 8192);
    EXPECT_EQ(engine_.get_max_concurrent_tasks(), 7u);
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{8192});
}

TEST_F(DownloadBackendTest, DaemonWakeCallbackOnNotification) {
    std::atomic<int> wakes{0};
    daemon_backend_->set_wake_callback([&wakes]() { wakes.fetch_add(1); });

    add_active_task("backend-wake");

    // daemon WebSocket 事件流通知（任务状态变更）应触发唤醒回调
    for (int i = 0; i < 2500 && wakes.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_GT(wakes.load(), 0);

    // 解除注册：返回后保证不再有在途调用
    daemon_backend_->set_wake_callback({});
}

TEST_F(DownloadBackendTest, DaemonSeedStatsSnapshotRoundtripAndStopSeeding) {
    falcon::DownloadOptions options;
    options.output_directory = "/tmp";
    auto result = daemon_backend_->add_task("seed://roundtrip", options, true);
    ASSERT_TRUE(result.ok) << result.error;
    const auto id = result.id;

    // 等做种快照经 tellStatus Falcon 扩展字段（seedUploadedBytes 等）
    // 往返到达桌面快照——handler 发布之前 seeding_active 恒 false
    std::vector<falcon::daemon::rpc::TaskSnapshot> tasks;
    std::optional<falcon::daemon::rpc::TaskSnapshot> seeding_snap;
    for (int i = 0; i < 2500 && !seeding_snap; ++i) {
        tasks = daemon_backend_->fetch_tasks();
        for (const auto& t : tasks) {
            if (t.id == id && t.seeding_active) {
                seeding_snap = t;
                break;
            }
        }
        if (!seeding_snap) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ASSERT_TRUE(seeding_snap);
    EXPECT_EQ(seeding_snap->seed_uploaded_bytes, SeedingHandler::kActiveUploaded);
    EXPECT_EQ(seeding_snap->seed_downloaded_bytes, 1000u);
    EXPECT_EQ(seeding_snap->seed_total_size, 1000u);
    EXPECT_DOUBLE_EQ(seeding_snap->seeded_seconds, SeedingHandler::kActiveSeconds);
    EXPECT_TRUE(seeding_snap->seeding_active);
    // ratio 分母 = max(downloaded, total_size) = 1000 → 2048/1000
    EXPECT_NEAR(seeding_snap->seed_ratio(), 2.048, 1e-9);

    // 手动停止做种：RPC falcon.stopSeeding → 粘性标志 → handler 终态收口
    EXPECT_TRUE(daemon_backend_->stop_seeding(id));

    std::optional<falcon::daemon::rpc::TaskSnapshot> stopped_snap;
    for (int i = 0; i < 2500 && !stopped_snap; ++i) {
        tasks = daemon_backend_->fetch_tasks();
        for (const auto& t : tasks) {
            if (t.id == id && !t.seeding_active &&
                t.seed_uploaded_bytes == SeedingHandler::kFinalUploaded) {
                stopped_snap = t;
                break;
            }
        }
        if (!stopped_snap) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
    ASSERT_TRUE(stopped_snap);
    EXPECT_DOUBLE_EQ(stopped_snap->seeded_seconds, SeedingHandler::kFinalSeconds);
    EXPECT_FALSE(stopped_snap->seeding_active);
    EXPECT_EQ(stopped_snap->status, falcon::TaskStatus::Completed);
}

TEST_F(DownloadBackendTest, DaemonStopSeedingErrorPaths) {
    // 非做种任务（BlockingHandler 从不发布做种快照）→ code 1 → false
    const auto id = add_active_task("stop-seed-err");
    EXPECT_FALSE(daemon_backend_->stop_seeding(id));

    // 不存在的 gid → code 2 → false
    EXPECT_FALSE(daemon_backend_->stop_seeding(0xDEADBEEFull));
}

TEST_F(DownloadBackendTest, DaemonApplySeedDefaultsSentinelAndExplicit) {
    daemon_backend_->apply_seed_defaults(2.5, 90);

    // 哨兵路径（IPC/扩展）：seed_ratio < 0 → 全局默认填充（两字段同填）
    falcon::DownloadOptions sentinel;
    sentinel.output_directory = "/tmp";
    sentinel.seed_ratio = -1.0;
    auto r1 = daemon_backend_->add_task("test://seed-sentinel", sentinel, true);
    ASSERT_TRUE(r1.ok) << r1.error;
    auto t1 = engine_.get_task(r1.id);
    ASSERT_TRUE(t1);
    EXPECT_DOUBLE_EQ(t1->options().seed_ratio, 2.5);
    EXPECT_EQ(t1->options().seed_time_minutes, 90u);

    // 显式路径（对话框）：>= 0 两字段原样透传，不被全局默认覆写
    falcon::DownloadOptions explicit_opts;
    explicit_opts.output_directory = "/tmp";
    explicit_opts.seed_ratio = 0.5;
    explicit_opts.seed_time_minutes = 30;
    auto r2 = daemon_backend_->add_task("test://seed-explicit", explicit_opts, true);
    ASSERT_TRUE(r2.ok) << r2.error;
    auto t2 = engine_.get_task(r2.id);
    ASSERT_TRUE(t2);
    EXPECT_DOUBLE_EQ(t2->options().seed_ratio, 0.5);
    EXPECT_EQ(t2->options().seed_time_minutes, 30u);
}

TEST_F(DownloadBackendTest, DaemonAddUriSeedOptionStringAndClamp) {
    // 直连 RPC：字符串形态解析 + 负值钳 0（daemon addUri 门禁语义）
    falcon::daemon::rpc::JsonRpcClientConfig rpc_cfg;
    rpc_cfg.url = "http://127.0.0.1:" + std::to_string(server_->port()) + "/jsonrpc";
    rpc_cfg.secret = cfg_.secret;
    rpc_cfg.timeout_seconds = 5;
    falcon::daemon::rpc::JsonRpcClient client(rpc_cfg);

    falcon::daemon::rpc::JsonRpcError err;
    auto gid = client.add_uri(
        {"seed://clamp"},
        json{{"seed-ratio", "-3.5"}, {"seed-time", "-7"}, {"dir", "/tmp"}},
        &err);
    ASSERT_TRUE(gid) << (err.message.empty() ? "addUri failed" : err.message);
    const auto id = falcon::daemon::rpc::task_id_from_gid(*gid);
    ASSERT_TRUE(id);
    auto task = engine_.get_task(*id);
    ASSERT_TRUE(task);
    EXPECT_DOUBLE_EQ(task->options().seed_ratio, 0.0);
    EXPECT_EQ(task->options().seed_time_minutes, 0u);

    // 数值形态 + 合法值原样接受
    auto gid2 = client.add_uri(
        {"seed://clamp2"},
        json{{"seed-ratio", 1.5}, {"seed-time", 45}, {"dir", "/tmp"}},
        &err);
    ASSERT_TRUE(gid2) << (err.message.empty() ? "addUri failed" : err.message);
    const auto id2 = falcon::daemon::rpc::task_id_from_gid(*gid2);
    ASSERT_TRUE(id2);
    auto task2 = engine_.get_task(*id2);
    ASSERT_TRUE(task2);
    EXPECT_DOUBLE_EQ(task2->options().seed_ratio, 1.5);
    EXPECT_EQ(task2->options().seed_time_minutes, 45u);
}

TEST_F(DownloadBackendTest, InProcessBackendBasics) {
    auto backend = make_inprocess_backend();
    ASSERT_TRUE(backend);

    // 默认引擎未链接内置协议时（stub），至少行为要一致：拒绝或受理
    auto tasks = backend->fetch_tasks();
    EXPECT_TRUE(tasks.empty());

    auto stats = backend->fetch_stats();
    ASSERT_TRUE(stats.has_value());
    EXPECT_EQ(stats->total_tasks(), 0u);
    EXPECT_EQ(stats->download_speed, 0u);
}

} // namespace
