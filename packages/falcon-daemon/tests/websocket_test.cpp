// WebSocket 事件流订阅测试
// 覆盖：握手（RFC 6455 向量）、帧解析（掩码/分片/控制帧/错误）、
// WS 上的 JSON-RPC、引擎状态变更通知、进度通知节流、广播 fan-out、
// 关闭帧与停机清理。

#include "rpc/json_rpc_server.hpp"
#include "rpc/websocket_frame.hpp"

#include <falcon/download_engine.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using json = nlohmann::json;
using namespace falcon;
using falcon::daemon::rpc::WS_OP_CLOSE;
using falcon::daemon::rpc::WS_OP_CONTINUATION;
using falcon::daemon::rpc::WS_OP_PING;
using falcon::daemon::rpc::WS_OP_PONG;
using falcon::daemon::rpc::WS_OP_TEXT;
using falcon::daemon::rpc::WsFrame;
using falcon::daemon::rpc::WsFrameParser;
using falcon::daemon::rpc::ws_compute_accept_key;

#ifdef _WIN32
using recv_send_size_t = int;
static int socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
static void ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
static void set_recv_timeout_ms(int fd, int ms) {
    DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
#else
using recv_send_size_t = ssize_t;
static int socket_close(int fd) { return ::close(fd); }
static void ensure_winsock_started() {}
static void set_recv_timeout_ms(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
#endif

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) socket_close(fd);
    }
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
};

static bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        const auto chunk_len = static_cast<
#ifdef _WIN32
            int
#else
            std::size_t
#endif
            >(data.size() - off);
        recv_send_size_t n = ::send(fd, data.data() + off, chunk_len, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 客户端 → 服务端帧必须掩码（RFC 6455）
static std::string ws_client_frame(std::uint8_t opcode, const std::string& payload) {
    static std::atomic<std::uint32_t> counter{0x1};
    const std::uint32_t seed = counter.fetch_add(1) * 2654435761u + 0x9E3779B9u;
    std::uint8_t key[4] = {static_cast<std::uint8_t>(seed >> 24),
                           static_cast<std::uint8_t>(seed >> 16),
                           static_cast<std::uint8_t>(seed >> 8),
                           static_cast<std::uint8_t>(seed)};

    std::string f;
    f.push_back(static_cast<char>(0x80u | opcode));
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>(0x80u | n));
    } else if (n < 65536) {
        f.push_back(static_cast<char>(0x80u | 126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>(0x80u | 127));
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
        }
    }
    f.append(reinterpret_cast<const char*>(key), 4);
    for (std::size_t i = 0; i < n; ++i) {
        f.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
    }
    return f;
}

// 服务端帧（不掩码）手工构造，用于 FrameParser 纯单测
static std::string raw_server_frame(std::uint8_t fin, std::uint8_t opcode,
                                    bool masked, const std::string& payload,
                                    std::uint32_t mask = 0) {
    std::string f;
    f.push_back(static_cast<char>((fin ? 0x80u : 0u) | opcode));
    const std::size_t n = payload.size();
    if (n < 126) {
        f.push_back(static_cast<char>((masked ? 0x80u : 0u) | n));
    } else if (n < 65536) {
        f.push_back(static_cast<char>((masked ? 0x80u : 0u) | 126));
        f.push_back(static_cast<char>((n >> 8) & 0xFF));
        f.push_back(static_cast<char>(n & 0xFF));
    } else {
        f.push_back(static_cast<char>((masked ? 0x80u : 0u) | 127));
        for (int i = 7; i >= 0; --i) {
            f.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
        }
    }
    if (masked) {
        std::uint8_t key[4] = {static_cast<std::uint8_t>(mask >> 24),
                               static_cast<std::uint8_t>(mask >> 16),
                               static_cast<std::uint8_t>(mask >> 8),
                               static_cast<std::uint8_t>(mask)};
        f.append(reinterpret_cast<const char*>(key), 4);
        for (std::size_t i = 0; i < n; ++i) {
            f.push_back(static_cast<char>(payload[i] ^ key[i % 4]));
        }
    } else {
        f += payload;
    }
    return f;
}

