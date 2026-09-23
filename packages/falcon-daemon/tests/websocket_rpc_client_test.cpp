// WebSocketRpcClient × 真实 JsonRpcServer 回环测试。
// 覆盖：WS 握手与 RPC 往返、token 认证、服务器通知接收（事件流驱动桌面端
// 的核心路径）、断线后自动重连、服务器停机时挂起请求失败返回、并发调用
// 的 id 匹配、客户端掩码帧编码。

#include "rpc/json_rpc_server.hpp"
#include "rpc/websocket_rpc_client.hpp"
#include "rpc/websocket_frame.hpp"

#include <falcon/detail/injection.hpp>
#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
#include <unistd.h>
#endif

namespace {

using json = nlohmann::json;
using falcon::daemon::rpc::JsonRpcClientConfig;
using falcon::daemon::rpc::JsonRpcError;
using falcon::daemon::rpc::JsonRpcServer;
using falcon::daemon::rpc::JsonRpcServerConfig;
using falcon::daemon::rpc::WebSocketRpcClient;
using falcon::daemon::rpc::WS_OP_BINARY;
using falcon::daemon::rpc::WS_OP_CLOSE;
using falcon::daemon::rpc::WS_OP_PING;
using falcon::daemon::rpc::WS_OP_PONG;
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

//==============================================================================
// 可编程原始 WebSocket 服务器：客户端协议栈边界测试基建
//
// 与 JsonRpcServer 不同，握手应答与帧级行为完全由测试控制——覆盖握手
// 容错（半截头/非 101/错 Accept）、服务器控制帧（ping/close/binary）、
// 异常响应形状与请求中途断连等客户端单侧路径。
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
        const auto chunk_len = static_cast<
#ifdef _WIN32
            int
#else
            std::size_t
#endif
            >(data.size() - off);
        raw_recv_send_size_t n = ::send(fd, data.data() + off, chunk_len, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

static std::string raw_trim(std::string s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

/// 从请求头块取头值（客户端握心头名为固定拼写，直接比较）
static std::string raw_header_value(const std::string& headers, const char* name) {
    std::size_t begin = 0;
    while (begin < headers.size()) {
        const auto end = headers.find("\r\n", begin);
        const std::size_t stop = end == std::string::npos ? headers.size() : end;
        const std::string line = headers.substr(begin, stop - begin);
        const auto colon = line.find(':');
        if (colon != std::string::npos &&
            raw_trim(line.substr(0, colon)) == name) {
            return raw_trim(line.substr(colon + 1));
        }
        if (end == std::string::npos) break;
        begin = end + 2;
    }
    return "";
}

class RawWsServer {
public:
    enum class Handshake {
        Ok,               // 正确 101 + Sec-WebSocket-Accept
        TruncatedHeader,  // 半截响应头后断连（客户端握手 EOF 分支）
        Not101,           // 普通 404 应答（客户端状态行校验分支）
        BadAccept,        // 101 但 Accept 值错误（客户端回显校验分支）
    };

    struct RecvdMsg {
        std::uint8_t opcode;
        std::string payload;
    };

    explicit RawWsServer(Handshake hs = Handshake::Ok) : handshake_(hs) {
        raw_ensure_winsock_started();
        start();
    }
    ~RawWsServer() { stop(); }
    RawWsServer(const RawWsServer&) = delete;
    RawWsServer& operator=(const RawWsServer&) = delete;

    std::string url() const {
        return "ws://127.0.0.1:" + std::to_string(port_) + "/jsonrpc";
    }

    /// 握手完成后的会话钩子（会话线程上下文）：注入服务器帧等
    std::function<void(RawWsServer&, int fd)> on_connected;
    /// text 请求响应脚本：返回应答 payload；空串 = 不应答
    std::function<std::string(const json&)> on_request;

    std::string request_line() const {
        std::lock_guard<std::mutex> lock(line_mutex_);
        return request_line_;
    }

    std::vector<RecvdMsg> messages() const {
        std::lock_guard<std::mutex> lock(msg_mutex_);
        return msgs_;
    }

    bool wait_for_msg(std::uint8_t opcode, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(msg_mutex_);
        return msg_cv_.wait_for(lock, timeout, [&]() {
            return std::any_of(msgs_.begin(), msgs_.end(),
                               [&](const RecvdMsg& m) { return m.opcode == opcode; });
        });
    }

    bool wait_for_request(std::chrono::milliseconds timeout) {
        return wait_for_msg(WS_OP_TEXT, timeout);
    }

    void send_server_frame(int fd, std::uint8_t opcode, const std::string& payload) {
        (void)raw_send_all(fd, falcon::daemon::rpc::ws_encode_frame(opcode, payload));
    }

    /// shutdown 全部活动会话（客户端读线程将观察到断连）
    void close_all() {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        for (const int fd : conns_) raw_socket_shutdown(fd);
    }

    void stop() {
        if (stopping_.exchange(true)) return;
        close_all();
        if (listen_fd_ >= 0) {
            raw_socket_shutdown(listen_fd_);
            raw_socket_close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        // 会话线程是 detach 的:close_all 只唤醒其 recv,线程此后还要走
        // retire_conn 访问成员。等全部会话线程退出(retire_conn 后的
        // 计数递减是其最后一次 this 访问),栈上实例析构后才无人触碰
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (active_sessions_.load() > 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
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
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(listen_fd_, 16) < 0) {
            raw_socket_close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        sockaddr_in actual{};
        socklen_t len = sizeof(actual);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&actual), &len) == 0) {
            port_ = ntohs(actual.sin_port);
        }
        accept_thread_ = std::thread(&RawWsServer::accept_loop, this);
    }

    void accept_loop() {
        while (!stopping_.load()) {
            const int fd = static_cast<int>(::accept(listen_fd_, nullptr, nullptr));
            if (fd < 0) {
                if (stopping_.load()) break;
                continue;
            }
            // spawn 时预登记计数(spawn 与登记之间无窗口;accept 线程被
            // join 后不再有新会话线程),线程体收尾注销——注销是最后一
            // 次 this 访问,计数归零即全部会话线程已离开成员
            active_sessions_.fetch_add(1, std::memory_order_seq_cst);
            const int session_fd = fd;
            std::thread([this, session_fd] {
                handle_conn(session_fd);
                active_sessions_.fetch_sub(1, std::memory_order_seq_cst);
            }).detach();
        }
    }

    void retire_conn(int fd) {
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conns_.erase(fd);
        }
        raw_socket_close(fd);
    }

    void handle_conn(int fd) {
        if (stopping_.load()) {
            raw_socket_close(fd);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            if (stopping_.load()) {
                raw_socket_close(fd);
                return;
            }
            conns_.insert(fd);
        }

        // 读握手请求头
        std::string buf;
        char tmp[1024];
        while (buf.find("\r\n\r\n") == std::string::npos && buf.size() < 16 * 1024) {
            raw_recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
        }
        {
            const auto line_end = buf.find("\r\n");
            std::lock_guard<std::mutex> lock(line_mutex_);
            request_line_ =
                line_end == std::string::npos ? buf : buf.substr(0, line_end);
        }

        switch (handshake_) {
            case Handshake::TruncatedHeader:
                (void)raw_send_all(
                    fd, "HTTP/1.1 101 Switching Protocols\r\nSec-WebSocket-A");
                retire_conn(fd);
                return;
            case Handshake::Not101:
                (void)raw_send_all(
                    fd,
                    "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n");
                retire_conn(fd);
                return;
            case Handshake::BadAccept:
                (void)raw_send_all(
                    fd,
                    "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                    "Connection: Upgrade\r\nSec-WebSocket-Accept: not-the-answer\r\n\r\n");
                retire_conn(fd);
                return;
            case Handshake::Ok:
                break;
        }

        std::string resp;
        resp += "HTTP/1.1 101 Switching Protocols\r\n";
        resp += "Upgrade: websocket\r\n";
        resp += "Connection: Upgrade\r\n";
        resp += "Sec-WebSocket-Accept: " +
                falcon::daemon::rpc::ws_compute_accept_key(
                    raw_header_value(buf, "Sec-WebSocket-Key")) +
                "\r\n\r\n";
        if (!raw_send_all(fd, resp)) {
            retire_conn(fd);
            return;
        }

        if (on_connected) on_connected(*this, fd);

        WsFrameParser parser;
        std::string rbuf(4096, '\0');
        bool closing = false;
        while (!closing && !stopping_.load()) {
            for (const auto& m : parser.pop_messages()) {
                {
                    std::lock_guard<std::mutex> lock(msg_mutex_);
                    msgs_.push_back(RecvdMsg{m.opcode, m.payload});
                }
                msg_cv_.notify_all();
                if (m.opcode == WS_OP_TEXT && on_request) {
                    std::string resp_payload;
                    try {
                        resp_payload = on_request(json::parse(m.payload));
                    } catch (const json::exception&) {
                    }
                    if (!resp_payload.empty()) {
                        (void)raw_send_all(
                            fd, falcon::daemon::rpc::ws_encode_frame(WS_OP_TEXT,
                                                                     resp_payload));
                    }
                } else if (m.opcode == WS_OP_CLOSE) {
                    closing = true;
                }
            }
            if (closing) break;
            raw_recv_send_size_t n = ::recv(fd, &rbuf[0], rbuf.size(), 0);
            if (n <= 0) break;
            parser.feed(rbuf.data(), static_cast<std::size_t>(n));
        }
        retire_conn(fd);
    }

    Handshake handshake_;
    int listen_fd_ = -1;
    std::atomic<std::uint16_t> port_{0};
    std::thread accept_thread_;
    std::atomic<bool> stopping_{false};
    std::atomic<int> active_sessions_{0};

    std::mutex conn_mutex_;
    std::set<int> conns_;

    mutable std::mutex line_mutex_;
    std::string request_line_;

    mutable std::mutex msg_mutex_;
    std::condition_variable msg_cv_;
    std::vector<RecvdMsg> msgs_;
};

static JsonRpcClientConfig raw_client_config(const std::string& url, long timeout = 5) {
    JsonRpcClientConfig cfg;
    cfg.url = url;
    cfg.timeout_seconds = timeout;
    return cfg;
}

// 轮询等待条件成立（客户端状态由内部线程翻转，无阻塞等待接口）
template <typename Pred>
static bool raw_wait_until(Pred done, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

// ---- 便捷方法全簇：14 个转发方法的 method/params 形状与响应解包 ----

TEST_F(WsRpcClientTest, ConvenienceMethodsFullSuite) {
    RawWsServer raw;
    raw.on_request = [](const json& req) {
        const std::string method = req.value("method", "");
        json resp = {{"jsonrpc", "2.0"}, {"id", req.value("id", json())}};
        if (method == "aria2.tellStatus") {
            resp["result"] = json{{"gid", "gid-1"}, {"status", "active"}};
        } else if (method == "aria2.tellActive" || method == "aria2.tellWaiting" ||
                   method == "aria2.tellStopped") {
            resp["result"] = json::array();
        } else if (method == "aria2.getGlobalStat") {
            resp["result"] = json{{"downloadSpeed", "0"}, {"numActive", "0"}};
        } else if (method == "aria2.changeGlobalOption" ||
                   method == "aria2.saveSession" ||
                   method == "aria2.purgeDownloadResult" ||
                   method == "aria2.removeDownloadResult" ||
                   method == "aria2.forceShutdown") {
            resp["result"] = "OK";
        } else {
            resp["result"] = "gid-1";  // addUri/pause/unpause/remove/changePriority
        }
        return resp.dump();
    };

    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /jsonrpc HTTP/1.1");

    JsonRpcError err;
    EXPECT_EQ(client.add_uri({"u1", "u2"}, json{{"dir", "/tmp"}}, &err).value_or(""),
              "gid-1");
    EXPECT_EQ(client.pause("g1", &err).value_or(""), "gid-1");
    EXPECT_EQ(client.unpause("g1", &err).value_or(""), "gid-1");
    EXPECT_EQ(client.remove("g1", &err).value_or(""), "gid-1");
    EXPECT_EQ(client.change_priority("g1", 2, &err).value_or(""), "gid-1");
    auto st = client.tell_status("g1", &err);
    ASSERT_TRUE(st) << err.message;
    EXPECT_EQ((*st)["gid"], "gid-1");
    EXPECT_TRUE(client.tell_active(&err)->empty());
    EXPECT_TRUE(client.tell_waiting(&err)->empty());
    EXPECT_TRUE(client.tell_stopped(&err)->empty());
    auto stat = client.get_global_stat(&err);
    ASSERT_TRUE(stat) << err.message;
    EXPECT_EQ((*stat)["downloadSpeed"], "0");
    EXPECT_TRUE(
        client.change_global_option(json{{"max-overall-download-limit", "0"}}, &err));
    EXPECT_TRUE(client.save_session(&err));
    EXPECT_TRUE(client.purge_download_result(&err));
    EXPECT_TRUE(client.remove_download_result("g9", &err));
    EXPECT_TRUE(client.shutdown(&err));
    EXPECT_FALSE(err.is_error());

    // 服务器侧请求形状断言：每个便捷方法的 params 归一（无 secret 时无 token）
    std::map<std::string, json> by_method;
    for (const auto& m : raw.messages()) {
        if (m.opcode != WS_OP_TEXT) continue;
        const json req = json::parse(m.payload);
        by_method[req["method"]] = req["params"];
    }
    ASSERT_EQ(by_method.size(), 15u);
    EXPECT_EQ(by_method["aria2.addUri"],
              json::array({json::array({"u1", "u2"}), json{{"dir", "/tmp"}}}));
    EXPECT_EQ(by_method["aria2.pause"], json::array({"g1"}));
    EXPECT_EQ(by_method["aria2.unpause"], json::array({"g1"}));
    EXPECT_EQ(by_method["aria2.remove"], json::array({"g1"}));
    EXPECT_EQ(by_method["aria2.changePriority"], json::array({"g1", 2}));
    EXPECT_EQ(by_method["aria2.tellStatus"], json::array({"g1"}));
    EXPECT_EQ(by_method["aria2.tellActive"], json::array());
    EXPECT_EQ(by_method["aria2.tellWaiting"], json::array({0, 10000}));
    EXPECT_EQ(by_method["aria2.tellStopped"], json::array({0, 10000}));
    EXPECT_EQ(by_method["aria2.getGlobalStat"], json::array());
    EXPECT_EQ(by_method["aria2.changeGlobalOption"],
              json::array({json{{"max-overall-download-limit", "0"}}}));
    EXPECT_EQ(by_method["aria2.saveSession"], json::array());
    EXPECT_EQ(by_method["aria2.purgeDownloadResult"], json::array());
    EXPECT_EQ(by_method["aria2.removeDownloadResult"], json::array({"g9"}));
    EXPECT_EQ(by_method["aria2.forceShutdown"], json::array());
}

// add_uri 的 result 非字符串 → -32600（as_gid 防御分支）
TEST(WsRpcClientEdge, AddUriRejectsNonStringGid) {
    RawWsServer raw;
    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result", 42}}
            .dump();
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    auto gid = client.add_uri({"test://x"}, json::object(), &err);
    EXPECT_FALSE(gid);
    EXPECT_EQ(err.code, -32600);
    EXPECT_NE(err.message.find("gid"), std::string::npos);
}

// expect_ok 的两类失败：result 非字符串 / 字符串但非 "OK"
TEST(WsRpcClientEdge, ExpectOkRejectsBadResults) {
    RawWsServer raw;
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", 1}}.dump();
    };
    JsonRpcError err;
    EXPECT_FALSE(client.save_session(&err));
    EXPECT_EQ(err.code, -32600);
    EXPECT_NE(err.message.find("OK"), std::string::npos);

    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"result", "NG"}}.dump();
    };
    err = JsonRpcError{};
    EXPECT_FALSE(client.change_global_option(json{{"k", "v"}}, &err));
    EXPECT_EQ(err.code, -32600);
}

