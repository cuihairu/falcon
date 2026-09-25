// ============================================================================
// SwarmClient 实现（见 swarm_client.hpp 头注释）
// ============================================================================

#include "swarm_client.hpp"

#include "../common/swarm_protocol.hpp"

#include <algorithm>
#include <vector>

namespace falcon::swarm {

namespace {

constexpr int kJsonRpcParseError = -32700;
constexpr int kJsonRpcInternalFallback = -1;  // 传输层失败（本地合成码）

nlohmann::json make_request(const std::string& method,
                            const nlohmann::json& params, int id) {
    return nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params},
    };
}

}  // namespace

SwarmClient::SwarmClient(SwarmClientConfig cfg, SwarmKeyMaterial key)
    : cfg_(std::move(cfg)),
      key_(std::move(key)),
      http_(SwarmHttpClient::Options{cfg_.host, cfg_.port, cfg_.server_token,
                                     cfg_.rpc_timeout_ms.count()}) {}

SwarmClient::~SwarmClient() { stop(); }

std::string SwarmClient::node_id() const { return key_.node_id; }

std::string SwarmClient::session() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return session_;
}

SwarmError SwarmClient::rpc_call(const std::string& method,
                                 const nlohmann::json& params,
                                 nlohmann::json* result) {
    SwarmError err;
    static std::atomic<int> next_id{1};
    const std::string body =
        make_request(method, params, next_id.fetch_add(1)).dump();

    const SwarmHttpReply reply = http_.post_json("/jsonrpc", body);
    if (!reply.transport_ok) {
        err.code = kJsonRpcInternalFallback;
        err.message = reply.transport_error;
        return err;
    }
    err.http_status = reply.http_status;

    nlohmann::json envelope;
    try {
        envelope = nlohmann::json::parse(reply.body);
    } catch (const std::exception&) {
        err.code = kJsonRpcParseError;
        err.message = "invalid JSON-RPC response";
        return err;
    }

    const auto eit = envelope.find("error");
    if (eit != envelope.end() && eit->is_object()) {
        const auto code = eit->find("code");
        const auto msg = eit->find("message");
        if (code != eit->end() && code->is_number()) {
            err.code = code->get<int>();
        } else {
            err.code = kJsonRpcInternalFallback;
        }
        if (msg != eit->end() && msg->is_string()) {
            err.message = msg->get<std::string>();
        }
        return err;
    }

    if (result != nullptr) {
        const auto rit = envelope.find("result");
        if (rit != envelope.end()) {
            *result = *rit;
        }
    }
    return err;
}

SwarmError SwarmClient::do_register() {
    // ---- step1：身份 params（canonical 序列化）→ 申请挑战 ------------
    const std::vector<uint8_t> nonce_bytes = SwarmCrypto::random_bytes(8);
    const std::string nonce =
        SwarmCrypto::bytes_to_hex(nonce_bytes.data(), nonce_bytes.size());
    if (nonce.empty()) {  // random_bytes 失败（空 vector）→ 空 nonce 收口
        SwarmError err;
        err.code = kJsonRpcInternalFallback;
        err.message = "nonce generation failed";
        return err;
    }

    nlohmann::json params{
        {kFieldNodeId, key_.node_id},
        {kFieldPubkey, key_.pubkey_hex},
        {kFieldNonce, nonce},
        {kFieldAgent, cfg_.agent},
    };

    if (!cfg_.group_token.empty()) {
        params[kFieldGroupToken] = cfg_.group_token;
    }
    if (!cfg_.advertise_addr.empty()) {
        params[kFieldAdvertise] = nlohmann::json{
            {kFieldAddr, cfg_.advertise_addr},
            {kFieldDirect, cfg_.advertise_direct},
        };
    }

    const std::string canonical = canonical_json(params);
    nlohmann::json step1_result;
    SwarmError err =
        rpc_call(kMethodRegister, params, &step1_result);
    if (!err.ok()) return err;

    const auto chal = step1_result.find(kFieldChallenge);
    if (chal == step1_result.end() || !chal->is_string()) {
        err.code = kJsonRpcInternalFallback;
        err.message = "challenge missing in register step1 reply";
        return err;
    }

    // ---- step2：同一 params + challenge_sig（其余键不变——服务器
    //      canonical 快照绑定校验天然通过）------------------------------
    const std::string payload =
        signing_payload(kMethodRegister, nonce, canonical);
    params[kFieldChallengeSig] =
        SwarmCrypto::sign(key_.private_seed, payload);
    if (params[kFieldChallengeSig].get<std::string>().empty()) {
        err.code = kJsonRpcInternalFallback;
        err.message = "Ed25519 signing failed";
        return err;
    }

    nlohmann::json step2_result;
    err = rpc_call(kMethodRegister, params, &step2_result);
    if (!err.ok()) return err;

    const auto sit = step2_result.find(kFieldSession);
    const auto hit = step2_result.find(kFieldHeartbeatInterval);
    if (sit == step2_result.end() || !sit->is_string()) {
        err.code = kJsonRpcInternalFallback;
        err.message = "session missing in register step2 reply";
        return err;
    }

    std::lock_guard<std::mutex> lock(session_mutex_);
    session_ = sit->get<std::string>();
    // 服务器下发秒 → 毫秒，clamp [1s, 600s]（防极端值让心跳线程空转或
    // 挂死——aria2 对配置值的防御姿态）。
    long interval_s = 30;
    if (hit != step2_result.end() && hit->is_number_integer()) {
        interval_s = static_cast<long>(hit->get<long long>());
    }
    heartbeat_interval_ms_ =
        std::min(600000L, std::max(1000L, interval_s * 1000L));
    return err;
}

