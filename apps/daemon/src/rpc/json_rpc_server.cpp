#include "json_rpc_server.hpp"

#include "json_rpc_server_detail.hpp"

#include "rpc/mcp_server.hpp"
#include "rpc/websocket_frame.hpp"

#ifdef FALCON_HAS_SQLITE3
#include "storage/task_storage.hpp"
#endif

#include <falcon/detail/injection.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/logger.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

namespace falcon::daemon::rpc {
namespace {

using json = nlohmann::json;

#ifdef _WIN32
using socket_len_t = int;
using recv_send_size_t = int;
static int socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
static int socket_shutdown(int fd) { return ::shutdown(static_cast<SOCKET>(fd), SD_BOTH); }
static void ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
#else
using socket_len_t = socklen_t;
using recv_send_size_t = ssize_t;
static int socket_close(int fd) { return ::close(fd); }
static int socket_shutdown(int fd) { return ::shutdown(fd, SHUT_RDWR); }
static void ensure_winsock_started() {}
#endif

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) {
            socket_close(fd);
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ScopedFd(ScopedFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) socket_close(fd);
            fd = other.fd;
            other.fd = -1;
        }
        return *this;
    }
};

static std::optional<std::size_t> parse_content_length(const std::unordered_map<std::string, std::string>& headers) {
    auto it = headers.find("content-length");
    if (it == headers.end()) return std::nullopt;
    try {
        return static_cast<std::size_t>(std::stoull(it->second));
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// WebSocket 订阅者状态与事件桥
// ---------------------------------------------------------------------------

} // namespace

/// 每连接写互斥（WebSocket 与 MCP SSE 订阅者共用同一形状）。
/// 广播线程与该连接的会话线程经 shared_ptr 共享，
/// 保证注销后仍在途的广播发送可以安全完成。
struct StreamClientState {
    std::mutex send_mutex;
};

namespace {

/// 通知 params[0]：gid + Falcon 扩展进度快照（aria2 原生通知只带 gid，
/// 额外字段对严格客户端是透明超集）
static json notification_entry(const falcon::DownloadTask* task) {
    json entry;
    entry["gid"] = detail::task_id_to_gid(task->id());
    entry["status"] = detail::aria2_status_from_task(*task);
    entry["totalLength"] = std::to_string(task->total_bytes());
    entry["completedLength"] = std::to_string(task->downloaded_bytes());
    entry["downloadSpeed"] = std::to_string(task->speed());
    return entry;
}

} // namespace

// 引擎事件 → WebSocket JSON-RPC 通知桥。
// 回调来自 EventDispatcher 线程；detach() 后（停机）所有事件被丢弃，
// 与 TaskStorageListener 相同的 mutex + 空指针检查模式。
// engine 指针与 server 生命周期一致（构造后不变），无需随 mutex 保护。
class RpcEventBridge final : public falcon::IEventListener {
public:
    RpcEventBridge(JsonRpcServer* server, falcon::DownloadEngine* engine)
        : server_(server), engine_(engine) {}

    void attach(JsonRpcServer* server) {
        std::lock_guard<std::mutex> lock(mutex_);
        server_ = server;
        last_progress_push_.clear();
    }

    void detach() {
        std::lock_guard<std::mutex> lock(mutex_);
        server_ = nullptr;
        last_progress_push_.clear();
    }

    void set_progress_interval(std::chrono::milliseconds interval) {
        std::lock_guard<std::mutex> lock(mutex_);
        progress_interval_ = interval;
    }

    void on_status_changed(falcon::TaskId task_id, falcon::TaskStatus old_status,
                           falcon::TaskStatus new_status) override {
        const char* method = nullptr;
        switch (new_status) {
            case falcon::TaskStatus::Downloading:
                // 下载真正开始（含从 Paused 恢复）；Preparing 归入 active 但不算 Start
                if (old_status == falcon::TaskStatus::Pending ||
                    old_status == falcon::TaskStatus::Preparing ||
                    old_status == falcon::TaskStatus::Paused) {
                    method = "aria2.onDownloadStart";
                }
                break;
            case falcon::TaskStatus::Paused:
                method = "aria2.onDownloadPause";
                break;
            case falcon::TaskStatus::Completed:
                method = "aria2.onDownloadComplete";
                break;
            case falcon::TaskStatus::Failed:
                method = "aria2.onDownloadError";
                break;
            case falcon::TaskStatus::Cancelled:
                method = "aria2.onDownloadStop";
                break;
            default:
                break;
        }
        if (!method) return;

        // 进度快照取自引擎当前内存态；任务可能已被移除（退化为仅 gid）
        json params = json::array();
        if (auto task = engine_->get_task(task_id)) {
            params.push_back(notification_entry(task.get()));
        } else {
            params.push_back(json{{"gid", detail::task_id_to_gid(task_id)}});
        }

        JsonRpcServer* server = server_ptr();
        if (!server) return;
        server->broadcast_notification(method, params.dump());
    }

    void on_progress(const falcon::ProgressInfo& info) override {
        if (info.task_id == falcon::INVALID_TASK_ID) return;

        const auto now = std::chrono::steady_clock::now();
        JsonRpcServer* server = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!server_) return;
            // Falcon 扩展：进度按任务节流推送（aria2 无此通知，未知 method
            // 会被兼容客户端忽略）
            auto [it, inserted] =
                last_progress_push_.try_emplace(info.task_id, now);
            if (!inserted) {
                if (now - it->second < progress_interval_) return;
                it->second = now;
            }
            server = server_;
        }

        json params = json::array();
        json entry;
        entry["gid"] = detail::task_id_to_gid(info.task_id);
        if (auto task = engine_->get_task(info.task_id)) {
            entry["status"] = detail::aria2_status_from_task(*task);
        }
        entry["progress"] = info.progress;
        entry["totalLength"] = std::to_string(info.total_bytes);
        entry["completedLength"] = std::to_string(info.downloaded_bytes);
        entry["downloadSpeed"] = std::to_string(info.speed);
        params.push_back(std::move(entry));
        server->broadcast_notification("falcon.onProgress", params.dump());
    }

