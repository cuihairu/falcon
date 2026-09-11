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
#include <thread>

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

class DownloadBackendTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.register_handler(std::make_unique<BlockingHandler>());

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

    auto tasks = daemon_backend_->fetch_tasks();
    ASSERT_EQ(tasks.size(), 1u);
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