// 最小 WebSocket 测试客户端：握手 → 掩码帧收发
class WsTestClient {
public:
    WsTestClient() { ensure_winsock_started(); }
    ~WsTestClient() { close(); }

    WsTestClient(const WsTestClient&) = delete;
    WsTestClient& operator=(const WsTestClient&) = delete;

    bool connect(uint16_t port, const std::string& path = "/jsonrpc") {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        for (int attempt = 0; attempt < 50; ++attempt) {
            fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
            if (fd_ < 0) return false;
            if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                break;
            }
            socket_close(fd_);
            fd_ = -1;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (fd_ < 0) return false;

        std::string req;
        req += "GET " + path + " HTTP/1.1\r\n";
        req += "Host: 127.0.0.1\r\n";
        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";
        req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
        req += "Sec-WebSocket-Version: 13\r\n\r\n";
        if (!send_all(fd_, req)) return false;

        std::string buf;
        while (buf.find("\r\n\r\n") == std::string::npos) {
            char tmp[1024];
            recv_send_size_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            buf.append(tmp, tmp + n);
        }
        const auto pos = buf.find("\r\n\r\n");
        handshake_response_ = buf.substr(0, pos);
        // 握手响应之后可能已捎带首帧数据，一并喂入解析器
        buffered_ = buf.substr(pos + 4);
        if (!buffered_.empty()) {
            parser_.feed(buffered_.data(), buffered_.size());
            buffered_.clear();
            pending_ = parser_.pop_messages();
        }
        return handshake_response_.rfind("HTTP/1.1 101", 0) == 0;
    }

    const std::string& handshake_response() const { return handshake_response_; }

    bool send_frame(std::uint8_t opcode, const std::string& payload) {
        return send_all(fd_, ws_client_frame(opcode, payload));
    }

    // 读一条完整消息；timeout_ms 内无数据或连接断开返回 nullopt
    std::optional<WsFrame> read_frame(int timeout_ms) {
        set_recv_timeout_ms(fd_, timeout_ms);
        while (true) {
            if (!pending_.empty()) {
                WsFrame msg = pending_.front();
                pending_.erase(pending_.begin());
                return msg;
            }
            char tmp[4096];
            recv_send_size_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
            if (n <= 0) return std::nullopt;
            parser_.feed(tmp, static_cast<std::size_t>(n));
            if (parser_.error()) return std::nullopt;
            pending_ = parser_.pop_messages();
        }
    }