private:
    JsonRpcServer* server_ptr() {
        std::lock_guard<std::mutex> lock(mutex_);
        return server_;
    }

    std::mutex mutex_;
    JsonRpcServer* server_ = nullptr; // mutex_ 保护
    falcon::DownloadEngine* engine_ = nullptr;
    std::chrono::milliseconds progress_interval_{1000};
    std::unordered_map<falcon::TaskId, std::chrono::steady_clock::time_point>
        last_progress_push_;
};

JsonRpcServer::~JsonRpcServer() {
    stop();
}

struct JsonRpcServer::HttpRequest {
    std::string method;
    std::string path;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct JsonRpcServer::HttpResponse {
    int status_code = 200;
    std::string status_text = "OK";
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

// 构造/析构放在 RpcEventBridge 完整定义之后（unique_ptr 成员析构需要完整类型）
JsonRpcServer::JsonRpcServer(falcon::DownloadEngine* engine,
                               JsonRpcServerConfig config,
                               TaskStorage* storage)
    : engine_(engine),
      storage_(storage),
      config_(config),
      auth_secret_(config.secret),
      auth_allow_origin_all_(config.allow_origin_all),
      mcp_(std::make_unique<McpServer>()) {
    // config_ 仅作启动快照（监听地址/端口等不可热更项）；
    // secret/allow_origin_all 经 update_auth 热更，读取一律走带锁访问器
}

void JsonRpcServer::set_shutdown_handler(std::function<void()> handler) {
    shutdown_handler_ = std::move(handler);
}

void JsonRpcServer::set_swarm_status_handler(
    std::function<nlohmann::json()> handler) {
    swarm_status_handler_ = std::move(handler);
}

void JsonRpcServer::set_swarm_share_handler(
    std::function<nlohmann::json(bool)> handler) {
    swarm_share_handler_ = std::move(handler);
}

void JsonRpcServer::update_auth(std::string secret, bool allow_origin_all) {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    auth_secret_ = std::move(secret);
    auth_allow_origin_all_ = allow_origin_all;
}

std::string JsonRpcServer::auth_secret() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return auth_secret_;
}

bool JsonRpcServer::auth_allow_origin_all() const {
    std::lock_guard<std::mutex> lock(auth_mutex_);
    return auth_allow_origin_all_;
}

bool JsonRpcServer::start() {
    if (!engine_) {
        FALCON_LOG_ERROR_STREAM("JsonRpcServer start failed: engine is null");
        return false;
    }
    if (accept_thread_.joinable()) {
        return true;
    }

    stop_requested_ = false;
    ensure_winsock_started();

    listen_fd_ = ::falcon::detail::inject_failure(
                     ::falcon::detail::InjectPoint::RpcServerSocket)
                     ? -1
                     : ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        FALCON_LOG_ERROR_STREAM("socket() failed: " << std::strerror(errno));
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                 reinterpret_cast<const char*>(&yes),
#else
                 &yes,
#endif
                 sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.listen_port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
        FALCON_LOG_ERROR_STREAM("inet_pton() failed for bind_address=" << config_.bind_address);
        socket_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        FALCON_LOG_ERROR_STREAM("bind() failed: " << std::strerror(errno));
        socket_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // If port was 0, OS picks an ephemeral port; query the actual port.
    if (config_.listen_port == 0) {
        sockaddr_in bound{};
        socket_len_t bound_len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
            config_.listen_port = ntohs(bound.sin_port);
        }
    }

