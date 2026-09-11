// JSON-RPC 客户端 × 真实 JsonRpcServer 回环集成测试。
//
// 客户端（libcurl 同步 POST）对 daemon 服务端全链路：
// - token 前置与鉴权失败路径
// - 便捷封装到引擎操作的往返（addUri/pause/unpause/changePriority/…）
// - 传输层失败（连接拒绝）的错误码映射

#include "rpc/json_rpc_client.hpp"
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

using json = nlohmann::json;

// download() 阻塞到 release/取消或任务进入 Paused/终态
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
        running_.fetch_add(1);
        while (!task->is_finished() && task->status() != falcon::TaskStatus::Paused) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        running_.fetch_sub(1);
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

private:
    std::atomic<int> running_{0};
};

class JsonRpcClientTest : public ::testing::Test {
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

        falcon::daemon::rpc::JsonRpcClientConfig client_cfg;
        client_cfg.url = "http://127.0.0.1:" + std::to_string(server_->port()) + "/jsonrpc";
        client_cfg.secret = cfg_.secret;
        client_cfg.timeout_seconds = 5;
        client_ = std::make_unique<falcon::daemon::rpc::JsonRpcClient>(client_cfg);
    }

    void TearDown() override {
        // 解除所有阻塞中的 download()，再收掉服务器
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

    // 添加一个阻塞中的下载任务，返回 gid（等待其真正进入活动态）
    std::string add_active_task(const std::string& name) {
        auto gid = client_->add_uri({"test://" + name},
                                    json{{"dir", "/tmp"}});
        EXPECT_TRUE(gid.has_value());
        if (!gid) return {};

        const auto tid = static_cast<falcon::TaskId>(
            std::stoull(*gid, nullptr, 16));
        auto task = engine_.get_task(tid);
        EXPECT_TRUE(task);
        for (int i = 0; i < 2500 && task && !task->is_active(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (task) EXPECT_TRUE(task->is_active());
        return *gid;
    }

    falcon::DownloadEngine engine_;
    falcon::daemon::rpc::JsonRpcServerConfig cfg_;
    std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> server_;
    std::unique_ptr<falcon::daemon::rpc::JsonRpcClient> client_;
};

TEST_F(JsonRpcClientTest, GetVersionAndUnknownGidError) {
    falcon::daemon::rpc::JsonRpcError err;
    auto version = client_->call("aria2.getVersion", json::array(), &err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(version.has_value());
    EXPECT_TRUE(version->is_object());

    client_->tell_status("0000000000000000", &err);
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, 2); // gid 不存在 → aria2 业务错误透传
}

TEST_F(JsonRpcClientTest, AddUriAndTellStatusRoundTrip) {
    const std::string gid = add_active_task("roundtrip");

    falcon::daemon::rpc::JsonRpcError err;
    auto status = client_->tell_status(gid, &err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(status->value("gid", ""), gid);
    EXPECT_EQ(status->value("status", ""), "active");
}

TEST_F(JsonRpcClientTest, TellActiveListsRunningTask) {
    add_active_task("active-list");

    falcon::daemon::rpc::JsonRpcError err;
    auto active = client_->tell_active(&err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(active.has_value());
    ASSERT_TRUE(active->is_array());
    EXPECT_EQ(active->size(), 1u);
}

TEST_F(JsonRpcClientTest, PauseUnpauseRoundTrip) {
    const std::string gid = add_active_task("pause-me");

    falcon::daemon::rpc::JsonRpcError err;
    auto paused = client_->pause(gid, &err);
    ASSERT_FALSE(err.is_error()) << err.message;
    EXPECT_EQ(paused, gid);

    auto resumed = client_->unpause(gid, &err);
    ASSERT_FALSE(err.is_error()) << err.message;
    EXPECT_EQ(resumed, gid);
}

TEST_F(JsonRpcClientTest, ChangePriorityRoundTrip) {
    const std::string gid = add_active_task("prio");

    falcon::daemon::rpc::JsonRpcError err;
    auto changed = client_->change_priority(gid, 2 /* High */, &err);
    ASSERT_FALSE(err.is_error()) << err.message;
    EXPECT_EQ(changed, gid);

    // 引擎侧验证优先级生效
    const auto id = static_cast<falcon::TaskId>(std::stoull(gid, nullptr, 16));
    auto task = engine_.get_task(id);
    ASSERT_TRUE(task);
    EXPECT_EQ(task->get_priority(), falcon::TaskPriority::High);

    // 越界值报业务错误
    client_->change_priority(gid, 5, &err);
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, 1);
}

TEST_F(JsonRpcClientTest, ChangeGlobalOptionRoundTrip) {
    falcon::daemon::rpc::JsonRpcError err;
    ASSERT_TRUE(client_->change_global_option(
        json{{"max-overall-download-limit", "4096"}}, &err))
        << err.message;
    EXPECT_EQ(engine_.get_global_speed_limit(), falcon::BytesPerSecond{4096});

    auto options = client_->get_global_option(&err);
    ASSERT_FALSE(err.is_error()) << err.message;
    EXPECT_EQ(options->value("max-overall-download-limit", ""), "4096");
}

TEST_F(JsonRpcClientTest, RemoveAndPurgeRoundTrip) {
    const std::string gid = add_active_task("to-remove");

    falcon::daemon::rpc::JsonRpcError err;
    EXPECT_TRUE(client_->remove(gid, &err)) << err.message;
    EXPECT_TRUE(client_->purge_download_result(&err)) << err.message;

    auto status = client_->tell_status(gid, &err);
    EXPECT_TRUE(err.is_error()); // 已被清理
}

TEST_F(JsonRpcClientTest, TransportErrorOnConnectionRefused) {
    falcon::daemon::rpc::JsonRpcClientConfig cfg;
    cfg.url = "http://127.0.0.1:1/jsonrpc"; // 无人监听
    cfg.timeout_seconds = 3;
    falcon::daemon::rpc::JsonRpcClient refused(cfg);

    falcon::daemon::rpc::JsonRpcError err;
    auto result = refused.call("aria2.getVersion", json::array(), &err);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, -32000); // 传输层失败
}

TEST_F(JsonRpcClientTest, UnauthorizedWithoutToken) {
    falcon::daemon::rpc::JsonRpcClientConfig cfg;
    cfg.url = "http://127.0.0.1:" + std::to_string(server_->port()) + "/jsonrpc";
    cfg.secret.clear(); // 不带 token
    cfg.timeout_seconds = 5;
    falcon::daemon::rpc::JsonRpcClient anonymous(cfg);

    falcon::daemon::rpc::JsonRpcError err;
    auto result = anonymous.call("aria2.getVersion", json::array(), &err);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(err.code, -32001);
}

TEST_F(JsonRpcClientTest, ShutdownTriggersHandler) {
    std::atomic<bool> shutdown_requested{false};
    server_->set_shutdown_handler([&shutdown_requested] { shutdown_requested.store(true); });

    falcon::daemon::rpc::JsonRpcError err;
    EXPECT_TRUE(client_->shutdown(&err)) << err.message;
    EXPECT_TRUE(shutdown_requested.load());
}

} // namespace
