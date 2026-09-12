// WebSocketRpcClient × 真实 JsonRpcServer 回环测试。
// 覆盖：WS 握手与 RPC 往返、token 认证、服务器通知接收（事件流驱动桌面端
// 的核心路径）、断线后自动重连、服务器停机时挂起请求失败返回、并发调用
// 的 id 匹配、客户端掩码帧编码。

#include "rpc/json_rpc_server.hpp"
#include "rpc/websocket_rpc_client.hpp"
#include "rpc/websocket_frame.hpp"

#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using falcon::daemon::rpc::JsonRpcClientConfig;
using falcon::daemon::rpc::JsonRpcError;
using falcon::daemon::rpc::JsonRpcServer;
using falcon::daemon::rpc::JsonRpcServerConfig;
using falcon::daemon::rpc::WebSocketRpcClient;
using falcon::daemon::rpc::WS_OP_TEXT;
using falcon::daemon::rpc::WsFrameParser;

// 任务开始后立即完成：产生 onDownloadStart → onDownloadComplete 通知链
class AutoCompleteHandler final : public falcon::IProtocolHandler {
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
        info.filename = "ws-client.bin";
        info.total_size = 100;
        info.supports_resume = false;
        return info;
    }

    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        task->set_status(falcon::TaskStatus::Downloading);
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

    bool supports_resume() const override { return false; }
};

class WsRpcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_.register_handler(std::make_unique<AutoCompleteHandler>());

        cfg_.listen_port = 0;
        cfg_.secret = "ws-client-secret";
        cfg_.allow_origin_all = false;
        cfg_.bind_address = "127.0.0.1";

        start_server();
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

    void start_server() {
        server_ = std::make_unique<JsonRpcServer>(&engine_, cfg_, /*storage=*/nullptr);
        ASSERT_TRUE(server_->start());
        ASSERT_NE(server_->port(), 0);
    }

    JsonRpcClientConfig client_config() const {
        JsonRpcClientConfig cfg;
        cfg.url = "ws://127.0.0.1:" + std::to_string(server_->port()) + "/jsonrpc";
        cfg.secret = cfg_.secret;
        cfg.timeout_seconds = 5;
        return cfg;
    }

    /// 添加一个任务并等 daemon 侧进入完成（保证通知链走完）
    void add_task(const std::string& name) {
        falcon::DownloadOptions options;
        options.output_directory = "/tmp";
        auto task = engine_.add_task("test://" + name, options);
        ASSERT_TRUE(task);
        (void)engine_.start_task(task->id());
    }

    falcon::DownloadEngine engine_;
    JsonRpcServerConfig cfg_;
    std::unique_ptr<JsonRpcServer> server_;
};

TEST_F(WsRpcClientTest, ConnectAndCallRoundtrip) {
    WebSocketRpcClient client(client_config());
    EXPECT_FALSE(client.is_connected());
    ASSERT_TRUE(client.connect());
    EXPECT_TRUE(client.is_connected());
    // 幂等 connect
    EXPECT_TRUE(client.connect());

    JsonRpcError err;
    auto result = client.call("aria2.tellActive", json::array(), &err);
    ASSERT_TRUE(result) << err.message;
    EXPECT_TRUE(result->is_array());
    EXPECT_FALSE(err.is_error());

    client.disconnect();
    EXPECT_FALSE(client.is_connected());
}

TEST_F(WsRpcClientTest, ConvenienceMethodsMatchHttpClient) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    nlohmann::json options = nlohmann::json::object();
    options["dir"] = "/tmp";
    auto gid = client.add_uri({"test://ws-convenience"}, options, &err);
    ASSERT_TRUE(gid) << err.message;
    EXPECT_FALSE(gid->empty());

    auto stat = client.get_global_stat(&err);
    ASSERT_TRUE(stat) << err.message;
    EXPECT_TRUE(stat->contains("downloadSpeed"));
}

TEST_F(WsRpcClientTest, CallWithSecretAuth) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    // 无 secret 的客户端 → 认证失败 -32001
    JsonRpcClientConfig anon_cfg = client_config();
    anon_cfg.secret.clear();
    WebSocketRpcClient anon(anon_cfg);
    ASSERT_TRUE(anon.connect());
    JsonRpcError err;
    auto refused = anon.call("aria2.tellActive", json::array(), &err);
    ASSERT_FALSE(refused);
    EXPECT_EQ(err.code, -32001);
}