// 非数组 params 归一为空数组（服务器回显 params 证明形状）
TEST(WsRpcClientEdge, CallNormalizesNonArrayParams) {
    RawWsServer raw;
    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result", req["params"]}}
            .dump();
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    auto result = client.call("system.echo", json{{"k", "v"}}, &err);
    ASSERT_TRUE(result) << err.message;
    EXPECT_EQ(*result, json::array());
    EXPECT_FALSE(err.is_error());

    for (const auto& m : raw.messages()) {
        if (m.opcode == WS_OP_TEXT) {
            EXPECT_EQ(json::parse(m.payload)["params"], json::array());
        }
    }
}

// 响应既无 result 也无 error → -32600
TEST(WsRpcClientEdge, CallReportsMissingResultAndError) {
    RawWsServer raw;
    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}}.dump();
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    auto result = client.call("aria2.tellActive", json::array(), &err);
    EXPECT_FALSE(result);
    EXPECT_EQ(err.code, -32600);
    EXPECT_NE(err.message.find("neither result nor error"), std::string::npos);
}

// 服务器收到请求但永不应答 → 超时 -32000（等待槽移除，迟到响应将被丢弃）
TEST(WsRpcClientEdge, CallTimesOutWithoutResponse) {
    RawWsServer raw;  // 未设 on_request：不应答
    WebSocketRpcClient client(raw_client_config(raw.url(), /*timeout=*/1));
    ASSERT_TRUE(client.connect());

    const auto t0 = std::chrono::steady_clock::now();
    JsonRpcError err;
    auto result = client.call("aria2.tellActive", json::array(), &err);
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    EXPECT_FALSE(result);
    EXPECT_EQ(err.code, -32000);
    EXPECT_EQ(err.message, "WebSocket request timed out");
    EXPECT_GE(elapsed, std::chrono::milliseconds(900));
}