    if (::falcon::detail::inject_failure(
            ::falcon::detail::InjectPoint::RpcServerListen) ||
        ::listen(listen_fd_, 128) < 0) {
        FALCON_LOG_ERROR_STREAM("listen() failed: " << std::strerror(errno));
        socket_close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // 事件桥：引擎状态/进度 → WebSocket 通知广播（与 accept 线程同生命周期）
    event_bridge_ = std::make_unique<RpcEventBridge>(this, engine_);
    event_bridge_->set_progress_interval(config_.progress_push_interval);
    engine_->add_listener(event_bridge_.get());

    accept_thread_ = std::thread([this] { accept_loop(); });
    // 每次 start() 生成新的会话 id（aria2.getSessionInfo 返回值）
    {
        std::random_device rd;
        const std::uint64_t value =
            (static_cast<std::uint64_t>(rd()) << 32) | static_cast<std::uint64_t>(rd());
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << value;
        session_id_ = oss.str();
    }
    FALCON_LOG_INFO_STREAM("JSON-RPC server listening on " << config_.bind_address << ":" << config_.listen_port);
    return true;
}

void JsonRpcServer::stop() {
    stop_requested_ = true;

    // 先摘除事件桥：之后引擎事件不再进入广播路径
    if (event_bridge_) {
        engine_->remove_listener(event_bridge_.get());
        event_bridge_->detach();
    }

    // 唤醒阻塞在 recv 的 WebSocket 会话线程（长连接不会因 listen fd 关闭而断开）
    std::vector<int> ws_fds;
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        ws_fds.reserve(ws_clients_.size());
        for (const auto& [fd, state] : ws_clients_) {
            ws_fds.push_back(fd);
        }
    }
    for (int fd : ws_fds) {
        socket_shutdown(fd);
    }

    // MCP SSE 流会话同样阻塞在 recv，单独唤醒
    std::vector<int> sse_fds;
    {
        std::lock_guard<std::mutex> lock(sse_clients_mutex_);
        sse_fds.reserve(sse_clients_.size());
        for (const auto& [fd, state] : sse_clients_) {
            sse_fds.push_back(fd);
        }
    }
    for (int fd : sse_fds) {
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
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }

    // 会话线程已全部退出，注册表应已清空；兜底释放
    event_bridge_.reset();
}

void JsonRpcServer::accept_loop() {
    while (!stop_requested_.load()) {
        sockaddr_in client_addr{};
        socket_len_t len = sizeof(client_addr);
        int client_fd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len);
        if (client_fd < 0) {
            if (stop_requested_.load()) break;
            continue;
        }

        std::lock_guard<std::mutex> lock(worker_threads_mutex_);
        worker_threads_.emplace_back([this, client_fd] { handle_connection(client_fd); });
    }
}

static bool recv_into(int fd, std::string& buf, std::size_t want_at_least, std::size_t max_bytes) {
    while (buf.size() < want_at_least) {
        if (buf.size() >= max_bytes) {
            return false;
        }
        char tmp[4096];
        recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            return false;
        }
        buf.append(tmp, tmp + n);
    }
    return true;
}