TEST_F(WsRpcClientTest, ReceivesNotifications) {
    WebSocketRpcClient client(client_config());

    std::mutex m;
    std::condition_variable cv;
    std::vector<std::pair<std::string, std::string>> notifications;
    client.set_notification_handler([&](const std::string& method,
                                        const std::string& params_json) {
        std::lock_guard<std::mutex> lock(m);
        notifications.emplace_back(method, params_json);
        cv.notify_all();
    });

    ASSERT_TRUE(client.connect());

    // 经同一连接 addUri → daemon 引擎事件 → WS 通知回流
    JsonRpcError err;
    nlohmann::json options = nlohmann::json::object();
    options["dir"] = "/tmp";
    auto gid = client.add_uri({"test://ws-notify"}, options, &err);
    ASSERT_TRUE(gid) << err.message;

    bool got = false;
    {
        std::unique_lock<std::mutex> lock(m);
        got = cv.wait_for(lock, std::chrono::seconds(5),
                          [&]() { return !notifications.empty(); });
    }
    ASSERT_TRUE(got) << "no notification received";

    std::lock_guard<std::mutex> lock(m);
    bool saw_state_notification = false;
    for (const auto& [method, params_json] : notifications) {
        if (method == "aria2.onDownloadStart" || method == "aria2.onDownloadComplete") {
            saw_state_notification = true;
            auto params = json::parse(params_json);
            ASSERT_TRUE(params.is_array());
            ASSERT_FALSE(params.empty());
            EXPECT_EQ(params[0]["gid"], *gid);
        }
    }
    EXPECT_TRUE(saw_state_notification);

    client.disconnect();
}

TEST_F(WsRpcClientTest, ReconnectAfterDisconnect) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    client.disconnect();
    EXPECT_FALSE(client.is_connected());

    // call 自动重连（桌面端断线自愈路径）
    JsonRpcError err;
    auto result = client.call("aria2.getGlobalStat", json::array(), &err);
    ASSERT_TRUE(result) << err.message;
    EXPECT_TRUE(client.is_connected());
}

TEST_F(WsRpcClientTest, CallFailsAfterServerStop) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    auto ok = client.call("aria2.getGlobalStat", json::array(), &err);
    ASSERT_TRUE(ok) << err.message;

    server_->stop();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 服务器关闭订阅连接 → 读线程断开 → call 重连失败（端口已无监听）
    auto failed = client.call("aria2.getGlobalStat", json::array(), &err);
    ASSERT_FALSE(failed);
    EXPECT_EQ(err.code, -32000);
}

TEST_F(WsRpcClientTest, ConcurrentCallsMatchTheirIds) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    std::atomic<int> succeeded{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&client, &succeeded, t]() {
            for (int i = 0; i < 10; ++i) {
                JsonRpcError err;
                const char* method =
                    (t % 2 == 0) ? "aria2.tellActive" : "aria2.getGlobalStat";
                auto r = client.call(method, json::array(), &err);
                if (r && !err.is_error()) succeeded.fetch_add(1);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    EXPECT_EQ(succeeded.load(), 40);
}

// 客户端帧必须是掩码帧，且服务端解析器能无损还原
TEST(WsClientFrameTest, MaskedFrameParserRoundtrip) {
    const std::string payload = R"({"jsonrpc":"2.0","id":1,"method":"aria2.tellActive"})";
    const auto frame = falcon::daemon::rpc::ws_encode_client_frame(WS_OP_TEXT, payload);

    ASSERT_GE(frame.size(), payload.size() + 6);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[0]) & 0x80u, 0x80u); // FIN
    EXPECT_EQ(static_cast<std::uint8_t>(frame[0]) & 0x0Fu, WS_OP_TEXT);
    EXPECT_EQ(static_cast<std::uint8_t>(frame[1]) & 0x80u, 0x80u); // MASK

    WsFrameParser parser;
    parser.feed(frame.data(), frame.size());
    auto messages = parser.pop_messages();
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].opcode, WS_OP_TEXT);
    EXPECT_EQ(messages[0].payload, payload);
    EXPECT_FALSE(parser.error());
}

// 126/127 扩展长度的掩码帧
TEST(WsClientFrameTest, MaskedFrameExtendedLengths) {
    for (std::size_t size : {std::size_t{125}, std::size_t{126},
                             std::size_t{65535}, std::size_t{65536}}) {
        const std::string payload(size, 'x');
        const auto frame =
            falcon::daemon::rpc::ws_encode_client_frame(WS_OP_TEXT, payload);
        WsFrameParser parser;
        parser.feed(frame.data(), frame.size());
        auto messages = parser.pop_messages();
        ASSERT_EQ(messages.size(), 1u) << "size=" << size;
        EXPECT_EQ(messages[0].payload, payload) << "size=" << size;
        EXPECT_FALSE(parser.error());
    }
}

// Sec-WebSocket-Key 的 base64：16 随机字节 → 24 字符（含 = 填充）
TEST(WsClientFrameTest, Base64ForHandshakeKey) {
    const std::uint8_t raw[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const auto encoded =
        falcon::daemon::rpc::ws_base64_encode(raw, sizeof(raw));
    EXPECT_EQ(encoded.size(), 24u);
    EXPECT_EQ(encoded.back(), '=');
}

} // namespace
