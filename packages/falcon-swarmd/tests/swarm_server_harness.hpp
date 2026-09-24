#pragma once

// ============================================================================
// falcon-swarmd 测试 harness：SwarmRpcServer port=0 回环基建
//
// SwarmServerHarness   —— SwarmServerState + SwarmRpcServer 一体起停
//                         （默认限频 0=不限：无关用例不互染，限频用例显式
//                         自建实例并设阈值）；测试用 TTL（秒级）直接可观测。
// http_post/http_get   —— 裸 socket HTTP；服务器恒 Content-Length +
//                         Connection: close（swarm_rpc_server.cpp 响应形
//                         制实锤），read-to-EOF 即完整应答。
// SwarmWsClient        —— 升级握手（带 Bearer）+ 掩码发帧 + 收帧解析 +
//                         有界读（SO_RCVTIMEO，断言失败绝不挂死）。
// SwarmTestNode        —— 真实 Ed25519 密钥 + 指纹 node_id + 两步注册
//                         请求构造（线协议对拍与 server 自证分离：测试
//                         侧独立组 payload，不经被测 handlers）。
// wait_until           —— 轮询条件锚（20ms 步进，有界）。
//
// 形制母本：daemon tests/websocket_test.cpp（Winsock 映射 / ws_client_frame）
// 与 tests/main_integration_test.cpp（wait_until / TempDir 守卫）。
// ============================================================================

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "common/swarm_crypto.hpp"
#include "common/swarm_protocol.hpp"
#include "rpc/websocket_frame.hpp"
#include "server/swarm_rpc_server.hpp"
#include "server/swarm_server_state.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
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
using recv_send_size_t = int;
static int swarm_test_socket_close(int fd) {
    return ::closesocket(static_cast<SOCKET>(fd));
}
static void swarm_test_ensure_winsock() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using recv_send_size_t = ssize_t;
static int swarm_test_socket_close(int fd) { return ::close(fd); }
static void swarm_test_ensure_winsock() {}
#endif