static std::optional<JsonRpcServer::HttpRequest> read_http_request(int fd) {
    std::string buf;
    if (!recv_into(fd, buf, 1, 1024 * 1024)) {
        return std::nullopt;
    }

    const std::string sep = "\r\n\r\n";
    std::size_t header_end = buf.find(sep);
    while (header_end == std::string::npos) {
        if (!recv_into(fd, buf, buf.size() + 1, 1024 * 1024)) {
            return std::nullopt;
        }
        header_end = buf.find(sep);
    }

    const std::string header_part = buf.substr(0, header_end);
    std::size_t body_start = header_end + sep.size();

    std::istringstream iss(header_part);
    std::string request_line;
    if (!std::getline(iss, request_line)) {
        return std::nullopt;
    }
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    std::istringstream rl(request_line);
    JsonRpcServer::HttpRequest req;
    std::string version;
    rl >> req.method >> req.path >> version;
    if (req.method.empty() || req.path.empty()) {
        return std::nullopt;
    }

    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto pos = line.find(':');
        if (pos == std::string::npos) continue;
        std::string key = detail::to_lower(detail::trim(line.substr(0, pos)));
        std::string value = detail::trim(line.substr(pos + 1));
        req.headers[key] = value;
    }

    std::size_t content_length = 0;
    if (auto cl = parse_content_length(req.headers)) {
        content_length = *cl;
    }

    const std::size_t already = buf.size() - body_start;
    if (already < content_length) {
        if (!recv_into(fd, buf, body_start + content_length, 1024 * 1024 + content_length)) {
            return std::nullopt;
        }
    }

    req.body = buf.substr(body_start, content_length);
    return req;
}