    // 读取循环直到收到指定 method 的通知帧（跳过 JSON-RPC 响应）
    std::optional<json> read_notification(const std::string& method, int timeout_ms) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline) {
            const int remain = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now())
                    .count());
            if (remain <= 0) break;
            auto frame = read_frame(remain);
            if (!frame) break;
            if (frame->opcode != WS_OP_TEXT) continue;
            json parsed = json::parse(frame->payload, nullptr, false);
            if (parsed.is_discarded()) continue;
            if (parsed.value("method", "") == method) {
                return std::optional<json>(std::in_place, std::move(parsed));
            }
        }
        return std::nullopt;
    }

    bool closed_by_server(int timeout_ms) {
        set_recv_timeout_ms(fd_, timeout_ms);
        char tmp[256];
        recv_send_size_t n = ::recv(fd_, tmp, sizeof(tmp), 0);
        return n <= 0;
    }

    void close() {
        if (fd_ >= 0) {
            socket_close(fd_);
            fd_ = -1;
        }
    }

    // RST 关闭：SO_LINGER{1,0} + close，不发 FIN——对端下一次 send/recv
    // 立即得到连接重置，用于模拟客户端崩溃式断连
    void reset_close() {
        if (fd_ >= 0) {
            linger lg{};
            lg.l_onoff = 1;
            lg.l_linger = 0;
            ::setsockopt(fd_, SOL_SOCKET, SO_LINGER,
                         reinterpret_cast<const char*>(&lg), sizeof(lg));
            socket_close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
    std::string handshake_response_;
    std::string buffered_; // 握手响应后的多余字节（并入解析器）
    WsFrameParser parser_;
    std::vector<WsFrame> pending_;
};

// 阻塞式协议处理器：download() 进入即置 Downloading、可发进度事件，
// 阻塞直到 release()，随后置 Completed —— 用于驱动真实引擎事件链
class BlockingHandler final : public IProtocolHandler {
public:
    struct ProgressStep {
        float progress;
        Bytes downloaded;
        int delay_ms;
    };

    std::string protocol_name() const override { return "ws_blocking"; }
    std::vector<std::string> supported_schemes() const override { return {"https"}; }
    bool can_handle(const std::string& url) const override {
        return url.rfind("https://", 0) == 0;
    }
    FileInfo get_file_info(const std::string& url, const DownloadOptions&) override {
        FileInfo info;
        info.url = url;
        info.filename = "ws_test.bin";
        info.total_size = 1000;
        info.supports_resume = false;
        return info;
    }

    void download(DownloadTask::Ptr task, IEventListener* listener) override {
        if (prepare_first_) {
            // 先进 Preparing 再 Downloading：驱动 RpcEventBridge 的
            // Preparing→Downloading 通知分支（与 Pending→Downloading 区分）
            task->set_status(TaskStatus::Preparing);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        task->set_status(TaskStatus::Downloading);
        for (const auto& step : progress_steps_) {
            if (!listener) break;
            ProgressInfo info;
            info.task_id = task->id();
            info.progress = step.progress;
            info.downloaded_bytes = step.downloaded;
            info.total_bytes = 1000;
            info.speed = 100;
            listener->on_progress(info);
            if (step.delay_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(step.delay_ms));
            }
        }
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return released_.load(); });
        }
        task->set_status(TaskStatus::Completed);
    }

    void pause(DownloadTask::Ptr task) override { task->set_status(TaskStatus::Paused); }
    void resume(DownloadTask::Ptr task, IEventListener* listener) override {
        download(std::move(task), listener);
    }
    void cancel(DownloadTask::Ptr task) override { task->set_status(TaskStatus::Cancelled); }

    void set_progress_steps(std::vector<ProgressStep> steps) {
        progress_steps_ = std::move(steps);
    }

    void set_prepare_first(bool v) { prepare_first_ = v; }

    void release() {
        released_.store(true);
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> released_{false};
    std::vector<ProgressStep> progress_steps_;
    bool prepare_first_ = false;
};

struct ServerHarness {
    DownloadEngine engine;
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> server;

    explicit ServerHarness(const std::string& secret = "",
                           std::chrono::milliseconds progress_interval =
                               std::chrono::milliseconds{1000},
                           bool allow_origin_all = false) {
        cfg.listen_port = 0;
        cfg.bind_address = "127.0.0.1";
        cfg.secret = secret;
        cfg.allow_origin_all = allow_origin_all;
        cfg.progress_push_interval = progress_interval;
        server = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine, cfg);
        server->start();
    }

    ~ServerHarness() { server->stop(); }
};