bool SwarmClient::start(std::string* error) {
    if (!key_.valid()) {
        if (error) *error = "invalid key material";
        return false;
    }
    if (subscriber_ != nullptr) return true;  // 幂等

    subscriber_ = std::make_unique<SwarmWsSubscriber>(
        SwarmWsSubscriber::Options{cfg_.host, cfg_.port, cfg_.server_token,
                                   cfg_.reconnect_delay,
                                   cfg_.rpc_timeout_ms});
    subscriber_->set_text_handler([this](const std::string&) {
        // 阶段 0：通知仅到达性消费（onPeerJoined/Left 由测试侧 harness
        // 客户端断言）；client 本体不基于通知改变会话状态。
    });
    subscriber_->start();

    const SwarmError err = do_register();
    if (!err.ok()) {
        subscriber_->stop();
        subscriber_.reset();
        if (error != nullptr) {
            *error = "register failed: " + err.message +
                     " (code " + std::to_string(err.code) + ")";
        }
        return false;
    }

    heartbeat_active_ = true;
    hb_stop_ = false;
    heartbeat_thread_ = std::thread([this] { heartbeat_loop(); });
    return true;
}

void SwarmClient::heartbeat_loop() {
    std::unique_lock<std::mutex> lock(hb_mutex_);
    while (!hb_stop_) {
        hb_cv_.wait_for(lock, std::chrono::milliseconds(heartbeat_interval_ms_),
                        [this] { return hb_stop_; });
        if (hb_stop_ || !heartbeat_active_) break;
        lock.unlock();

        std::string session;
        {
            std::lock_guard<std::mutex> slock(session_mutex_);
            session = session_;
        }
        if (session.empty()) {
            lock.lock();
            continue;
        }

        nlohmann::json result;
        const SwarmError err =
            rpc_call(kMethodHeartbeat, nlohmann::json{{kFieldSession, session}},
                     &result);
        heartbeat_count_.fetch_add(1);

        if (err.code == kErrUnknownSession) {
            // 自愈：server sweep 已摘除本会话 → 重注册换新 session
            const SwarmError rerr = do_register();
            if (rerr.ok()) reregister_count_.fetch_add(1);
        }

        lock.lock();
    }
}

SwarmError SwarmClient::query(const std::string& sha256_hex,
                              nlohmann::json* result) {
    std::string session;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session = session_;
    }
    if (session.empty()) {
        SwarmError err;
        err.code = kErrUnknownSession;
        err.message = "no active session";
        return err;
    }
    return rpc_call(kMethodQuery,
                    nlohmann::json{{kFieldSession, session},
                                   {kFieldSha256, sha256_hex}},
                    result);
}

void SwarmClient::detach() {
    {
        std::lock_guard<std::mutex> lock(hb_mutex_);
        heartbeat_active_ = false;
        hb_stop_ = true;
    }
    hb_cv_.notify_all();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    // 会话与 WS 收订保留（server 心跳超时后摘除并广播 onPeerLeft）
}

void SwarmClient::stop() {
    {
        std::lock_guard<std::mutex> lock(hb_mutex_);
        heartbeat_active_ = false;
        hb_stop_ = true;
    }
    hb_cv_.notify_all();
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

    // unsubscribe 尽力（一次性 HTTP；会话可能已过期——失败静默）
    std::string session;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session = std::move(session_);
        session_.clear();
    }
    if (!session.empty()) {
        rpc_call(kMethodUnsubscribe, nlohmann::json{{kFieldSession, session}},
                 nullptr);
    }

    if (subscriber_ != nullptr) {
        subscriber_->stop();
        subscriber_.reset();
    }
}

}  // namespace falcon::swarm