static bool send_all(int fd, const std::string& data) {
    std::size_t off = 0;
    while (off < data.size()) {
        recv_send_size_t n = 0;
#ifdef _WIN32
        const auto remaining = static_cast<int>(data.size() - off);
        n = ::send(fd, data.data() + off, remaining, 0);
#else
        const auto remaining = data.size() - off;
        n = ::send(fd, data.data() + off, remaining, 0);
#endif
        if (n <= 0) {
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

static std::string format_http_response(const JsonRpcServer::HttpResponse& resp) {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << resp.status_code << " " << resp.status_text << "\r\n";
    for (const auto& [k, v] : resp.headers) {
        oss << k << ": " << v << "\r\n";
    }
    oss << "Content-Length: " << resp.body.size() << "\r\n\r\n";
    oss << resp.body;
    return oss.str();
}

void JsonRpcServer::handle_connection(int client_fd) {
    ScopedFd fd(client_fd);

    auto req = read_http_request(fd.fd);
    if (!req) {
        return;
    }

    // WebSocket 升级：接管连接进入事件流会话循环（aria2 兼容：与 HTTP
    // JSON-RPC 同端口同路径，如 ws://host:6800/jsonrpc）
    if (is_websocket_upgrade(*req)) {
        handle_websocket(fd.fd, *req);
        return;
    }

    // MCP SSE 通知流（GET /mcp）：接管连接进入流循环（阶段 2 增量 3）。
    // 与 WS 同端口不同路径；准入校验（403/401/404）在 handle_mcp_sse 内
    // 经 McpServer 完成，错误形状照常回发。
    if (config_.mcp_enabled && req->method == "GET" && req->path == "/mcp") {
        handle_mcp_sse(fd.fd, *req);
        return;
    }

    auto resp = handle_http_request(*req);
    const std::string out = format_http_response(resp);
    send_all(fd.fd, out);
}

bool JsonRpcServer::is_websocket_upgrade(const HttpRequest& req) const {
    if (req.method != "GET") return false;
    auto upgrade = req.headers.find("upgrade");
    if (upgrade == req.headers.end() ||
        detail::to_lower(upgrade->second).find("websocket") == std::string::npos) {
        return false;
    }
    auto key = req.headers.find("sec-websocket-key");
    return key != req.headers.end() && !key->second.empty();
}

bool JsonRpcServer::ws_send_frame(int client_fd, std::uint8_t opcode,
                                  const std::string& payload) {
    std::shared_ptr<StreamClientState> state;
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        auto it = ws_clients_.find(client_fd);
        if (it == ws_clients_.end()) return false;
        state = it->second;
    }
    std::lock_guard<std::mutex> send_lock(state->send_mutex);
    return send_all(client_fd, ws_encode_frame(opcode, payload));
}

void JsonRpcServer::handle_websocket(int client_fd, const HttpRequest& req) {
    ScopedFd fd(client_fd);

    // 仅 /jsonrpc 与 / 提供 WebSocket 升级（与 HTTP JSON-RPC 端点一致）
    if (req.path != "/" && req.path != "/jsonrpc") {
        HttpResponse resp;
        resp.status_code = 404;
        resp.status_text = "Not Found";
        resp.body = R"({"error":"not found"})";
        send_all(fd.fd, format_http_response(resp));
        return;
    }

    const std::string key = req.headers.at("sec-websocket-key");
    std::ostringstream oss;
    oss << "HTTP/1.1 101 Switching Protocols\r\n"
        << "Upgrade: websocket\r\n"
        << "Connection: Upgrade\r\n"
        << "Sec-WebSocket-Accept: " << ws_compute_accept_key(key) << "\r\n";
    if (auth_allow_origin_all()) {
        oss << "Access-Control-Allow-Origin: *\r\n";
    }
    oss << "\r\n";
    if (!send_all(fd.fd, oss.str())) {
        return;
    }

    // 注册为订阅者；连接 fd 的注销统一由本线程完成（广播失败只 shutdown
    // 唤醒本线程，避免跨线程 close 引发 fd 复用竞争）
    auto state = std::make_shared<StreamClientState>();
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        ws_clients_[fd.fd] = state;
    }

    WsFrameParser parser;
    bool disconnect = false;
    char tmp[4096];
    while (!stop_requested_.load() && !disconnect) {
        recv_send_size_t n = ::recv(fd.fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;

        parser.feed(tmp, static_cast<std::size_t>(n));
        if (parser.error()) {
            // 协议违规：按 RFC 6455 以 1002 关闭
            ws_send_frame(fd.fd, WS_OP_CLOSE,
                          std::string("\x03\xEA", 2));
            break;
        }

        for (const auto& msg : parser.pop_messages()) {
            switch (msg.opcode) {
                case WS_OP_TEXT:
                case WS_OP_BINARY:
                    // WS 上的 JSON-RPC 与 HTTP 走同一分发路径（含 token 校验）
                    if (!ws_send_frame(fd.fd, WS_OP_TEXT,
                                       handle_jsonrpc(msg.payload).body)) {
                        disconnect = true;
                    }
                    break;
                case WS_OP_PING:
                    if (!ws_send_frame(fd.fd, WS_OP_PONG, msg.payload)) {
                        disconnect = true;
                    }
                    break;
                case WS_OP_CLOSE:
                    // 回应关闭帧后结束会话
                    ws_send_frame(fd.fd, WS_OP_CLOSE, msg.payload);
                    disconnect = true;
                    break;
                default:
                    break;
            }
            if (disconnect) break;
        }
    }

    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        ws_clients_.erase(fd.fd);
    }
}

void JsonRpcServer::handle_mcp_sse(int client_fd, const HttpRequest& req) {
    ScopedFd fd(client_fd);

    // 准入校验经 McpServer（与 POST/DELETE 同语义：未配 secret 403 /
    // Bearer 失败 401 / 会话缺失或过期 404）；错误形状照常回发
    McpServer::Request mreq;
    mreq.method = req.method;
    for (const auto& [key, value] : req.headers)
        mreq.headers[key] = value;
    McpServer::Response gate =
        mcp_->validate_sse_request(mreq, auth_secret());
    if (gate.status_code != 200) {
        HttpResponse resp;
        resp.status_code = gate.status_code;
        resp.status_text = gate.status_text;
        resp.body = std::move(gate.body);
        for (auto& [key, value] : gate.headers)
            resp.headers[key] = std::move(value);
        resp.headers["Server"] = "falcon-daemon";
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Connection"] = "close";
        send_all(fd.fd, format_http_response(resp));
        return;
    }

    // 流式响应头（无 Content-Length；SSE 单向服务器→客户端）。注释行
    // 让客户端与测试在首个通知到达前即可确认流已开通。
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: text/event-stream\r\n"
        << "Cache-Control: no-cache\r\n";
    if (auth_allow_origin_all()) {
        oss << "Access-Control-Allow-Origin: *\r\n";
    }
    oss << "\r\n: falcon mcp stream\r\n\r\n";
    if (!send_all(fd.fd, oss.str())) {
        return;
    }

    // 注册为订阅者；连接 fd 的注销统一由本线程完成（广播失败只 shutdown
    // 唤醒本线程，避免跨线程 close 引发 fd 复用竞争）——与 WS 会话同一纪律
    auto state = std::make_shared<StreamClientState>();
    {
        std::lock_guard<std::mutex> lock(sse_clients_mutex_);
        sse_clients_[fd.fd] = state;
    }

    // SSE 是单向流：客户端协议上不发送数据。阻塞 recv 只为感知对端
    // 断开（EOF/ECONNRESET）与 stop()（shutdown 唤醒）；收到的任何
    // 字节忽略。
    char tmp[256];
    while (!stop_requested_.load()) {
        recv_send_size_t n = ::recv(fd.fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
    }

    {
        std::lock_guard<std::mutex> lock(sse_clients_mutex_);
        sse_clients_.erase(fd.fd);
    }
}

void JsonRpcServer::broadcast_notification(const std::string& method,
                                           const std::string& params_json) {
    // JSON-RPC 通知：无 id 字段
    std::string body;
    body.reserve(40 + method.size() + params_json.size());
    body += "{\"jsonrpc\":\"2.0\",\"method\":\"";
    body += method;
    body += "\",\"params\":";
    body += params_json;
    body += "}";

    const std::string frame = ws_encode_frame(WS_OP_TEXT, body);
    // SSE 帧：同一 JSON-RPC 通知信封装进 data 行
    const std::string sse_line = "data: " + body + "\n\n";

    // 快照后逐连接发送，避免持注册锁做 I/O
    std::vector<std::pair<int, std::shared_ptr<StreamClientState>>> snapshot;
    {
        std::lock_guard<std::mutex> lock(ws_clients_mutex_);
        snapshot.assign(ws_clients_.begin(), ws_clients_.end());
    }
    for (const auto& [fd, state] : snapshot) {
        std::lock_guard<std::mutex> send_lock(state->send_mutex);
        if (!send_all(fd, frame)) {
            // 发送失败说明对端已断：shutdown 唤醒阻塞在 recv 的会话线程，
            // 由其完成注销与 fd 关闭
            socket_shutdown(fd);
        }
    }

    std::vector<std::pair<int, std::shared_ptr<StreamClientState>>> sse_snapshot;
    {
        std::lock_guard<std::mutex> lock(sse_clients_mutex_);
        sse_snapshot.assign(sse_clients_.begin(), sse_clients_.end());
    }
    for (const auto& [fd, state] : sse_snapshot) {
        std::lock_guard<std::mutex> send_lock(state->send_mutex);
        if (!send_all(fd, sse_line)) {
            socket_shutdown(fd);
        }
    }
}

std::size_t JsonRpcServer::websocket_client_count() {
    std::lock_guard<std::mutex> lock(ws_clients_mutex_);
    return ws_clients_.size();
}

std::size_t JsonRpcServer::sse_client_count() {
    std::lock_guard<std::mutex> lock(sse_clients_mutex_);
    return sse_clients_.size();
}

JsonRpcServer::HttpResponse JsonRpcServer::handle_http_request(const HttpRequest& req) {
    HttpResponse resp;
    resp.headers["Server"] = "falcon-daemon";
    resp.headers["Content-Type"] = "application/json";
    resp.headers["Connection"] = "close";

    const bool allow_all = auth_allow_origin_all();
    if (allow_all) {
        resp.headers["Access-Control-Allow-Origin"] = "*";
        resp.headers["Access-Control-Allow-Methods"] = "POST, OPTIONS";
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
    }

    if (req.method == "OPTIONS") {
        resp.status_code = 204;
        resp.status_text = "No Content";
        resp.body.clear();
        return resp;
    }

    // MCP Streamable HTTP 端点（/mcp）：mcp_enabled 启动期配置只读，
    // 无需加锁。DELETE 也由该端点消费，故在非 POST 判定之前分流。
    if (req.path == "/mcp") {
        if (!config_.mcp_enabled) {
            resp.status_code = 404;
            resp.status_text = "Not Found";
            resp.body = R"({"error":"not found"})";
            return resp;
        }
        McpServer::Request mreq;
        mreq.method = req.method;
        mreq.body = req.body;
        for (const auto& [key, value] : req.headers)
            mreq.headers[key] = value;
        McpServer::Response mresp = mcp_->handle_request(
            mreq, auth_secret(),
            // 异常边界对齐 handle_jsonrpc：dispatch 内部运行时失败（如
            // 协议无 handler 抛 UnsupportedProtocolException）以 -32603
            // 返回，绝不让异常逃进连接线程（terminate 整个进程）。
            [this](const std::string& m, nlohmann::json p) -> nlohmann::json {
                try {
                    return dispatch_rpc(m, std::move(p));
                } catch (const std::exception& e) {
                    return {{"error", {{"code", -32603},
                                       {"message",
                                        std::string("Internal error: ") + e.what()}}}};
                }
            });
        resp.status_code = mresp.status_code;
        resp.status_text = mresp.status_text;
        resp.body = std::move(mresp.body);
        for (auto& [key, value] : mresp.headers)
            resp.headers[key] = std::move(value);
        if (allow_all) {
            resp.headers["Access-Control-Allow-Origin"] = "*";
            resp.headers["Access-Control-Allow-Methods"] = "POST, OPTIONS";
            resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
        }
        resp.headers["Server"] = "falcon-daemon";
        resp.headers["Content-Type"] = "application/json";
        resp.headers["Connection"] = "close";
        return resp;
    }

    if (req.method != "POST") {
        resp.status_code = 405;
        resp.status_text = "Method Not Allowed";
        resp.body = R"({"error":"method not allowed"})";
        return resp;
    }

    if (req.path != "/" && req.path != "/jsonrpc") {
        resp.status_code = 404;
        resp.status_text = "Not Found";
        resp.body = R"({"error":"not found"})";
        return resp;
    }

    resp = handle_jsonrpc(req.body);
    if (allow_all) {
        resp.headers["Access-Control-Allow-Origin"] = "*";
        resp.headers["Access-Control-Allow-Methods"] = "POST, OPTIONS";
        resp.headers["Access-Control-Allow-Headers"] = "Content-Type";
    }
    resp.headers["Server"] = "falcon-daemon";
    resp.headers["Content-Type"] = "application/json";
    resp.headers["Connection"] = "close";
    return resp;
}



JsonRpcServer::HttpResponse JsonRpcServer::handle_jsonrpc(const std::string& body) {
    HttpResponse resp;
    resp.status_code = 200;
    resp.status_text = "OK";

    // 认证配置可被 SIGHUP 重载热更：请求内固定一份快照，保证一致性
    const std::string auth_secret = this->auth_secret();

    json id = nullptr;
    try {
        auto req = json::parse(body);
        if (!req.is_object()) {
            resp.body = detail::make_error(id, -32600, "Invalid Request").dump();
            return resp;
        }

        id = req.value("id", json(nullptr));
        const std::string method = req.value("method", "");
        json params = req.value("params", json::array());

        if (method.empty() || (!params.is_array() && !params.is_object())) {
            resp.body = detail::make_error(id, -32600, "Invalid Request").dump();
            return resp;
        }

        // aria2-style authentication: first param "token:<secret>"
        if (!detail::validate_and_strip_token(params, auth_secret)) {
            resp.body = detail::make_error(id, -32001, "Unauthorized").dump();
            return resp;
        }


        json result = dispatch_rpc(method, params);
        if (result.is_object() && result.contains("error")) {
            resp.body = detail::make_error(id, result["error"].value("code", -32000),
                                   result["error"].value("message", "Error"))
                            .dump();
            return resp;
        }

        resp.body = detail::make_result(id, result).dump();
        return resp;
    } catch (const json::parse_error& e) {
        // -32700 专属于请求体不是合法 JSON 的情形
        resp.body = detail::make_error(id, -32700, std::string("Parse error: ") + e.what()).dump();
        return resp;
    } catch (const std::exception& e) {
        // dispatch 内部的运行时失败（如输出目录不可写）是服务端错误，不是解析错误
        resp.body = detail::make_error(id, -32603, std::string("Internal error: ") + e.what()).dump();
        return resp;
    }
}

} // namespace falcon::daemon::rpc