namespace falcon::swarm::test {

// ===========================================================================
// 通用助手
// ===========================================================================

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

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
        const recv_send_size_t n = ::send(fd, data.data() + off, chunk_len, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

static void set_recv_timeout_ms(int fd, int ms) {
#ifdef _WIN32
    const DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

// ===========================================================================
// HTTP 客户端（read-to-EOF：服务器恒 Content-Length + Connection: close）
// ===========================================================================

struct HttpReply {
    int status = 0;
    std::string body;
    std::string raw;  // 完整应答（头断言用：Content-Type 等）
};

// 发一次请求并收完整应答；连接级失败返回 nullopt。
inline std::optional<HttpReply> http_request(
    std::uint16_t port, const std::string& raw_request, int timeout_ms = 5000) {
    swarm_test_ensure_winsock();

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::nullopt;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        swarm_test_socket_close(fd);
        return std::nullopt;
    }
    set_recv_timeout_ms(fd, timeout_ms);

    std::optional<HttpReply> reply;
    if (send_all(fd, raw_request)) {
        std::string raw;
        char buf[4096];
        for (;;) {
            const recv_send_size_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            raw.append(buf, static_cast<std::size_t>(n));
        }
        if (!raw.empty()) {
            HttpReply r;
            // 状态行："HTTP/1.1 200 OK"
            if (raw.rfind("HTTP/1.", 0) == 0) {
                const std::size_t sp1 = raw.find(' ');
                const std::size_t sp2 =
                    sp1 == std::string::npos ? sp1 : raw.find(' ', sp1 + 1);
                if (sp1 != std::string::npos && sp2 != std::string::npos) {
                    r.status = std::atoi(raw.substr(sp1 + 1, sp2 - sp1 - 1).c_str());
                }
            }
            const std::size_t sep = raw.find("\r\n\r\n");
            r.raw = raw;
            r.body = sep == std::string::npos ? std::string()
                                              : raw.substr(sep + 4);
            reply = std::move(r);
        }
    }
    swarm_test_socket_close(fd);
    return reply;
}

// JSON-RPC POST 便捷形态。extra_headers 逐条原样附加（每条须自带 "\r\n"）。
inline std::optional<HttpReply> http_post_json(
    std::uint16_t port, const nlohmann::json& body,
    const std::string& bearer_token = {},
    const std::vector<std::string>& extra_headers = {}) {
    std::string req = "POST /jsonrpc HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Type: application/json\r\n";
    if (!bearer_token.empty()) {
        req += "Authorization: Bearer " + bearer_token + "\r\n";
    }
    for (const auto& h : extra_headers) req += h;
    req += "Content-Length: " + std::to_string(body.dump().size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    req += body.dump();
    return http_request(port, req);
}

// 解析 JSON-RPC 信封：error → {code,message}；result → ok。
struct RpcOutcome {
    bool ok = false;
    int error_code = 0;
    std::string error_message;
    nlohmann::json result;
};

inline RpcOutcome parse_rpc_envelope(const std::string& body) {
    RpcOutcome out;
    nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
    if (j.is_discarded()) return out;
    if (j.contains("error") && j.at("error").is_object()) {
        const auto& e = j.at("error");
        if (e.contains("code")) out.error_code = e.at("code").get<int>();
        if (e.contains("message")) {
            out.error_message = e.at("message").get<std::string>();
        }
        return out;
    }
    if (j.contains("result")) {
        out.ok = true;
        out.result = j.at("result");
    }
    return out;
}

// POST + 信封解析一体（绝大多数用例只关心信封与 HTTP 状态）。
struct HttpRpcOutcome {
    int http_status = 0;
    bool ok = false;
    int error_code = 0;
    std::string error_message;
    nlohmann::json result;
    std::string raw_body;
};

inline HttpRpcOutcome http_rpc_call(std::uint16_t port,
                                    const std::string& method,
                                    const nlohmann::json& params, int id = 1,
                                    const std::string& bearer_token = {},
                                    const std::vector<std::string>& extra_headers = {}) {
    const nlohmann::json body = {
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    auto reply = http_post_json(port, body, bearer_token, extra_headers);
    HttpRpcOutcome out;
    if (!reply) {
        out.error_code = -1;
        out.error_message = "connection failed";
        return out;
    }
    out.http_status = reply->status;
    out.raw_body = reply->body;
    const RpcOutcome inner = parse_rpc_envelope(reply->body);
    out.ok = inner.ok;
    out.error_code = inner.error_code;
    out.error_message = inner.error_message;
    out.result = inner.result;
    return out;
}

// ===========================================================================
// SwarmTestNode：真实密钥 + 两步注册请求构造
// ===========================================================================

class SwarmTestNode {
public:
    SwarmTestNode() {
        auto kp = SwarmCrypto::generate_keypair();
        seed_ = std::move(kp.private_seed);
        pubkey_der_ = std::move(kp.public_key_der);
        node_id_ = SwarmCrypto::fingerprint(pubkey_der_);
        pubkey_hex_ = SwarmCrypto::bytes_to_hex(
            pubkey_der_.data(), pubkey_der_.size());
    }

    const std::string& node_id() const { return node_id_; }
    const std::string& pubkey_hex() const { return pubkey_hex_; }
    const std::vector<uint8_t>& seed() const { return seed_; }
    bool valid() const { return !seed_.empty() && !node_id_.empty(); }

    // step1 params（无 challenge_sig）。advertise 非空时附带。
    nlohmann::json step1_params(const std::string& nonce,
                                const std::string& agent = "falcon-test",
                                const nlohmann::json* advertise = nullptr,
                                const std::string& group_token = {}) const {
        nlohmann::json p{
            {kFieldNodeId, node_id_},
            {kFieldPubkey, pubkey_hex_},
            {kFieldNonce, nonce},
            {kFieldAgent, agent},
        };
        if (advertise != nullptr) p[kFieldAdvertise] = *advertise;
        if (!group_token.empty()) p[kFieldGroupToken] = group_token;
        return p;
    }

    // 对 canonical step1 params 签名（payload 组装独立于被测代码——
    // 防同一 bug 自我印证）。
    std::string sign_register(const std::string& nonce,
                              const std::string& canonical_params) const {
        const std::string payload =
            signing_payload(kMethodRegister, nonce, canonical_params);
        return SwarmCrypto::sign(seed_, payload);
    }

    // step2 params = step1 + challenge_sig
    static nlohmann::json step2_params(const nlohmann::json& step1,
                                       const std::string& challenge_sig) {
        nlohmann::json p = step1;
        p[kFieldChallengeSig] = challenge_sig;
        return p;
    }

private:
    std::vector<uint8_t> seed_;
    std::vector<uint8_t> pubkey_der_;
    std::string node_id_;
    std::string pubkey_hex_;
};

// ===========================================================================
// SwarmWsClient：升级握手 + 掩码发帧 + 有界收帧
// ===========================================================================

struct WsFrame {
    std::uint8_t opcode = 0;
    std::string payload;
};

class SwarmWsClient {
public:
    SwarmWsClient() = default;
    ~SwarmWsClient() {
        if (fd_ >= 0) swarm_test_socket_close(fd_);
    }
    SwarmWsClient(const SwarmWsClient&) = delete;
    SwarmWsClient& operator=(const SwarmWsClient&) = delete;

    // GET /jsonrpc 升级握手；upgrade_ok=false 时保留 fd 由用例读原始应答
    // （负路径断言 401/404 形态）。
    bool connect(std::uint16_t port, const std::string& bearer_token = {},
                 bool* upgrade_ok = nullptr) {
        swarm_test_ensure_winsock();
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
            0) {
            swarm_test_socket_close(fd_);
            fd_ = -1;
            return false;
        }
        set_recv_timeout_ms(fd_, 5000);

        // 定长随机形态的 Sec-WebSocket-Key（16 字节 → base64；测试不追求
        // 随机性，恒定值即可——服务器只回显派生值）
        const char* key = "dGhlIHNhbXBsZSBub25jZQ==";  // RFC 6455 示例 key
        std::string req = "GET /jsonrpc HTTP/1.1\r\n"
                          "Host: 127.0.0.1\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Key: ";
        req += std::string(key) + "\r\n";
        req += "Sec-WebSocket-Version: 13\r\n";
        if (!bearer_token.empty()) {
            req += "Authorization: Bearer " + bearer_token + "\r\n";
        }
        req += "\r\n";
        if (!send_all(fd_, req)) {
            swarm_test_socket_close(fd_);
            fd_ = -1;
            return false;
        }

        // 读应答头（\r\n\r\n 截断）
        handshake_raw_.clear();
        char buf[1024];
        std::size_t sep = std::string::npos;
        while (sep == std::string::npos) {
            const recv_send_size_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            handshake_raw_.append(buf, static_cast<std::size_t>(n));
            sep = handshake_raw_.find("\r\n\r\n");
        }
        const bool ok = sep != std::string::npos &&
                        handshake_raw_.rfind("HTTP/1.1 101", 0) == 0;
        if (upgrade_ok != nullptr) *upgrade_ok = ok;
        if (ok) {
            // 101 之后的残余字节进收帧缓冲
            buf_.assign(handshake_raw_.substr(sep + 4));
            // 验收 Sec-WebSocket-Accept 正确性（防假升级）
            const std::string expect =
                "Sec-WebSocket-Accept: " + falcon::daemon::rpc::ws_compute_accept_key(key);
            handshake_ok_ = handshake_raw_.find(expect) != std::string::npos;
            return handshake_ok_;
        }
        return false;
    }

    const std::string& handshake_raw() const { return handshake_raw_; }

    // 客户端 → 服务器帧必须掩码（RFC 6455）
    void send_frame(std::uint8_t opcode, const std::string& payload) {
        if (fd_ < 0) return;
        static std::uint32_t counter = 0x1;
        const std::uint32_t seed =
            (counter += 0x9E3779B9u) ^ 0xA5A5A5A5u;
        uint8_t key[4] = {static_cast<uint8_t>(seed >> 24),
                          static_cast<uint8_t>(seed >> 16),
                          static_cast<uint8_t>(seed >> 8),
                          static_cast<uint8_t>(seed)};
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
        send_all(fd_, f);
    }

    void send_text(const std::string& payload) {
        send_frame(0x1, payload);
    }
    void send_ping(const std::string& payload = "") {
        send_frame(0x9, payload);
    }

    // 有界读一帧（SO_RCVTIMEO 兜底 + deadline 双保险）；连接关/超时/解析
    // 失败返回 nullopt。
    std::optional<WsFrame> read_frame(int timeout_ms = 5000) {
        if (fd_ < 0) return std::nullopt;
        set_recv_timeout_ms(fd_, timeout_ms);
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            auto frame = try_parse_frame();
            if (frame) return frame;
            if (std::chrono::steady_clock::now() > deadline) return std::nullopt;
            char buf[4096];
            const recv_send_size_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) return std::nullopt;
            buf_.append(buf, static_cast<std::size_t>(n));
        }
    }

    int fd() const { return fd_; }

private:
    std::optional<WsFrame> try_parse_frame() {
        if (buf_.size() < 2) return std::nullopt;
        const auto byte_at = [&](std::size_t i) {
            return static_cast<uint8_t>(buf_[i]);
        };
        const std::uint8_t opcode = byte_at(0) & 0x0F;
        const bool masked = (byte_at(1) & 0x80u) != 0;
        std::uint64_t len = byte_at(1) & 0x7F;
        std::size_t off = 2;
        if (len == 126) {
            if (buf_.size() < off + 2) return std::nullopt;
            len = (static_cast<std::uint64_t>(byte_at(2)) << 8) |
                  static_cast<std::uint64_t>(byte_at(3));
            off += 2;
        } else if (len == 127) {
            if (buf_.size() < off + 8) return std::nullopt;
            len = 0;
            for (int i = 0; i < 8; ++i) {
                len = (len << 8) | byte_at(off + static_cast<std::size_t>(i));
            }
            off += 8;
        }
        if (masked) off += 4;  // 服务器 → 客户端不应掩码；形态错则跳过键
        if (buf_.size() < off + len) return std::nullopt;
        WsFrame frame;
        frame.opcode = opcode;
        frame.payload = buf_.substr(off, static_cast<std::size_t>(len));
        buf_.erase(0, static_cast<std::size_t>(off + len));
        return frame;
    }

    int fd_ = -1;
    std::string buf_;
    std::string handshake_raw_;
    bool handshake_ok_ = false;
};

// ===========================================================================
// SwarmServerHarness：状态 + 传输层一体起停
// ===========================================================================

struct HarnessConfig {
    std::string server_token;      // 空 = 不鉴权
    std::string group_token;       // 空 = 不校验
    std::vector<std::string> blacklist;
    std::chrono::seconds heartbeat_interval{1};
    std::chrono::seconds heartbeat_timeout{2};
    std::chrono::seconds challenge_ttl{5};
    std::chrono::milliseconds sweep_interval{50};
    std::size_t rate_register_per_min = 0;  // 默认不限（无关用例不互染）
    std::size_t rate_query_per_min = 0;
};

class SwarmServerHarness {
public:
    explicit SwarmServerHarness(const HarnessConfig& cfg = {}) {
        SwarmServerState::Config state_cfg;
        state_cfg.group_token = cfg.group_token;
        state_cfg.blacklist = cfg.blacklist;
        state_cfg.heartbeat_interval = cfg.heartbeat_interval;
        state_cfg.heartbeat_timeout = cfg.heartbeat_timeout;
        state_cfg.challenge_ttl = cfg.challenge_ttl;
        state_ = std::make_unique<SwarmServerState>(state_cfg);

        SwarmServerOptions opts;
        opts.host = "127.0.0.1";
        opts.port = 0;
        opts.server_token = cfg.server_token;
        opts.sweep_interval = cfg.sweep_interval;
        opts.rate_register_per_min = cfg.rate_register_per_min;
        opts.rate_query_per_min = cfg.rate_query_per_min;
        server_ = std::make_unique<SwarmRpcServer>(opts, *state_);
        started_ = server_->start();
    }