/// 等待服务端注册表达到 n 个订阅者。
/// connect() 返回只代表客户端收到了 101，服务端会话线程可能尚未执行到
/// 注册；负载下该窗口可达毫秒级，立即断言 count 或广播会偶发落空。
bool wait_registered(falcon::daemon::rpc::JsonRpcServer* server, std::size_t n) {
    for (int i = 0; i < 500; ++i) {
        if (server->websocket_client_count() == n) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return server->websocket_client_count() == n;
}

// 两阶段 handler：第一阶段完成后停在 stage2 门等测试放行，随后以 Paused
// 收尾——测试在两阶段间隙把任务从引擎移除，驱动「任务已不存在时的通知
// 退化为仅 gid」分支
class TwoStageHandler final : public IProtocolHandler {
public:
    std::string protocol_name() const override { return "ws_twostage"; }
    std::vector<std::string> supported_schemes() const override { return {"https"}; }
    bool can_handle(const std::string& url) const override {
        return url.rfind("https://", 0) == 0;
    }
    FileInfo get_file_info(const std::string& url, const DownloadOptions&) override {
        FileInfo info;
        info.url = url;
        info.filename = "twostage.bin";
        info.total_size = 1000;
        info.supports_resume = false;
        return info;
    }

    void download(DownloadTask::Ptr task, IEventListener*) override {
        task->set_status(TaskStatus::Downloading);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stage1_.wait_for(lock, std::chrono::seconds(10),
                             [this] { return stage1_go_.load(); });
        }
        // 第一阶段终点：Complete 通知（此时任务仍在引擎）
        task->set_status(TaskStatus::Completed);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stage2_.wait_for(lock, std::chrono::seconds(10),
                             [this] { return stage2_go_.load(); });
        }
        // 第二阶段终点：Pause 通知（此时任务已被引擎移除——
        // set_status 无终态守卫，保留的 Ptr 仍可触发状态变化）
        task->set_status(TaskStatus::Paused);
    }

    void pause(DownloadTask::Ptr task) override { task->set_status(TaskStatus::Paused); }
    void resume(DownloadTask::Ptr task, IEventListener*) override {
        task->set_status(TaskStatus::Downloading);
    }
    void cancel(DownloadTask::Ptr task) override { task->set_status(TaskStatus::Cancelled); }

    bool supports_resume() const override { return false; }

    void go_stage1() {
        stage1_go_.store(true);
        stage1_.notify_all();
    }
    void go_stage2() {
        stage2_go_.store(true);
        stage2_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable stage1_;
    std::condition_variable stage2_;
    std::atomic<bool> stage1_go_{false};
    std::atomic<bool> stage2_go_{false};
};

} // namespace

// ============================================================================
// 纯协议层单测
// ============================================================================

TEST(WsProtocolTest, AcceptKeyRfc6455Vector) {
    // RFC 6455 §1.3 示例
    EXPECT_EQ(ws_compute_accept_key("dGhlIHNhbXBsZSBub25jZQ=="),
              "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(WsProtocolTest, FrameParserMaskedText) {
    WsFrameParser parser;
    const std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":1}";
    // 客户端帧：掩码 key = 0x01020304
    std::string frame = raw_server_frame(1, WS_OP_TEXT, true, payload, 0x01020304u);
    // 分两段喂入，验证增量解析
    parser.feed(frame.data(), 3);
    EXPECT_TRUE(parser.pop_messages().empty());
    parser.feed(frame.data() + 3, frame.size() - 3);

    auto msgs = parser.pop_messages();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].opcode, WS_OP_TEXT);
    EXPECT_EQ(msgs[0].payload, payload);
}

TEST(WsProtocolTest, FrameParserFragmentationWithControl) {
    WsFrameParser parser;
    // text 分片中间插入 ping：控制帧应立即透传，text 聚合为一条
    const std::string part1 = raw_server_frame(0, WS_OP_TEXT, false, "Hel");
    parser.feed(part1.data(), part1.size());
    parser.feed(raw_server_frame(0, WS_OP_PING, false, "hb").data(),
                raw_server_frame(0, WS_OP_PING, false, "hb").size());
    parser.feed(raw_server_frame(0, WS_OP_CONTINUATION, false, "lo").data(),
                raw_server_frame(0, WS_OP_CONTINUATION, false, "lo").size());
    parser.feed(raw_server_frame(1, WS_OP_CONTINUATION, false, "!").data(),
                raw_server_frame(1, WS_OP_CONTINUATION, false, "!").size());

    auto msgs = parser.pop_messages();
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].opcode, WS_OP_PING);
    EXPECT_EQ(msgs[0].payload, "hb");
    EXPECT_EQ(msgs[1].opcode, WS_OP_TEXT);
    EXPECT_EQ(msgs[1].payload, "Hello!");
}

