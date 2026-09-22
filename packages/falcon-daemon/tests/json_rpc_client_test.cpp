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

#include <falcon/detail/injection.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

// 脚本化原始 HTTP 服务器（见文件尾）的跨平台 socket 头；
// falcon_daemon_rpc_client_tests 在 Windows CI 同样编译
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

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

//==============================================================================
// 脚本化原始 HTTP 服务器（覆盖率批次 T）：单一剧本应答，驱动客户端对
// 非 200 / 畸形 JSON 应答形状的防御路径（平台辅助照抄
// websocket_rpc_client_test.cpp 的 raw_* 家族）
//==============================================================================

#ifdef _WIN32
using raw_recv_send_size_t = int;
static int raw_socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
static int raw_socket_shutdown(int fd) {
    return ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
}
static void raw_ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using raw_recv_send_size_t = ssize_t;
static int raw_socket_close(int fd) { return ::close(fd); }
static int raw_socket_shutdown(int fd) { return ::shutdown(fd, SHUT_RDWR); }
static void raw_ensure_winsock_started() {}
#endif

static bool raw_send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
#ifdef _WIN32
        const int chunk_len = static_cast<int>(data.size() - off);
#else
        const std::size_t chunk_len = data.size() - off;
#endif
        raw_recv_send_size_t n = ::send(fd, data.data() + off, chunk_len, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 每连接处理单个请求：读满请求头 + 请求体 → 记录 last_body() →
// 返回构造注入的原始剧本应答。应答后半关闭写端并以 SO_RCVTIMEO 排空
// 读端再 close（防未读完的 POST 体在 close 时触发 RST 吞掉刚写出的
// 应答，mock_http_server.hpp 同款防御）；停机先 shutdown(listen_fd)
// 唤醒阻塞 accept（Linux close 不唤醒，RawWsServer 同款）
class ScriptHttpServer {
public:
    explicit ScriptHttpServer(std::string response)
        : response_(std::move(response)) {
        raw_ensure_winsock_started();
        start();
    }
    ~ScriptHttpServer() { stop(); }
    ScriptHttpServer(const ScriptHttpServer&) = delete;
    ScriptHttpServer& operator=(const ScriptHttpServer&) = delete;

    int port() const { return port_; }
    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/jsonrpc";
    }
    std::string last_body() const {
        std::lock_guard<std::mutex> lock(body_mutex_);
        return last_body_;
    }

private:
    void start() {
        listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (listen_fd_ < 0) return;
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) < 0 ||
            ::listen(listen_fd_, 16) < 0) {
            raw_socket_close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound),
                          &len) == 0) {
            port_ = static_cast<int>(ntohs(bound.sin_port));
        }
        accept_thread_ = std::thread(&ScriptHttpServer::accept_loop, this);
    }

    void accept_loop() {
        while (!stopping_.load()) {
            const int fd =
                static_cast<int>(::accept(listen_fd_, nullptr, nullptr));
            if (fd < 0) {
                if (stopping_.load()) break;
                continue;
            }
            std::thread(&ScriptHttpServer::handle_conn, this, fd).detach();
        }
    }

    void handle_conn(int fd) {
        std::string buf;
        // 读满请求头（有界，畸形输入不悬挂）
        while (buf.find("\r\n\r\n") == std::string::npos &&
               buf.size() < 16 * 1024) {
            char tmp[2048];
            const auto n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
        }
        const std::size_t header_end = buf.find("\r\n\r\n");
        std::size_t content_length = 0;
        if (header_end != std::string::npos) {
            // 大小写不敏感解析 Content-Length（libcurl 发 "Content-Length:"）
            std::size_t pos = 0;
            while (pos < header_end) {
                std::size_t eol = buf.find("\r\n", pos);
                if (eol == std::string::npos || eol > header_end) {
                    eol = header_end;
                }
                if (eol - pos >= 15) {
                    std::string prefix = buf.substr(pos, 15);
                    std::transform(
                        prefix.begin(), prefix.end(), prefix.begin(),
                        [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                    if (prefix == "content-length:") {
                        // strtoull 不抛异常（detached 线程内异常逃逸即 terminate）
                        content_length = static_cast<std::size_t>(
                            std::strtoull(buf.substr(pos + 15, eol - pos - 15)
                                              .c_str(),
                                          nullptr, 10));
                    }
                }
                pos = eol + 2;
            }
            // 记录请求体（token 注入形状断言用）
            {
                const std::size_t need = header_end + 4 + content_length;
                while (buf.size() < need) {
                    char tmp[2048];
                    const auto n = ::recv(fd, tmp, sizeof(tmp), 0);
                    if (n <= 0) break;
                    buf.append(tmp, static_cast<std::size_t>(n));
                }
                std::lock_guard<std::mutex> lock(body_mutex_);
                last_body_ = buf.substr(header_end + 4);
            }
        }
        raw_send_all(fd, response_);
        // 半关闭写端 + 限时排空读端再 close
        ::shutdown(fd,
#ifdef _WIN32
                   SD_SEND
#else
                   SHUT_WR
#endif
        );
#ifdef _WIN32
        DWORD drain_timeout = 200;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&drain_timeout),
                     sizeof(drain_timeout));
#else
        timeval drain_timeout{0, 200 * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &drain_timeout,
                     sizeof(drain_timeout));
#endif
        char sink[512];
        while (::recv(fd, sink, sizeof(sink), 0) > 0) {
        }
        raw_socket_close(fd);
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        if (listen_fd_ >= 0) {
            raw_socket_shutdown(listen_fd_);
            raw_socket_close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    std::string response_;
    int listen_fd_{-1};
    int port_{0};
    std::atomic<bool> stopping_{false};
    std::thread accept_thread_;
    mutable std::mutex body_mutex_;
    std::string last_body_;
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

//==============================================================================
// 覆盖率批次 T：零调用便捷方法全簇 + 应答形状边界 + params 归一
//==============================================================================

// tell_waiting/tell_stopped/get_global_stat/save_session 此前全库零调用
// （真实服务器回环：getGlobalStat 回对象含 downloadSpeed/numActive/
// numWaiting；saveSession 服务端无条件 "OK"，storage=nullptr 也成立）
TEST_F(JsonRpcClientTest, ConvenienceQueryMethodsSuite) {
    falcon::daemon::rpc::JsonRpcError err;

    auto waiting = client_->tell_waiting(&err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(waiting.has_value());
    EXPECT_TRUE(waiting->is_array());

    auto stopped = client_->tell_stopped(&err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(stopped.has_value());
    EXPECT_TRUE(stopped->is_array());

    auto stat = client_->get_global_stat(&err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(stat.has_value());
    EXPECT_TRUE(stat->is_object());
    EXPECT_TRUE(stat->contains("downloadSpeed"));
    EXPECT_TRUE(stat->contains("numActive"));
    EXPECT_TRUE(stat->contains("numWaiting"));

    EXPECT_TRUE(client_->save_session(&err)) << err.message;
}

// removeDownloadResult：终态任务移除 OK；合法 16 字符 gid（=TaskId 255）
// 无记录 → 业务错误 code 2（"合法 gid 无任务"变体，区别于长度拒绝路径）
TEST_F(JsonRpcClientTest, RemoveDownloadResultHappyPathAndMissingGid) {
    const std::string gid = add_active_task("rm-result");

    falcon::daemon::rpc::JsonRpcError err;
    EXPECT_TRUE(client_->remove(gid, &err)) << err.message;

    // 等任务真正进入终态（活动任务移除被拒 code 1）
    const auto tid = static_cast<falcon::TaskId>(std::stoull(gid, nullptr, 16));
    for (int i = 0; i < 2500; ++i) {
        auto task = engine_.get_task(tid);
        if (task && task->is_finished()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(client_->remove_download_result(gid, &err)) << err.message;

    EXPECT_FALSE(client_->remove_download_result("00000000000000ff", &err));
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, 2);
}

// 脚本服务器便捷客户端（无 secret——脚本服务器不检查认证）
namespace {
falcon::daemon::rpc::JsonRpcClient make_scripted_client(
    const ScriptHttpServer& server) {
    falcon::daemon::rpc::JsonRpcClientConfig cfg;
    cfg.url = server.url();
    cfg.timeout_seconds = 5;
    return falcon::daemon::rpc::JsonRpcClient(cfg);
}
} // namespace

// HTTP 状态码 != 200：curl 无 FAILONERROR 时 500 应答照常读回，
// 客户端报传输层错误 -32000 且消息携带状态码
TEST_F(JsonRpcClientTest, CallRejectsNon200Response) {
    ScriptHttpServer server("HTTP/1.1 500 Internal Server Error\r\n"
                            "Content-Length: 0\r\n"
                            "\r\n");
    ASSERT_NE(server.port(), 0);
    auto scripted = make_scripted_client(server);

    falcon::daemon::rpc::JsonRpcError err;
    auto result = scripted.call("aria2.getVersion", json::array(), &err);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, -32000);
    EXPECT_NE(err.message.find("HTTP 500"), std::string::npos);
}

// 200 但应答体形状畸形：数组 / 既无 result 又无 error 的对象
TEST_F(JsonRpcClientTest, CallRejectsMalformedResponseShapes) {
    // JSON 数组不是对象
    {
        ScriptHttpServer server("HTTP/1.1 200 OK\r\n"
                                "Content-Length: 7\r\n"
                                "\r\n"
                                "[1,2,3]");
        ASSERT_NE(server.port(), 0);
        auto scripted = make_scripted_client(server);

        falcon::daemon::rpc::JsonRpcError err;
        auto result = scripted.call("aria2.getVersion", json::array(), &err);
        EXPECT_FALSE(result.has_value());
        EXPECT_TRUE(err.is_error());
        EXPECT_EQ(err.code, -32600);
        EXPECT_NE(err.message.find("Response is not a JSON object"),
                  std::string::npos);
    }
    // 对象无 result 无 error
    {
        ScriptHttpServer server("HTTP/1.1 200 OK\r\n"
                                "Content-Length: 26\r\n"
                                "\r\n"
                                "{\"jsonrpc\":\"2.0\",\"id\":\"1\"}");
        ASSERT_NE(server.port(), 0);
        auto scripted = make_scripted_client(server);

        falcon::daemon::rpc::JsonRpcError err;
        auto result = scripted.call("aria2.getVersion", json::array(), &err);
        EXPECT_FALSE(result.has_value());
        EXPECT_TRUE(err.is_error());
        EXPECT_EQ(err.code, -32600);
        EXPECT_NE(err.message.find("Response has neither result nor error"),
                  std::string::npos);
    }
}

// 解包防御：as_gid 收到非字符串 result / expect_ok 收到非 "OK"
TEST_F(JsonRpcClientTest, UnwrapGuardsRejectWrongResultTypes) {
    // pause 收 {"result":123} → "Expected gid string in response"
    {
        ScriptHttpServer server("HTTP/1.1 200 OK\r\n"
                                "Content-Length: 14\r\n"
                                "\r\n"
                                "{\"result\":123}");
        ASSERT_NE(server.port(), 0);
        auto scripted = make_scripted_client(server);

        falcon::daemon::rpc::JsonRpcError err;
        auto gid = scripted.pause("0000000000000001", &err);
        EXPECT_FALSE(gid.has_value());
        EXPECT_TRUE(err.is_error());
        EXPECT_EQ(err.code, -32600);
        EXPECT_NE(err.message.find("Expected gid string in response"),
                  std::string::npos);
    }
    // save_session 收 {"result":"NG"} → "Expected \"OK\" in response"
    {
        ScriptHttpServer server("HTTP/1.1 200 OK\r\n"
                                "Content-Length: 15\r\n"
                                "\r\n"
                                "{\"result\":\"NG\"}");
        ASSERT_NE(server.port(), 0);
        auto scripted = make_scripted_client(server);

        falcon::daemon::rpc::JsonRpcError err;
        EXPECT_FALSE(scripted.save_session(&err));
        EXPECT_TRUE(err.is_error());
        EXPECT_EQ(err.code, -32600);
        EXPECT_NE(err.message.find("Expected \"OK\" in response"),
                  std::string::npos);
    }
}

// call 的非数组 params 归一（56-58）：服务端顶层 dispatch 允许 object
// params，故归一证明走 secret 注入路径——带 secret 下传 object 调
// getVersion 成功即证明归一发生（归一则 object→[]→insert token→
// ["token:test-secret"]→认证通过；不归一则 object 上 insert 产生
// 非合法 token 形状 → -32001，两种结果可区分）
TEST_F(JsonRpcClientTest, CallNormalizesObjectParamsWithSecret) {
    falcon::daemon::rpc::JsonRpcError err;
    auto version = client_->call("aria2.getVersion", json{{"a", 1}}, &err);
    ASSERT_FALSE(err.is_error()) << err.message;
    ASSERT_TRUE(version.has_value());
    EXPECT_TRUE(version->is_object());
}

// HTTP 200 但应答体不是合法 JSON：json::parse 抛出 → -32700
//（区别于既有 -32600 畸形形状用例——那些 body 是合法 JSON）
TEST_F(JsonRpcClientTest, CallRejectsNonJsonResponseBody) {
    ScriptHttpServer server("HTTP/1.1 200 OK\r\n"
                            "Content-Length: 9\r\n"
                            "\r\n"
                            "<not-json>");
    ASSERT_NE(server.port(), 0);
    auto scripted = make_scripted_client(server);

    falcon::daemon::rpc::JsonRpcError err;
    auto result = scripted.call("aria2.getVersion", json::array(), &err);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, -32700);
    EXPECT_NE(err.message.find("Invalid JSON response"), std::string::npos);
}

#if defined(FALCON_FAILURE_INJECTION)

// curl 句柄创建失败（注入短路）：传输层错误 -32000，不发起任何请求
TEST_F(JsonRpcClientTest, CallFailsWhenCurlInitInjected) {
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::CurlEasyInit);
    falcon::daemon::rpc::JsonRpcError err;
    auto result = client_->call("aria2.getVersion", json::array(), &err);
    EXPECT_FALSE(result.has_value());
    EXPECT_TRUE(err.is_error());
    EXPECT_EQ(err.code, -32000);
    EXPECT_EQ(err.message, "curl_easy_init failed");
}

#endif  // FALCON_FAILURE_INJECTION

} // namespace
