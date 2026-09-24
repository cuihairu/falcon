// ============================================================================
// SwarmRpcServer 实现（见 swarm_rpc_server.hpp 头注释）
//
// 传输层骨架与 daemon json_rpc_server.cpp 形制同源（accept 线程 / 每连接
// worker / WS 升级 / 停机顺序），准入与路由为 swarmd 线协议专属：
//   - GET /v1/health 无鉴权；POST /jsonrpc 过 Bearer 门
//   - WS 升级仅 /jsonrpc 路径，同样过 Bearer 门
//   - JSON-RPC 层错误全部 HTTP 200（aria2 同形制）；限频命中 429
//   - 通知广播 params 为 object、帧无 id（与 daemon 数组式分叉，测试钉死）
// ============================================================================

#include "swarm_rpc_server.hpp"

#include "rpc/websocket_frame.hpp"

#include <falcon/detail/injection.hpp>
#include <falcon/logger.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstring>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "../common/swarm_protocol.hpp"
#include "swarm_rpc_handlers.hpp"

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

namespace falcon::swarm {

// 每连接发送互斥（广播线程与该连接的请求应答线程可能并发写同一 fd）
struct WsClientState {
    std::mutex send_mutex;
};

namespace ws = falcon::daemon::rpc;

namespace {

#ifdef _WIN32
using socket_len_t = int;
using recv_send_size_t = int;

void ensure_winsock_started() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        WSADATA data{};
        (void)WSAStartup(MAKEWORD(2, 2), &data);
    });
}

int socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
int socket_shutdown(int fd) {
    return ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
}
#else
using socket_len_t = socklen_t;
using recv_send_size_t = ssize_t;

void ensure_winsock_started() {}

int socket_close(int fd) { return ::close(fd); }
int socket_shutdown(int fd) { return ::shutdown(fd, SHUT_RDWR); }
#endif

// fd 生命周期：所有权归 ScopedFd，构造即接管、析构即关闭。挂到
// ws_clients_ 表后 fd 仍由 ScopedFd 持有——表内只存弱语义键（int）。
struct ScopedFd {
    int fd = -1;
    explicit ScopedFd(int f) : fd(f) {}
    ~ScopedFd() {
        if (fd >= 0) {
            socket_close(fd);
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd(ScopedFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    ScopedFd& operator=(ScopedFd&&) = delete;
};

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim(const std::string& s) {
    std::size_t begin = 0;
    while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin]))) {
        ++begin;
    }
    std::size_t end = s.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(begin, end - begin);
}