TEST(WsProtocolTest, FrameParserRejectsBadOpcode) {
    WsFrameParser parser;
    const std::string frame = raw_server_frame(1, 0x3, false, "x");
    parser.feed(frame.data(), frame.size());
    EXPECT_TRUE(parser.error());
    EXPECT_TRUE(parser.pop_messages().empty());
}

TEST(WsProtocolTest, FrameParserRejectsOversize) {
    WsFrameParser parser;
    // 127 扩展长度声明超大 payload（不实际发送数据也应判错）
    std::string frame;
    frame.push_back(static_cast<char>(0x80u | WS_OP_TEXT));
    frame.push_back(static_cast<char>(127));
    for (int i = 0; i < 8; ++i) {
        frame.push_back(static_cast<char>(i == 0 ? 0x02 : 0x00)); // 32 位声明远超上限
    }
    parser.feed(frame.data(), frame.size());
    EXPECT_TRUE(parser.error());
}

TEST(WsProtocolTest, FrameParserRejectsOrphanContinuation) {
    WsFrameParser parser;
    const std::string frame = raw_server_frame(1, 0x0, false, "x");
    parser.feed(frame.data(), frame.size());
    EXPECT_TRUE(parser.error());
}

// ============================================================================
// 回环集成测试
// ============================================================================

TEST(WsServerTest, HandshakeOverLoopback) {
    ServerHarness h;
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));
    // RFC 6455 示例 key 对应的 Accept 值
    EXPECT_NE(client.handshake_response().find("Sec-WebSocket-Accept: "
                                               "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
              std::string::npos);
    EXPECT_TRUE(wait_registered(h.server.get(), 1u));
}

TEST(WsServerTest, PlainGetNotUpgraded) {
    ServerHarness h;
    ensure_winsock_started();
    ScopedFd fd = [&] {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(h.server->port());
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        int s = ::socket(AF_INET, SOCK_STREAM, 0);
        ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        return ScopedFd{s};
    }();
    ASSERT_GE(fd.fd, 0);

    const std::string req =
        "GET /jsonrpc HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    ASSERT_TRUE(send_all(fd.fd, req));

    std::string resp;
    char tmp[1024];
    while (true) {
        recv_send_size_t n = ::recv(fd.fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        resp.append(tmp, tmp + n);
    }
    EXPECT_NE(resp.find("405"), std::string::npos);
}

TEST(WsServerTest, JsonRpcOverWebsocket) {
    ServerHarness h;
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    json req = {{"jsonrpc", "2.0"}, {"id", 7}, {"method", "system.listMethods"},
                {"params", json::array()}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    auto frame = client.read_frame(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, WS_OP_TEXT);
    json resp = json::parse(frame->payload);
    EXPECT_EQ(resp.value("id", 0), 7);
    ASSERT_TRUE(resp.contains("result"));
    EXPECT_TRUE(resp["result"].is_array());
}

TEST(WsServerTest, JsonRpcOverWebsocketAuth) {
    ServerHarness h("s3cr3t");
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    // 缺 token：与 HTTP 同一认证路径
    json req1 = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "system.listMethods"},
                 {"params", json::array()}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req1.dump()));
    auto frame1 = client.read_frame(5000);
    ASSERT_TRUE(frame1.has_value());
    json resp1 = json::parse(frame1->payload);
    ASSERT_TRUE(resp1.contains("error"));
    EXPECT_EQ(resp1["error"]["code"], -32001);

    // 正确 token
    json req2 = {{"jsonrpc", "2.0"}, {"id", 2}, {"method", "system.listMethods"},
                 {"params", json::array({"token:s3cr3t"})}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req2.dump()));
    auto frame2 = client.read_frame(5000);
    ASSERT_TRUE(frame2.has_value());
    json resp2 = json::parse(frame2->payload);
    EXPECT_TRUE(resp2.contains("result"));
}