// 请求在途时连接断开 → 挂起调用被 -32000 "connection closed" 唤醒
TEST(WsRpcClientEdge, CallFailsWhenConnectionDropsMidRequest) {
    RawWsServer raw;  // 不应答，收妥请求后由测试主动断连
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    std::optional<nlohmann::json> result;
    std::thread caller([&]() {
        result = client.call("aria2.getGlobalStat", json::array(), &err);
    });
    ASSERT_TRUE(raw.wait_for_request(std::chrono::seconds(5)));
    raw.close_all();
    caller.join();

    EXPECT_FALSE(result);
    EXPECT_EQ(err.code, -32000);
    EXPECT_EQ(err.message, "connection closed");
}

// 服务器 ping → 客户端回 pong（心跳保活路径）
TEST(WsRpcClientEdge, ServerPingGetsPong) {
    RawWsServer raw;
    raw.on_connected = [](RawWsServer& s, int fd) {
        s.send_server_frame(fd, WS_OP_PING, "hb");
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    ASSERT_TRUE(raw.wait_for_msg(WS_OP_PONG, std::chrono::seconds(5)));
    bool saw_pong = false;
    for (const auto& m : raw.messages()) {
        if (m.opcode == WS_OP_PONG) {
            saw_pong = true;
            EXPECT_EQ(m.payload, "hb");
        }
    }
    EXPECT_TRUE(saw_pong);
}

// 服务器 close → 客户端回应 close 帧并结束会话
TEST(WsRpcClientEdge, ServerCloseEndsSession) {
    RawWsServer raw;
    raw.on_connected = [](RawWsServer& s, int fd) {
        s.send_server_frame(fd, WS_OP_CLOSE, "");
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    // 客户端回应 close 帧
    EXPECT_TRUE(raw.wait_for_msg(WS_OP_CLOSE, std::chrono::seconds(5)));
    // 会话收尾：connected_ 复位
    EXPECT_TRUE(raw_wait_until([&]() { return !client.is_connected(); },
                               std::chrono::seconds(5)));
}

// 客户端忽略 pong / binary 帧（非 text 非控制应答），会话保持可用
TEST(WsRpcClientEdge, ClientIgnoresPongAndBinaryFrames) {
    RawWsServer raw;
    raw.on_connected = [](RawWsServer& s, int fd) {
        s.send_server_frame(fd, WS_OP_BINARY, "blob");
        s.send_server_frame(fd, WS_OP_PONG, "stale");
    };
    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result", json::array()}}
            .dump();
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    JsonRpcError err;
    auto result = client.call("aria2.tellActive", json::array(), &err);
    ASSERT_TRUE(result) << err.message;  // 忽略帧后连接仍可用
    EXPECT_FALSE(err.is_error());

    // 客户端不应答 pong、不转发 binary（给读线程留出处理窗口再断言）
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    for (const auto& m : raw.messages()) {
        EXPECT_NE(m.opcode, WS_OP_PONG);
        EXPECT_NE(m.opcode, WS_OP_BINARY);
    }
}

// ---- 握手容错簇 ----

// 响应头半截断连（无 \r\n\r\n 终结）→ 握手失败，不悬挂
TEST(WsRpcClientEdge, HandshakeFailsOnTruncatedHeader) {
    RawWsServer raw(RawWsServer::Handshake::TruncatedHeader);
    WebSocketRpcClient client(raw_client_config(raw.url(), /*timeout=*/2));
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

// 状态行非 101 → 握手失败
TEST(WsRpcClientEdge, HandshakeFailsOnNon101Status) {
    RawWsServer raw(RawWsServer::Handshake::Not101);
    WebSocketRpcClient client(raw_client_config(raw.url(), /*timeout=*/2));
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

// Sec-WebSocket-Accept 回显错误 → 握手失败（防跨协议握手混淆）
TEST(WsRpcClientEdge, HandshakeFailsOnBadAcceptKey) {
    RawWsServer raw(RawWsServer::Handshake::BadAccept);
    WebSocketRpcClient client(raw_client_config(raw.url(), /*timeout=*/2));
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

// 主机名不可解析（保留域 .invalid）→ connect 干净失败
TEST(WsRpcClientEdge, ConnectFailsOnUnresolvableHost) {
    WebSocketRpcClient client(
        raw_client_config("ws://nonexistent-falcon-test.invalid:12345/jsonrpc", 2));
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

// ---- URL 解析与运行期重定向 ----

// set_url 重定向端点：旧服务器下线后 call 仍成功 ⇒ 一定连到了新端点
TEST_F(WsRpcClientTest, SetUrlRedirectsToNewServer) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());
    JsonRpcError err;
    ASSERT_TRUE(client.call("aria2.getGlobalStat", json::array(), &err)) << err.message;

    JsonRpcServerConfig cfg2 = cfg_;
    cfg2.listen_port = 0;
    auto server2 = std::make_unique<JsonRpcServer>(&engine_, cfg2, /*storage=*/nullptr);
    ASSERT_TRUE(server2->start());
    ASSERT_NE(server2->port(), 0);
    ASSERT_NE(server2->port(), server_->port());

    client.set_url("ws://127.0.0.1:" + std::to_string(server2->port()) + "/jsonrpc");
    EXPECT_FALSE(client.is_connected());  // set_url 断开旧连接

    server_->stop();  // 旧端点下线：若仍指向旧端点 call 必然失败
    auto r = client.call("aria2.getGlobalStat", json::array(), &err);
    ASSERT_TRUE(r) << err.message;
    EXPECT_TRUE(client.is_connected());

    client.disconnect();
    server2->stop();
}

// IPv6 字面量解析（[::1]:port）+ set_url 换回后恢复
TEST_F(WsRpcClientTest, SetUrlParsesIpv6Literal) {
    WebSocketRpcClient client(client_config());
    ASSERT_TRUE(client.connect());

    // 服务器只监听 127.0.0.1：IPv6 回环解析正确但连接必败（解析分支被走通）
    client.set_url("ws://[::1]:1/jsonrpc");
    EXPECT_FALSE(client.connect());

    client.set_url(client_config().url);
    JsonRpcError err;
    auto r = client.call("aria2.tellActive", json::array(), &err);
    ASSERT_TRUE(r) << err.message;
}

// 裸主机 URL（无 path 无端口）：path 默认 /jsonrpc、端口默认 6800
TEST(WsRpcClientEdge, ParseUrlDefaultsForBareHost) {
    WebSocketRpcClient client(raw_client_config(
        "ws://nonexistent-falcon-test.invalid", /*timeout=*/2));
    EXPECT_FALSE(client.connect());  // 默认端口与路径解析不悬挂，按解析失败收口
}

// 自定义 path 原样进请求行
TEST(WsRpcClientEdge, CustomPathCarriedInRequestLine) {
    RawWsServer raw;
    // raw.url() = ws://127.0.0.1:<port>/jsonrpc → 换自定义 path
    const std::string base = raw.url();
    const std::string host_port = base.substr(5, base.find('/', 5) - 5);
    JsonRpcClientConfig cfg;
    cfg.url = "ws://" + host_port + "/myrpc";
    cfg.timeout_seconds = 5;
    WebSocketRpcClient client(cfg);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /myrpc HTTP/1.1");
}

// userinfo 剥离（parse_url 的 '@' 分支）：凭据段不进 host 解析。
// 不剥离时 host = "user:pass@127.0.0.1"（rfind(':') 命中端口前冒号），
// DNS 解析必败 → connect 成功 + 请求行正确即剥离生效的铁证
TEST(WsRpcClientEdge, ParseUrlStripsUserinfoBeforeHostResolution) {
    RawWsServer raw;
    const std::string base = raw.url();
    const std::string host_port = base.substr(5, base.find('/', 5) - 5);
    JsonRpcClientConfig cfg;
    cfg.url = "ws://user:pass@" + host_port + "/jsonrpc";
    cfg.timeout_seconds = 5;
    WebSocketRpcClient client(cfg);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /jsonrpc HTTP/1.1");
}

// 尾斜杠 path 归一化："ws://host:port/" 的 path = "/" → 归一化为 /jsonrpc
// （空 path 方向结构不可达：slash 命中时 substr 至少 "/"，else 分支恒 "/jsonrpc"）
TEST(WsRpcClientEdge, ParseUrlTrailingSlashPathBecomesJsonrpc) {
    RawWsServer raw;
    const std::string base = raw.url();
    const std::string host_port = base.substr(5, base.find('/', 5) - 5);
    JsonRpcClientConfig cfg;
    cfg.url = "ws://" + host_port + "/";
    cfg.timeout_seconds = 5;
    WebSocketRpcClient client(cfg);
    ASSERT_TRUE(client.connect());
    EXPECT_EQ(raw.request_line(), "GET /jsonrpc HTTP/1.1");
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


// 服务器推非 JSON 文本帧 / 合法 JSON 非 object 帧：客户端静默忽略
//（不断连、不误认为响应），随后同一连接上的请求-应答照常工作
TEST(WsRpcClientEdge, NonJsonFramesIgnoredAndConnectionStaysUsable) {
    RawWsServer raw;
    raw.on_connected = [](RawWsServer& s, int fd) {
        s.send_server_frame(fd, WS_OP_TEXT, "hello, not json");
        s.send_server_frame(fd, WS_OP_TEXT, "[1,2,3]");
    };
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    raw.on_request = [](const json& req) {
        return json{{"jsonrpc", "2.0"},
                    {"id", req["id"]},
                    {"result", json::array()}}.dump();
    };
    JsonRpcError err;
    auto result = client.call("aria2.tellActive", json::array(), &err);
    ASSERT_TRUE(result) << err.message;
}

#if defined(FALCON_FAILURE_INJECTION)

// 握手请求发送失败：connect 报 false，fd 被回收
TEST(WsRpcClientEdge, ConnectFailsWhenHandshakeSendInjected) {
    RawWsServer raw;
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::WsClientSendFail);
    EXPECT_FALSE(client.connect());
}

// 已连接后请求帧发送失败：挂起请求以 -32000 唤醒并标注失败原因
TEST(WsRpcClientEdge, CallFailsWhenSendFrameInjected) {
    RawWsServer raw;
    WebSocketRpcClient client(raw_client_config(raw.url()));
    ASSERT_TRUE(client.connect());

    ::falcon::detail::ScopedInjection guard(
        ::falcon::detail::InjectPoint::WsClientSendFail);
    JsonRpcError err;
    EXPECT_FALSE(client.call("aria2.getGlobalStat", json::array(), &err));
    EXPECT_EQ(err.code, -32000);
    EXPECT_EQ(err.message, "WebSocket send failed");
}

#endif  // FALCON_FAILURE_INJECTION

} // namespace} // namespace