std::optional<std::size_t> parse_content_length(
    const std::unordered_map<std::string, std::string>& headers) {
    const auto it = headers.find("content-length");
    if (it == headers.end()) {
        return std::nullopt;
    }
    try {
        return static_cast<std::size_t>(std::stoull(it->second));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

// 循环 recv 至少 want_at_least 字节（append 进 out）；超过 max_bytes 或
// 对端关闭/错误返回 false。
bool recv_into(int fd, std::string& out, std::size_t want_at_least,
               std::size_t max_bytes) {
    char tmp[4096];
    while (out.size() < want_at_least) {
        const recv_send_size_t n =
            ::recv(fd, tmp, static_cast<recv_send_size_t>(sizeof(tmp)), 0);
        if (n <= 0) {
            return false;
        }
        out.append(tmp, static_cast<std::size_t>(n));
        if (out.size() > max_bytes) {
            return false;
        }
    }
    return true;
}

// 逐字节增量找 "\r\n\r\n"（头边界），解析请求行与头，按 Content-Length
// 补收 body。畸形/超限/断连返回 nullopt。
std::optional<SwarmRpcServer::HttpRequest> read_http_request(int fd) {
    std::string raw;
    static constexpr std::size_t kHeaderLimit = 64 * 1024;
    static constexpr std::size_t kBodyLimit = 4 * 1024 * 1024;

    if (!recv_into(fd, raw, 4, kHeaderLimit + kBodyLimit)) {
        return std::nullopt;
    }
    while (raw.find("\r\n\r\n") == std::string::npos) {
        if (!recv_into(fd, raw, raw.size() + 1, kHeaderLimit + kBodyLimit)) {
            return std::nullopt;
        }
    }
    const std::size_t head_end = raw.find("\r\n\r\n");
    std::string head = raw.substr(0, head_end);
    std::string rest = raw.substr(head_end + 4);

    std::istringstream stream(head);
    std::string version;
    SwarmRpcServer::HttpRequest req;
    if (!(stream >> req.method >> req.path >> version)) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        req.headers[to_lower(trim(line.substr(0, colon)))] =
            trim(line.substr(colon + 1));
    }

    if (req.method == "POST" || req.method == "PUT") {
        const auto content_length = parse_content_length(req.headers);
        if (content_length.has_value()) {
            if (*content_length > kBodyLimit) {
                return std::nullopt;
            }
            if (!recv_into(fd, rest, *content_length, kHeaderLimit + kBodyLimit)) {
                return std::nullopt;
            }
            req.body = rest.substr(0, *content_length);
        }
    }
    return req;
}

bool send_all(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        const recv_send_size_t n = ::send(
            fd, data.data() + sent,
            static_cast<recv_send_size_t>(data.size() - sent), 0);
#else
        const recv_send_size_t n =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

std::string format_http_response(const SwarmRpcServer::HttpResponse& resp) {
    std::string out = "HTTP/1.1 " + std::to_string(resp.status_code) + " " +
                      resp.status_text + "\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    out += "Connection: close\r\n";
    out += "\r\n";
    out += resp.body;
    return out;
}

std::string status_text(int code) {
    switch (code) {
        case 200:
            return "OK";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 429:
            return "Too Many Requests";
        default:
            return "OK";
    }
}

std::string result_envelope(const nlohmann::json& id,
                            const nlohmann::json& result) {
    return nlohmann::json{
        {"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
}

std::string error_envelope(const nlohmann::json& id, int code,
                           const std::string& message) {
    return nlohmann::json{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", code}, {"message", message}}}}
        .dump();
}

}  // namespace

SwarmRpcServer::SwarmRpcServer(SwarmServerOptions options,
                               SwarmServerState& state)
    : opts_(std::move(options)),
      state_(state),
      register_limiter_(opts_.rate_register_per_min, std::chrono::minutes(1)),
      query_limiter_(opts_.rate_query_per_min, std::chrono::minutes(1)) {}

SwarmRpcServer::~SwarmRpcServer() { stop(); }

std::string SwarmRpcServer::last_error() const {
    std::lock_guard<std::mutex> lock(last_error_mutex_);
    return last_error_;
}

void SwarmRpcServer::set_last_error(std::string message) {
    std::lock_guard<std::mutex> lock(last_error_mutex_);
    last_error_ = std::move(message);
}

bool SwarmRpcServer::start() {
    if (accept_thread_.joinable()) {
        return true;
    }
    stop_requested_ = false;
    set_last_error("");
    ensure_winsock_started();
#ifndef _WIN32
    // 服务器被客户端 abort 是常态（测试客户端失败收口、浏览器断连），
    // SIGPIPE 必须进程级免疫——macOS XNU 对 socket 写信号做进程级投递，
    // 线程级屏蔽不可依赖（CI 红面 1f44064/02671e7 教训直落新基建）。
    ::signal(SIGPIPE, SIG_IGN);
#endif

    listen_fd_ = ::falcon::detail::inject_failure(
                     ::falcon::detail::InjectPoint::SwarmServerSocket)
                     ? -1
                     : ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        set_last_error("socket() failed");
        FALCON_LOG_ERROR_STREAM("SwarmRpcServer socket() failed");
        return false;
    }

    int reuse = 1;
    (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                       reinterpret_cast<const char*>(&reuse),
#else
                       &reuse,
#endif
                       sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(opts_.port);
    if (::inet_pton(AF_INET, opts_.host.c_str(), &addr.sin_addr) != 1) {
        set_last_error("invalid bind host: " + opts_.host);
        FALCON_LOG_ERROR_STREAM(
            "SwarmRpcServer inet_pton() failed for host=" << opts_.host);
        socket_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) !=
        0) {
        set_last_error("bind() failed");
        FALCON_LOG_ERROR_STREAM("SwarmRpcServer bind() failed");
        socket_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    if (opts_.port == 0) {
        sockaddr_in bound{};
        socket_len_t len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound),
                          &len) == 0) {
            port_.store(ntohs(bound.sin_port));
        }
    } else {
        port_.store(opts_.port);
    }

    if (::falcon::detail::inject_failure(
            ::falcon::detail::InjectPoint::SwarmServerListen) ||
        ::listen(listen_fd_, 128) != 0) {
        set_last_error("listen() failed");
        FALCON_LOG_ERROR_STREAM("SwarmRpcServer listen() failed");
        socket_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    accept_thread_ = std::thread([this] { accept_loop(); });
    sweep_thread_ = std::thread([this] { sweep_loop(); });
    FALCON_LOG_INFO_STREAM("swarmd listening on " << opts_.host << ":"
                                                  << port_.load());
    return true;
}

void SwarmRpcServer::stop() {
    stop_requested_ = true;
    sweep_cv_.notify_all();

    // 唤醒阻塞在 recv 的 WS 会话线程（shutdown 而非 close——防跨线程
    // close 的 fd 复用竞争；会话线程注销自己的表项）
    std::vector<int> ws_fds;
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        for (const auto& entry : ws_clients_) {
            ws_fds.push_back(entry.first);
        }
    }
    for (int fd : ws_fds) {
        socket_shutdown(fd);
    }

    if (listen_fd_ >= 0) {
        socket_shutdown(listen_fd_);
        socket_close(listen_fd_);
        listen_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    std::vector<std::thread> workers;
    {
        std::lock_guard<std::mutex> lock(worker_threads_mutex_);
        workers.swap(worker_threads_);
    }
    for (std::thread& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    if (sweep_thread_.joinable()) {
        sweep_thread_.join();
    }
}

void SwarmRpcServer::accept_loop() {
    while (!stop_requested_.load()) {
        // accept 缓冲按家族给足：sockaddr_storage 恒安全（IPv6 peer /
        // 平台语义差异教训，CI 红面 35480073040）
        sockaddr_storage peer{};
        socket_len_t len = sizeof(peer);
        const int client_fd = ::accept(
            listen_fd_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (client_fd < 0) {
            if (stop_requested_.load()) {
                break;
            }
            continue;
        }
        std::lock_guard<std::mutex> lock(worker_threads_mutex_);
        worker_threads_.emplace_back([this, client_fd] {
            handle_connection(client_fd);
        });
    }
}

void SwarmRpcServer::handle_connection(int client_fd) {
    ScopedFd fd(client_fd);
    auto req = read_http_request(fd.fd);
    if (!req.has_value()) {
        return;
    }
    const std::string peer = peer_ip(fd.fd);
    if (is_websocket_upgrade(*req)) {
        handle_websocket(fd.fd, *req);
        return;
    }
    (void)send_all(fd.fd,
                   format_http_response(handle_http_request(*req, peer)));
}

bool SwarmRpcServer::is_websocket_upgrade(const HttpRequest& req) const {
    if (req.method != "GET") {
        return false;
    }
    const auto upgrade = req.headers.find("upgrade");
    if (upgrade == req.headers.end() ||
        to_lower(upgrade->second).find("websocket") == std::string::npos) {
        return false;
    }
    const auto key = req.headers.find("sec-websocket-key");
    return key != req.headers.end() && !key->second.empty();
}

bool SwarmRpcServer::bearer_ok(
    const std::unordered_map<std::string, std::string>& headers) const {
    if (opts_.server_token.empty()) {
        return true;
    }
    const auto it = headers.find("authorization");
    if (it == headers.end()) {
        return false;
    }
    // RFC 9110 §11：auth-scheme 大小写不敏感，逐字符 tolower 比较
    static constexpr std::string_view kPrefix = "bearer ";
    const std::string& value = it->second;
    if (value.size() <= kPrefix.size()) {
        return false;
    }
    for (std::size_t i = 0; i < kPrefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) !=
            kPrefix[i]) {
            return false;
        }
    }
    return value.compare(kPrefix.size(), std::string::npos,
                         opts_.server_token) == 0;
}

SwarmRpcServer::HttpResponse SwarmRpcServer::handle_http_request(
    const HttpRequest& req, const std::string& peer) {
    // 健康面：无鉴权（连通性探测，无副作用）
    if (req.method == "GET" && req.path == "/v1/health") {
        HttpResponse resp;
        resp.body = nlohmann::json{
            {"status", "ok"}, {"peers", state_.peer_count()}}.dump();
        return resp;
    }

    if (req.path != "/jsonrpc") {
        HttpResponse resp;
        resp.status_code = 404;
        resp.status_text = status_text(404);
        resp.body = error_envelope(nlohmann::json(nullptr), -32600,
                                   "Not found");
        return resp;
    }
    if (req.method != "POST") {
        HttpResponse resp;
        resp.status_code = 405;
        resp.status_text = status_text(405);
        resp.body = error_envelope(nlohmann::json(nullptr), -32600,
                                   "Method not allowed");
        return resp;
    }

    if (!bearer_ok(req.headers)) {
        HttpResponse resp;
        resp.status_code = 401;
        resp.status_text = status_text(401);
        resp.body = error_envelope(nlohmann::json(nullptr), kErrBearer,
                                   "Missing or invalid bearer token");
        return resp;
    }

    int http_status = 200;
    HttpResponse resp;
    resp.body = handle_jsonrpc(req.body, peer, &http_status);
    resp.status_code = http_status;
    resp.status_text = status_text(http_status);
    return resp;
}

void SwarmRpcServer::handle_websocket(int client_fd, const HttpRequest& req) {
    if (req.path != "/jsonrpc") {
        (void)send_all(client_fd, format_http_response([] {
            HttpResponse resp;
            resp.status_code = 404;
            resp.status_text = status_text(404);
            resp.body = error_envelope(nlohmann::json(nullptr), -32600,
                                       "Not found");
            return resp;
        }()));
        return;
    }
    if (!bearer_ok(req.headers)) {
        HttpResponse resp;
        resp.status_code = 401;
        resp.status_text = status_text(401);
        resp.body = error_envelope(nlohmann::json(nullptr), kErrBearer,
                                   "Missing or invalid bearer token");
        (void)send_all(client_fd, format_http_response(resp));
        return;
    }

    const auto key_it = req.headers.find("sec-websocket-key");
    std::string accept = "HTTP/1.1 101 Switching Protocols\r\n"
                         "Upgrade: websocket\r\n"
                         "Connection: Upgrade\r\n"
                         "Sec-WebSocket-Accept: ";
    accept += ws::ws_compute_accept_key(key_it->second);
    accept += "\r\n\r\n";
    if (!send_all(client_fd, accept)) {
        return;
    }

    auto state = std::make_shared<WsClientState>();
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        ws_clients_[client_fd] = state;
    }

    const std::string peer = peer_ip(client_fd);
    ws::WsFrameParser parser;
    char tmp[4096];
    bool disconnect = false;
    while (!stop_requested_.load() && !disconnect) {
        const recv_send_size_t n =
            ::recv(client_fd, tmp, static_cast<recv_send_size_t>(sizeof(tmp)),
                   0);
        if (n <= 0) {
            break;
        }
        parser.feed(tmp, static_cast<std::size_t>(n));
        if (parser.error()) {
            // 协议违规：CLOSE 1002（0x03EA 大端状态码）
            (void)ws_send_frame(client_fd, ws::WS_OP_CLOSE,
                                std::string("\x03\xEA", 2));
            break;
        }
        for (const ws::WsFrame& frame : parser.pop_messages()) {
            switch (frame.opcode) {
                case ws::WS_OP_TEXT:
                case ws::WS_OP_BINARY: {
                    int http_status = 200;
                    const std::string body =
                        handle_jsonrpc(frame.payload, peer, &http_status);
                    disconnect = !ws_send_frame(client_fd, ws::WS_OP_TEXT,
                                                body);
                    break;
                }
                case ws::WS_OP_PING:
                    disconnect = !ws_send_frame(client_fd, ws::WS_OP_PONG,
                                                frame.payload);
                    break;
                case ws::WS_OP_CLOSE:
                    (void)ws_send_frame(client_fd, ws::WS_OP_CLOSE,
                                        frame.payload);
                    disconnect = true;
                    break;
                default:
                    break;  // PONG/CONTINUATION：忽略
            }
            if (disconnect) {
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        ws_clients_.erase(client_fd);
    }
}

bool SwarmRpcServer::ws_send_frame(int client_fd, std::uint8_t opcode,
                                   const std::string& payload) {
    std::shared_ptr<WsClientState> state;
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        const auto it = ws_clients_.find(client_fd);
        if (it == ws_clients_.end()) {
            return false;
        }
        state = it->second;
    }
    std::lock_guard<std::mutex> send_lock(state->send_mutex);
    return send_all(client_fd, ws::ws_encode_frame(opcode, payload));
}

std::string SwarmRpcServer::handle_jsonrpc(const std::string& payload,
                                           const std::string& peer,
                                           int* http_status) {
    *http_status = 200;

    nlohmann::json req;
    try {
        req = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::parse_error&) {
        return error_envelope(nlohmann::json(nullptr), -32700, "Parse error");
    }
    if (!req.is_object()) {
        return error_envelope(nlohmann::json(nullptr), -32600,
                              "Invalid Request");
    }

    nlohmann::json id =
        req.contains("id") ? req["id"] : nlohmann::json(nullptr);
    const std::string method = req.value("method", std::string());
    if (method.empty()) {
        return error_envelope(id, -32600, "Invalid Request");
    }

    nlohmann::json params = nlohmann::json::object();
    if (req.contains("params")) {
        if (!req["params"].is_object()) {
            return error_envelope(id, -32600,
                                  "params must be an object");
        }
        params = req["params"];
    }

    // per-IP 限频（HTTP/WS 共用；对端 IP 不可得时不限——本机回环恒可得）
    SwarmRateLimiter* limiter = nullptr;
    if (method == kMethodRegister) {
        limiter = &register_limiter_;
    } else if (method == kMethodQuery) {
        limiter = &query_limiter_;
    }
    if (limiter != nullptr && !peer.empty() &&
        !limiter->allow(peer, SwarmRateLimiter::Clock::now())) {
        *http_status = 429;
        return error_envelope(id, kErrRateLimited, "Rate limit exceeded");
    }

    SwarmReply reply;
    try {
        reply = dispatch_swarm_method(state_, method, params,
                                      SwarmServerState::Clock::now());
    } catch (const std::exception& e) {
        return error_envelope(id, -32603,
                              std::string("Internal error: ") + e.what());
    }

    // 通知锁外广播（ok/error 均冲刷——通知随返回值带出即派发）
    for (const auto& note : reply.notifications) {
        broadcast_notification(note.method, note.params);
    }

    if (reply.ok()) {
        return result_envelope(id, reply.result);
    }
    return error_envelope(id, reply.error_code, reply.error_message);
}

void SwarmRpcServer::broadcast_notification(const std::string& method,
                                            const nlohmann::json& params) {
    // 通知帧无 id、params 为 object（与 daemon 数组式分叉，线协议钉死）
    const std::string body = nlohmann::json{
        {"jsonrpc", "2.0"}, {"method", method}, {"params", params}}.dump();
    const std::string frame = ws::ws_encode_frame(ws::WS_OP_TEXT, body);

    std::vector<std::pair<int, std::shared_ptr<WsClientState>>> targets;
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        targets.assign(ws_clients_.begin(), ws_clients_.end());
    }
    for (auto& [fd, state] : targets) {
        std::lock_guard<std::mutex> send_lock(state->send_mutex);
        if (!send_all(fd, frame)) {
            // 失败仅 shutdown 唤醒会话线程自行收尾（防跨线程 close 竞争）
            socket_shutdown(fd);
        }
    }
}

void SwarmRpcServer::sweep_loop() {
    std::unique_lock<std::mutex> lock(sweep_mutex_);
    while (!stop_requested_.load()) {
        sweep_cv_.wait_for(lock, opts_.sweep_interval,
                           [this] { return stop_requested_.load(); });
        if (stop_requested_.load()) {
            break;
        }
        lock.unlock();
        const std::vector<SwarmServerState::Notification> notes =
            state_.sweep(SwarmServerState::Clock::now());
        for (const auto& note : notes) {
            broadcast_notification(note.method, note.params);
        }
        lock.lock();
    }
}

std::string SwarmRpcServer::peer_ip(int fd) const {
    sockaddr_storage ss{};
    socket_len_t len = sizeof(ss);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&ss), &len) != 0) {
        return "";
    }
    char buf[INET6_ADDRSTRLEN] = {0};
    if (ss.ss_family == AF_INET) {
        const auto* a = reinterpret_cast<const sockaddr_in*>(&ss);
        if (::inet_ntop(AF_INET, &a->sin_addr, buf, sizeof(buf)) != nullptr) {
            return buf;
        }
    } else if (ss.ss_family == AF_INET6) {
        const auto* a = reinterpret_cast<const sockaddr_in6*>(&ss);
        if (::inet_ntop(AF_INET6, &a->sin6_addr, buf, sizeof(buf)) !=
            nullptr) {
            return buf;
        }
    }
    return "";
}

}  // namespace falcon::swarm