TEST(WsServerTest, DownloadNotificationsOverWebsocket) {
    auto handler = std::make_unique<BlockingHandler>();
    auto* handler_ptr = handler.get();
    ServerHarness h;
    h.engine.register_handler(std::move(handler));

    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    // 通过 WS 发起下载（顺带覆盖 WS 上的 addUri）
    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.addUri"},
                {"params", json::array({json::array({"https://example.com/ws.bin"})})}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    auto start = client.read_notification("aria2.onDownloadStart", 5000);
    ASSERT_TRUE(start.has_value());
    ASSERT_TRUE(start->contains("params"));
    ASSERT_TRUE((*start)["params"].is_array() && !(*start)["params"].empty());
    // Falcon 扩展进度快照字段
    EXPECT_TRUE((*start)["params"][0].contains("status"));
    EXPECT_TRUE((*start)["params"][0].contains("completedLength"));

    handler_ptr->release();

    auto complete = client.read_notification("aria2.onDownloadComplete", 5000);
    ASSERT_TRUE(complete.has_value());
    EXPECT_EQ((*complete)["params"][0]["gid"], (*start)["params"][0]["gid"]);
}

TEST(WsServerTest, ProgressNotificationAndThrottling) {
    auto handler = std::make_unique<BlockingHandler>();
    auto* handler_ptr = handler.get();
    // 间隔 10 秒：同一任务 80ms 内的两次进度只推第一条（节流生效）
    ServerHarness h("", std::chrono::milliseconds{10000});
    h.engine.register_handler(std::move(handler));

    handler_ptr->set_progress_steps({{0.25f, 250, 0}, {0.50f, 500, 80}});

    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    json req = {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "aria2.addUri"},
                {"params", json::array({json::array({"https://example.com/p.bin"})})}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    auto start = client.read_notification("aria2.onDownloadStart", 5000);
    ASSERT_TRUE(start.has_value());

    auto progress = client.read_notification("falcon.onProgress", 5000);
    ASSERT_TRUE(progress.has_value());
    // progress 是 Falcon 扩展的浮点（0~1）
    EXPECT_TRUE((*progress)["params"][0].contains("progress"));
    // 节流窗口内（80ms < 10s）不应出现第二条
    auto second = client.read_notification("falcon.onProgress", 300);
    EXPECT_FALSE(second.has_value());

    handler_ptr->release();
}

TEST(WsServerTest, BroadcastFanout) {
    ServerHarness h;
    WsTestClient a, b;
    ASSERT_TRUE(a.connect(h.server->port()));
    ASSERT_TRUE(b.connect(h.server->port()));
    // 两端都注册进广播表后再广播，否则快照可能漏掉尚未注册的连接
    ASSERT_TRUE(wait_registered(h.server.get(), 2u));

    h.server->broadcast_notification("falcon.test",
                                     R"([{"gid":"0000000000000042"}])");

    auto fa = a.read_frame(5000);
    ASSERT_TRUE(fa.has_value());
    json ja = json::parse(fa->payload);
    EXPECT_EQ(ja.value("method", ""), "falcon.test");
    EXPECT_EQ(ja["params"][0]["gid"], "0000000000000042");
    // 通知无 id 字段（JSON-RPC 通知语义）
    EXPECT_FALSE(ja.contains("id"));

    auto fb = b.read_frame(5000);
    ASSERT_TRUE(fb.has_value());
    json jb = json::parse(fb->payload);
    EXPECT_EQ(jb.value("method", ""), "falcon.test");
}