    ~SwarmServerHarness() {
        if (server_) server_->stop();
    }
    SwarmServerHarness(const SwarmServerHarness&) = delete;
    SwarmServerHarness& operator=(const SwarmServerHarness&) = delete;

    bool ok() const { return started_; }
    std::uint16_t port() const { return server_->port(); }
    SwarmServerState& state() { return *state_; }
    SwarmRpcServer& server() { return *server_; }

private:
    std::unique_ptr<SwarmServerState> state_;
    std::unique_ptr<SwarmRpcServer> server_;
    bool started_ = false;
};

// ===========================================================================
// 两步注册便捷流（HTTP 全链）：成功返回 session，失败 error_code 置位
// ===========================================================================

struct RegisterFlowResult {
    bool ok = false;
    std::string session;
    int error_code = 0;
    std::string error_message;
};

inline RegisterFlowResult register_node_two_steps(std::uint16_t port,
                                                  SwarmTestNode& node,
                                                  const std::string& nonce,
                                                  const std::string& bearer = {},
                                                  const std::string& group_token = {},
                                                  const nlohmann::json* advertise = nullptr) {
    RegisterFlowResult out;
    const nlohmann::json step1 = node.step1_params(nonce, "falcon-test",
                                                   advertise, group_token);
    const auto r1 = http_rpc_call(port, kMethodRegister, step1, 1, bearer);
    if (!r1.ok) {
        out.error_code = r1.error_code;
        out.error_message = r1.error_message;
        return out;
    }
    const auto challenge_it = r1.result.find(kFieldChallenge);
    if (challenge_it == r1.result.end() || !challenge_it->is_string()) {
        out.error_code = -1;
        out.error_message = "no challenge in step1 result";
        return out;
    }
    // challenge 值由 server 持有（单次消耗 + TTL），不进 step2 params——
    // step2 = step1 + challenge_sig，签名为 step1 快照的 signing_payload
    // （server 侧 complete_register 同一构造，测试侧独立组包）。
    const std::string sig = node.sign_register(nonce, canonical_json(step1));
    const auto r2 = http_rpc_call(
        port, kMethodRegister, SwarmTestNode::step2_params(step1, sig), 2,
        bearer);
    if (r2.ok) {
        out.ok = true;
        out.session = r2.result.value(kFieldSession, std::string());
        return out;
    }
    out.error_code = r2.error_code;
    out.error_message = r2.error_message;
    return out;
}

}  // namespace falcon::swarm::test