TEST(WsServerTest, CloseFrameRoundtripAndCleanup) {
    ServerHarness h;
    {
        WsTestClient client;
        ASSERT_TRUE(client.connect(h.server->port()));
        ASSERT_TRUE(wait_registered(h.server.get(), 1u));

        ASSERT_TRUE(client.send_frame(WS_OP_CLOSE, std::string("\x03\xE8", 2)));
        auto frame = client.read_frame(5000);
        ASSERT_TRUE(frame.has_value());
        EXPECT_EQ(frame->opcode, WS_OP_CLOSE);
        // 服务端随后关闭连接
        EXPECT_TRUE(client.closed_by_server(5000));
    }
    // 会话线程退出后注册表清理
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (h.server->websocket_client_count() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(h.server->websocket_client_count(), 0u);
}

TEST(WsServerTest, StopWithActiveSubscriberDoesNotHang) {
    ServerHarness h;
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    // 客户端保持连接（阻塞在服务器会话线程的 recv），stop() 必须 shutdown 唤醒
    const auto started = std::chrono::steady_clock::now();
    h.server->stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              5000);
    EXPECT_EQ(h.server->websocket_client_count(), 0u);
}

// ===========================================================================
// Batch P: handshake routing, control-frame handling and failure paths.
// ===========================================================================

TEST(WsServerTest, WsUpgradeUnknownPathReturns404) {
    ServerHarness h;
    WsTestClient client;
    // path 白名单之外的 WS 升级请求按普通 HTTP 404 拒绝
    EXPECT_FALSE(client.connect(h.server->port(), "/nope"));
    EXPECT_NE(client.handshake_response().find("HTTP/1.1 404"), std::string::npos);
}

TEST(WsServerTest, WsHandshakeEchoesCorsHeaderWhenAllowed) {
    ServerHarness h("", std::chrono::milliseconds{1000}, true);
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));
    EXPECT_NE(client.handshake_response().find("Access-Control-Allow-Origin: *"),
              std::string::npos);
}

TEST(WsServerTest, WsPingPongThenPongIgnored) {
    ServerHarness h;
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));
    ASSERT_TRUE(wait_registered(h.server.get(), 1u));

    // ping → 服务器原样回 pong
    ASSERT_TRUE(client.send_frame(WS_OP_PING, "hb"));
    auto pong = client.read_frame(5000);
    ASSERT_TRUE(pong.has_value());
    EXPECT_EQ(pong->opcode, WS_OP_PONG);
    EXPECT_EQ(pong->payload, "hb");

    // 服务器方向收到的 pong 被静默忽略（不回帧）
    ASSERT_TRUE(client.send_frame(WS_OP_PONG, "idle"));

    // 随后的 text RPC 正常应答——连接未被 pong 打断，也未产生多余应答
    json req = {{"jsonrpc", "2.0"},
                {"id", 9},
                {"method", "system.listMethods"},
                {"params", json::array()}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));
    auto resp = client.read_frame(5000);
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->opcode, WS_OP_TEXT);
    json parsed = json::parse(resp->payload);
    EXPECT_EQ(parsed.value("id", 0), 9);
}

TEST(WsServerTest, WsBadOpcodeFrameClosedWith1002) {
    ServerHarness h;
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));
    ASSERT_TRUE(wait_registered(h.server.get(), 1u));

    // 坏操作码（0x3 保留区间）→ 解析器 error → 协议违规 1002 close
    ASSERT_TRUE(client.send_frame(0x3, "x"));
    auto frame = client.read_frame(5000);
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->opcode, WS_OP_CLOSE);
    EXPECT_EQ(frame->payload, std::string("\x03\xEA", 2));
    EXPECT_TRUE(client.closed_by_server(5000));
}

TEST(WsServerTest, WsBroadcastToRstPeerWhileDispatchSlowShutsSession) {
#ifndef _WIN32
    // 服务器对已 RST 的 fd send 会触发 SIGPIPE（无 MSG_NOSIGNAL），先忽略
    ::signal(SIGPIPE, SIG_IGN);
#endif
    ServerHarness h;
    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));
    ASSERT_TRUE(wait_registered(h.server.get(), 1u));

    // 慢分发：会话线程滞留在 dispatch（forceShutdown 的 handler 睡 400ms），
    // 其间 RST 掉客户端并广播——广播快照仍含死 fd（发送失败），dispatch
    // 返回后的应答发送同样失败，两条失败路径都把会话注销
    h.server->set_shutdown_handler(
        [] { std::this_thread::sleep_for(std::chrono::milliseconds(400)); });

    json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "aria2.forceShutdown"},
                {"params", json::array()}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    client.reset_close();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    h.server->broadcast_notification("falcon.test", R"([{"gid":"0000000000000042"}])");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (h.server->websocket_client_count() != 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(h.server->websocket_client_count(), 0u);
}

TEST(WsServerTest, WsDownloadStartEmittedFromPreparingTransition) {
    auto handler = std::make_unique<BlockingHandler>();
    auto* handler_ptr = handler.get();
    handler_ptr->set_prepare_first(true);
    ServerHarness h;
    h.engine.register_handler(std::move(handler));

    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "aria2.addUri"},
                {"params", json::array({json::array({"https://example.com/prep.bin"})})}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    // Preparing→Downloading 同样触发 onDownloadStart
    auto start = client.read_notification("aria2.onDownloadStart", 5000);
    ASSERT_TRUE(start.has_value());

    handler_ptr->release();
}

TEST(WsServerTest, WsProgressPushResumesAfterThrottleInterval) {
    auto handler = std::make_unique<BlockingHandler>();
    auto* handler_ptr = handler.get();
    // 节流 50ms：第一步推送后睡 120ms（>50ms），第二步应再次推送
    ServerHarness h("", std::chrono::milliseconds{50});
    h.engine.register_handler(std::move(handler));

    handler_ptr->set_progress_steps({{0.25f, 250, 120}, {0.50f, 500, 0}});

    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "aria2.addUri"},
                {"params", json::array({json::array({"https://example.com/th.bin"})})}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    auto first = client.read_notification("falcon.onProgress", 5000);
    ASSERT_TRUE(first.has_value());
    // 节流窗口到期后恢复推送（时间戳被刷新）
    auto second = client.read_notification("falcon.onProgress", 5000);
    ASSERT_TRUE(second.has_value());
    EXPECT_LT((*first)["params"][0]["progress"].get<float>(),
              (*second)["params"][0]["progress"].get<float>());

    handler_ptr->release();
}

TEST(WsServerTest, WsNotificationAfterEngineRemovalDegradesToGidOnly) {
    auto handler = std::make_unique<TwoStageHandler>();
    auto* handler_ptr = handler.get();
    ServerHarness h;
    h.engine.register_handler(std::move(handler));

    WsTestClient client;
    ASSERT_TRUE(client.connect(h.server->port()));

    json req = {{"jsonrpc", "2.0"},
                {"id", 1},
                {"method", "aria2.addUri"},
                {"params", json::array({json::array({"https://example.com/two.bin"})})}};
    ASSERT_TRUE(client.send_frame(WS_OP_TEXT, req.dump()));

    ASSERT_TRUE(client.read_notification("aria2.onDownloadStart", 5000).has_value());
    handler_ptr->go_stage1();

    // 第一阶段终点：任务仍在引擎，Complete 通知携带完整进度快照
    auto complete = client.read_notification("aria2.onDownloadComplete", 5000);
    ASSERT_TRUE(complete.has_value());
    const std::string gid = (*complete)["params"][0]["gid"];
    EXPECT_TRUE((*complete)["params"][0].contains("status"));
    EXPECT_EQ(h.engine.remove_finished_tasks(), std::size_t{1});

    handler_ptr->go_stage2();
    // 第二阶段终点：任务已不在引擎，Pause 通知退化为仅 gid
    auto pause = client.read_notification("aria2.onDownloadPause", 5000);
    ASSERT_TRUE(pause.has_value());
    EXPECT_EQ((*pause)["params"][0]["gid"], gid);
    EXPECT_FALSE((*pause)["params"][0].contains("status"));
}
