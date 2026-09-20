#include "json_rpc_server.hpp"

#include "rpc/websocket_frame.hpp"

#ifdef FALCON_HAS_SQLITE3
#include "storage/task_storage.hpp"
#endif

#include <falcon/detail/injection.hpp>
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

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string trim(std::string s) {
    auto is_ws = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

static std::optional<std::size_t> parse_content_length(const std::unordered_map<std::string, std::string>& headers) {
    auto it = headers.find("content-length");
    if (it == headers.end()) return std::nullopt;
    try {
        return static_cast<std::size_t>(std::stoull(it->second));
    } catch (...) {
        return std::nullopt;
    }
}

static std::string task_id_to_gid(falcon::TaskId id) {
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << static_cast<std::uint64_t>(id);
    return oss.str();
}

static std::optional<falcon::TaskId> gid_to_task_id(const std::string& gid) {
    std::string s = gid;
    if (s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0) {
        s = s.substr(2);
    }
    if (s.empty() || s.size() > 16) return std::nullopt;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return std::nullopt;
    }
    try {
        std::uint64_t value = std::stoull(s, nullptr, 16);
        if (value == 0) return std::nullopt;
        return static_cast<falcon::TaskId>(value);
    } catch (...) {
        return std::nullopt;
    }
}

static std::string aria2_status_from_task_status(falcon::TaskStatus status) {
    switch (status) {
        case falcon::TaskStatus::Downloading:
        case falcon::TaskStatus::Preparing:
            return "active";
        case falcon::TaskStatus::Pending:
            return "waiting";
        case falcon::TaskStatus::Paused:
            return "paused";
        case falcon::TaskStatus::Completed:
            return "complete";
        case falcon::TaskStatus::Cancelled:
            return "removed";
        case falcon::TaskStatus::Failed:
            return "error";
    }
    return "error";
}

static std::string aria2_status_from_task(const falcon::DownloadTask& task) {
    return aria2_status_from_task_status(task.status());
}

// aria2 风格的文件条目（tellStatus.files / getFiles 共用）
static json files_json(const std::string& path, falcon::Bytes total,
                       falcon::Bytes completed, const std::string& url,
                       bool include_selected) {
    json file{
        {"path", path},
        {"length", std::to_string(total)},
        {"completedLength", std::to_string(completed)},
        {"uris", json::array({json{{"uri", url}, {"status", "used"}}})},
    };
    if (include_selected) {
        file["selected"] = "true";
    }
    return json::array({std::move(file)});
}

// 将任务的下载选项映射回 aria2 选项键（getOption / 便于客户端回显）
static json download_options_to_json(const falcon::DownloadOptions& options) {
    json out;
    out["dir"] = options.output_directory;
    out["out"] = options.output_filename;
    out["user-agent"] = options.user_agent;
    out["referer"] = options.referer;
    out["load-cookies"] = options.cookie_file;
    out["save-cookies"] = options.cookie_jar;
    out["http-user"] = options.http_username;
    out["http-passwd"] = options.http_password;
    out["certificate"] = options.client_certificate;
    out["private-key"] = options.client_private_key;
    out["all-proxy"] = options.proxy;
    out["all-proxy-user"] = options.proxy_username;
    out["all-proxy-passwd"] = options.proxy_password;
    out["check-certificate"] = options.verify_ssl ? "true" : "false";
    out["max-tries"] = std::to_string(options.max_retries);
    out["retry-wait"] = std::to_string(options.retry_delay_seconds);
    out["max-connection-per-server"] = std::to_string(options.max_connections);
    out["max-download-limit"] = std::to_string(options.speed_limit);
    json headers = json::object();
    for (const auto& [key, value] : options.headers) {
        headers[key] = value;
    }
    out["header"] = std::move(headers);
    return out;
}

// aria2 tellWaiting/tellStopped 的 offset/num 分页语义
static json paginate_items(const std::vector<json>& items, int offset, int num) {
    json out = json::array();
    const int start = std::max(0, offset);
    const int end = std::min<int>(static_cast<int>(items.size()), start + std::max(0, num));
    for (int i = start; i < end; ++i) {
        out.push_back(items[static_cast<std::size_t>(i)]);
    }
    return out;
}

static json task_to_status_json(const falcon::DownloadTask& task) {
    json out;
    out["gid"] = task_id_to_gid(task.id());
    out["status"] = aria2_status_from_task(task);
    // Falcon 扩展字段：aria2 原生没有优先级，桌面客户端用它还原任务列表
    out["priority"] = static_cast<int>(task.get_priority());
    out["totalLength"] = std::to_string(task.total_bytes());
    out["completedLength"] = std::to_string(task.downloaded_bytes());
    out["downloadSpeed"] = std::to_string(task.speed());
    out["errorMessage"] = task.error_message();
    out["files"] = files_json(task.output_path(), task.total_bytes(),
                              task.downloaded_bytes(), task.url(), false);
    return out;
}

#ifdef FALCON_HAS_SQLITE3
// 引擎内任务（内存态）转状态 JSON；历史记录转状态 JSON
static json task_record_to_status_json(const TaskRecord& record) {
    json out;
    out["gid"] = task_id_to_gid(record.id);
    out["status"] = aria2_status_from_task_status(record.status);
    // storage 未持久化优先级，历史记录回默认 Normal
    out["priority"] = static_cast<int>(falcon::TaskPriority::Normal);
    out["totalLength"] = std::to_string(record.total_bytes);
    out["completedLength"] = std::to_string(record.downloaded_bytes);
    out["downloadSpeed"] = std::to_string(record.speed);
    out["errorMessage"] = record.error_message;
    out["files"] = files_json(record.output_path, record.total_bytes,
                              record.downloaded_bytes, record.url, false);
    return out;
}

// 收集 storage 中存在、但引擎当前不持有的记录（典型场景：重启后
// 终态任务只保留在数据库中，引擎恢复时跳过终态）。引擎内任务以
// 内存状态为准，通过 engine_ids 去重避免同一任务重复出现。
static std::vector<TaskRecord> storage_extra_records(
    TaskStorage* storage,
    const std::vector<falcon::DownloadTask::Ptr>& engine_tasks,
    const std::vector<falcon::TaskStatus>& statuses) {
    std::unordered_set<falcon::TaskId> engine_ids;
    for (const auto& task : engine_tasks) {
        if (task) engine_ids.insert(task->id());
    }
    std::vector<TaskRecord> out;
    for (auto status : statuses) {
        for (auto& record : storage->get_tasks_by_status(status)) {
            if (engine_ids.count(record.id) == 0) {
                out.push_back(std::move(record));
            }
        }
    }
    return out;
}
#endif

static json make_error(const json& id, int code, std::string message) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id.is_null() ? json(nullptr) : id},
        {"error", json{{"code", code}, {"message", std::move(message)}}},
    };
}

static json make_result(const json& id, json result) {
    return json{
        {"jsonrpc", "2.0"},
        {"id", id.is_null() ? json(nullptr) : id},
        {"result", std::move(result)},
    };
}

static bool validate_and_strip_token(json& params, const std::string& secret) {
    if (secret.empty()) {
        return true;
    }
    if (!params.is_array() || params.empty() || !params[0].is_string()) {
        return false;
    }
    const std::string token = params[0].get<std::string>();
    const std::string expected = "token:" + secret;
    if (token != expected) {
        return false;
    }
    params.erase(params.begin());
    return true;
}

static void maybe_strip_token(json& params, const std::string& secret) {
    if (secret.empty()) return;
    if (!params.is_array() || params.empty() || !params[0].is_string()) return;
    const std::string token = params[0].get<std::string>();
    const std::string expected = "token:" + secret;
    if (token == expected) {
        params.erase(params.begin());
    }
}

// ---------------------------------------------------------------------------
// WebSocket 订阅者状态与事件桥
// ---------------------------------------------------------------------------

} // namespace

/// 每连接写互斥。广播线程与该连接的会话线程经 shared_ptr 共享，
/// 保证注销后仍在途的广播发送可以安全完成。
struct WsClientState {
    std::mutex send_mutex;
};

namespace {

/// 通知 params[0]：gid + Falcon 扩展进度快照（aria2 原生通知只带 gid，
/// 额外字段对严格客户端是透明超集）
static json notification_entry(const falcon::DownloadTask* task) {
    json entry;
    entry["gid"] = task_id_to_gid(task->id());
    entry["status"] = aria2_status_from_task(*task);
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
            params.push_back(json{{"gid", task_id_to_gid(task_id)}});
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
        entry["gid"] = task_id_to_gid(info.task_id);
        if (auto task = engine_->get_task(info.task_id)) {
            entry["status"] = aria2_status_from_task(*task);
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
      auth_allow_origin_all_(config.allow_origin_all) {
    // config_ 仅作启动快照（监听地址/端口等不可热更项）；
    // secret/allow_origin_all 经 update_auth 热更，读取一律走带锁访问器
}

void JsonRpcServer::set_shutdown_handler(std::function<void()> handler) {
    shutdown_handler_ = std::move(handler);
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
        std::string key = to_lower(trim(line.substr(0, pos)));
        std::string value = trim(line.substr(pos + 1));
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

    auto resp = handle_http_request(*req);
    const std::string out = format_http_response(resp);
    send_all(fd.fd, out);
}

bool JsonRpcServer::is_websocket_upgrade(const HttpRequest& req) const {
    if (req.method != "GET") return false;
    auto upgrade = req.headers.find("upgrade");
    if (upgrade == req.headers.end() ||
        to_lower(upgrade->second).find("websocket") == std::string::npos) {
        return false;
    }
    auto key = req.headers.find("sec-websocket-key");
    return key != req.headers.end() && !key->second.empty();
}

bool JsonRpcServer::ws_send_frame(int client_fd, std::uint8_t opcode,
                                  const std::string& payload) {
    std::shared_ptr<WsClientState> state;
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
    auto state = std::make_shared<WsClientState>();
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

    // 快照后逐连接发送，避免持注册锁做 I/O
    std::vector<std::pair<int, std::shared_ptr<WsClientState>>> snapshot;
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
}

std::size_t JsonRpcServer::websocket_client_count() {
    std::lock_guard<std::mutex> lock(ws_clients_mutex_);
    return ws_clients_.size();
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
            resp.body = make_error(id, -32600, "Invalid Request").dump();
            return resp;
        }

        id = req.value("id", json(nullptr));
        const std::string method = req.value("method", "");
        json params = req.value("params", json::array());

        if (method.empty() || (!params.is_array() && !params.is_object())) {
            resp.body = make_error(id, -32600, "Invalid Request").dump();
            return resp;
        }

        // aria2-style authentication: first param "token:<secret>"
        if (!validate_and_strip_token(params, auth_secret)) {
            resp.body = make_error(id, -32001, "Unauthorized").dump();
            return resp;
        }

        std::function<json(const std::string&, json)> dispatch;
        dispatch = [&](const std::string& m, json p) -> json {
            // For system.multicall inner calls, some clients may redundantly include token again.
            maybe_strip_token(p, auth_secret);

            if (m == "system.listMethods") {
                return json::array({
                    "aria2.addUri",
                    "aria2.pause",
                    "aria2.pauseAll",
                    "aria2.forcePause",
                    "aria2.unpause",
                    "aria2.unpauseAll",
                    "aria2.remove",
                    "aria2.forceRemove",
                    "aria2.forceShutdown",
                    "aria2.tellStatus",
                    "aria2.tellActive",
                    "aria2.tellWaiting",
                    "aria2.tellStopped",
                    "aria2.getFiles",
                    "aria2.getUris",
                    "aria2.getOption",
                    "aria2.getGlobalOption",
                    "aria2.changeGlobalOption",
                    "aria2.changePriority",
                    "aria2.getGlobalStat",
                    "aria2.getVersion",
                    "aria2.getSessionInfo",
                    "aria2.saveSession",
                    "aria2.purgeDownloadResult",
                    "aria2.removeDownloadResult",
                    "system.listMethods",
                    "system.multicall",
                });
            }

            if (m == "system.multicall") {
                if (!p.is_array() || p.empty() || !p[0].is_array()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                json results = json::array();
                for (const auto& call : p[0]) {
                    // 非对象或缺失字符串 methodName 的条目不是合法调用（JSON-RPC 2.0）
                    if (!call.is_object() || !call.contains("methodName") ||
                        !call["methodName"].is_string()) {
                        results.push_back(json{{"code", -32600}, {"message", "Invalid Request"}});
                        continue;
                    }
                    const std::string cm = call.value("methodName", "");
                    json cp = call.value("params", json::array());
                    json r = dispatch(cm, cp);
                    if (r.is_object() && r.contains("error")) {
                        results.push_back(r["error"]);
                    } else {
                        results.push_back(json::array({r}));
                    }
                }
                return results;
            }

            if (m == "aria2.getVersion") {
                json out;
                out["version"] = "0.1.0";
                out["enabledFeatures"] = json::array({"jsonrpc", "asyncdns", "https"});
                return out;
            }

            if (m == "aria2.getSessionInfo") {
                return json{{"sessionId", session_id_}};
            }

            // 会话通过 TaskStorage 持续落库，无需显式快照；
            // 接受 aria2 的 filePath 参数但忽略
            if (m == "aria2.saveSession") {
                return "OK";
            }

            if (m == "aria2.pauseAll" || m == "aria2.unpauseAll") {
#ifdef FALCON_HAS_SQLITE3
                std::vector<falcon::TaskId> affected;
                if (storage_) {
                    // pause 对 Downloading 任务经 handler 异步生效，回读有竞态；
                    // 先收集受影响任务，操作后按 aria2 语义直接落目标状态
                    if (m == "aria2.pauseAll") {
                        for (const auto& t : engine_->get_active_tasks()) {
                            if (t) affected.push_back(t->id());
                        }
                    } else {
                        for (const auto& t :
                             engine_->get_tasks_by_status(falcon::TaskStatus::Paused)) {
                            if (t) affected.push_back(t->id());
                        }
                    }
                }
#endif
                if (m == "aria2.pauseAll") {
                    engine_->pause_all();
                } else {
                    engine_->resume_all();
                }
#ifdef FALCON_HAS_SQLITE3
                if (storage_) {
                    if (m == "aria2.pauseAll") {
                        for (auto id : affected) {
                            storage_->update_status(id, TaskStatus::Paused);
                        }
                    } else {
                        // 恢复经任务队列重新入队，RPC 返回时通常为 Pending，
                        // 后续状态变化由持久化监听器异步修正
                        for (auto id : affected) {
                            if (auto t = engine_->get_task(id)) {
                                storage_->update_status(id, t->status());
                            }
                        }
                    }
                }
#endif
                return "OK";
            }

            if (m == "aria2.forceShutdown" || m == "aria2.shutdown") {
                FALCON_LOG_WARN_STREAM("Daemon shutdown requested via JSON-RPC (" << m << ")");
                if (shutdown_handler_) {
                    shutdown_handler_();
                } else {
                    FALCON_LOG_WARN_STREAM("No shutdown handler registered; ignoring " << m);
                }
                return "OK";
            }

            if (m == "aria2.purgeDownloadResult") {
                engine_->remove_finished_tasks();
#ifdef FALCON_HAS_SQLITE3
                if (storage_) {
                    for (auto status : {falcon::TaskStatus::Completed, falcon::TaskStatus::Failed,
                                        falcon::TaskStatus::Cancelled}) {
                        for (const auto& record : storage_->get_tasks_by_status(status)) {
                            storage_->delete_task(record.id);
                        }
                    }
                }
#endif
                return "OK";
            }

            if (m == "aria2.getGlobalOption") {
                json out;
                out["max-overall-download-limit"] = std::to_string(engine_->get_global_speed_limit());
                out["max-concurrent-downloads"] = std::to_string(engine_->get_max_concurrent_tasks());
                // daemon 没有全局默认下载目录的概念（addUri 逐任务指定），保持 aria2 键位存在
                out["dir"] = "";
                return out;
            }

            if (m == "aria2.changePriority") {
                // p = [gid, priority]；priority 用 falcon::TaskPriority 的数值
                // （0=Low 1=Normal 2=High 3=Critical），与 aria2 原生语义不同，
                // 这是 Falcon 客户端约定
                if (!p.is_array() || p.size() < 2 || !p[0].is_string() ||
                    (!p[1].is_number_integer() && !p[1].is_string())) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                auto tid = gid_to_task_id(p[0].get<std::string>());
                if (!tid) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                int value = -1;
                try {
                    value = p[1].is_number_integer()
                                ? p[1].get<int>()
                                : std::stoi(p[1].get<std::string>());
                } catch (const std::exception&) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid priority"}}}};
                }
                if (value < 0 || value > 3) {
                    return json{{"error", json{{"code", 1},
                                               {"message", "Priority must be 0..3"}}}};
                }
                auto task = engine_->get_task(*tid);
                if (!task) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }
                engine_->adjust_task_priority(
                    *tid, static_cast<falcon::TaskPriority>(value));
                return task_id_to_gid(*tid);
            }

            if (m == "aria2.changeGlobalOption") {
                // aria2 签名是 (secret, options)：token 剥离后 options 就在 p[0]
                if (!p.is_array() || p.empty() || !p[0].is_object()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                json result;
                for (const auto& [key, value] : p[0].items()) {
                    try {
                        if (key == "max-overall-download-limit") {
                            const std::string v = value.is_string()
                                                      ? value.get<std::string>()
                                                      : value.dump();
                            falcon::BytesPerSecond limit = 0;
                            if (v != "none" && v != "0") {
                                limit = static_cast<falcon::BytesPerSecond>(std::stoull(v));
                            }
                            engine_->set_global_speed_limit(limit);
                        } else if (key == "max-concurrent-downloads") {
                            const std::size_t n = value.is_number_integer()
                                                      ? static_cast<std::size_t>(value.get<int>())
                                                      : std::stoull(value.get<std::string>());
                            engine_->set_max_concurrent_tasks(n);
                        } else {
                            return json{{"error", json{{"code", 1},
                                                       {"message", "Option not supported: " + key}}}};
                        }
                    } catch (const std::exception&) {
                        return json{{"error", json{{"code", 1},
                                                   {"message", "Invalid option value: " + key}}}};
                    }
                }
                return "OK";
            }

            if (m == "aria2.getGlobalStat") {
                auto tasks = engine_->get_all_tasks();
                std::size_t active = 0, waiting = 0, stopped = 0;
                for (const auto& t : tasks) {
                    if (!t) continue;
                    switch (t->status()) {
                        case falcon::TaskStatus::Downloading:
                        case falcon::TaskStatus::Preparing:
                            active++;
                            break;
                        case falcon::TaskStatus::Pending:
                        case falcon::TaskStatus::Paused:
                            waiting++;
                            break;
                        case falcon::TaskStatus::Completed:
                        case falcon::TaskStatus::Failed:
                        case falcon::TaskStatus::Cancelled:
                            stopped++;
                            break;
                    }
                }

#ifdef FALCON_HAS_SQLITE3
                // 重启后终态历史只存在于 storage（引擎恢复时跳过终态），
                // 计数需合并 storage 独有记录，与 tell* 视图保持一致
                if (storage_) {
                    waiting += storage_extra_records(
                        storage_, tasks, {falcon::TaskStatus::Pending, falcon::TaskStatus::Paused}).size();
                    stopped += storage_extra_records(
                        storage_, tasks,
                        {falcon::TaskStatus::Completed, falcon::TaskStatus::Failed,
                         falcon::TaskStatus::Cancelled}).size();
                }
#endif

                json out;
                out["downloadSpeed"] = std::to_string(engine_->get_total_speed());
                out["uploadSpeed"] = "0";
                out["numActive"] = std::to_string(active);
                out["numWaiting"] = std::to_string(waiting);
                out["numStopped"] = std::to_string(stopped);
                out["numStoppedTotal"] = std::to_string(stopped);
                return out;
            }

            if (m == "aria2.addUri") {
                if (!p.is_array() || p.empty() || !p[0].is_array() || p[0].empty()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                const auto& uris = p[0];
                const std::string url = uris[0].get<std::string>();

                falcon::DownloadOptions options;
                if (p.size() >= 2 && p[1].is_object()) {
                    const auto& o = p[1];
                    if (o.contains("dir") && o["dir"].is_string()) options.output_directory = o["dir"].get<std::string>();
                    if (o.contains("out") && o["out"].is_string()) options.output_filename = o["out"].get<std::string>();
                    if (o.contains("user-agent") && o["user-agent"].is_string()) options.user_agent = o["user-agent"].get<std::string>();
                    if (o.contains("referer") && o["referer"].is_string()) options.referer = o["referer"].get<std::string>();
                    if (o.contains("load-cookies") && o["load-cookies"].is_string()) options.cookie_file = o["load-cookies"].get<std::string>();
                    if (o.contains("save-cookies") && o["save-cookies"].is_string()) options.cookie_jar = o["save-cookies"].get<std::string>();
                    if (o.contains("http-user") && o["http-user"].is_string()) options.http_username = o["http-user"].get<std::string>();
                    if (o.contains("http-passwd") && o["http-passwd"].is_string()) options.http_password = o["http-passwd"].get<std::string>();
                    if (o.contains("certificate") && o["certificate"].is_string()) options.client_certificate = o["certificate"].get<std::string>();
                    if (o.contains("private-key") && o["private-key"].is_string()) options.client_private_key = o["private-key"].get<std::string>();
                    if (o.contains("all-proxy") && o["all-proxy"].is_string()) options.proxy = o["all-proxy"].get<std::string>();
                    if (o.contains("all-proxy-user") && o["all-proxy-user"].is_string()) options.proxy_username = o["all-proxy-user"].get<std::string>();
                    if (o.contains("all-proxy-passwd") && o["all-proxy-passwd"].is_string()) options.proxy_password = o["all-proxy-passwd"].get<std::string>();

                    if (o.contains("check-certificate")) {
                        if (o["check-certificate"].is_boolean()) {
                            options.verify_ssl = o["check-certificate"].get<bool>();
                        } else if (o["check-certificate"].is_string()) {
                            options.verify_ssl = (o["check-certificate"].get<std::string>() != "false");
                        }
                    }

                    if (o.contains("max-tries")) {
                        if (o["max-tries"].is_string()) options.max_retries = static_cast<std::size_t>(std::stoull(o["max-tries"].get<std::string>()));
                        if (o["max-tries"].is_number_integer()) options.max_retries = static_cast<std::size_t>(o["max-tries"].get<int>());
                    }
                    if (o.contains("retry-wait")) {
                        if (o["retry-wait"].is_string()) options.retry_delay_seconds = static_cast<std::size_t>(std::stoull(o["retry-wait"].get<std::string>()));
                        if (o["retry-wait"].is_number_integer()) options.retry_delay_seconds = static_cast<std::size_t>(o["retry-wait"].get<int>());
                    }
                    if (o.contains("max-connection-per-server")) {
                        if (o["max-connection-per-server"].is_string()) options.max_connections = static_cast<std::size_t>(std::stoull(o["max-connection-per-server"].get<std::string>()));
                        if (o["max-connection-per-server"].is_number_integer()) options.max_connections = static_cast<std::size_t>(o["max-connection-per-server"].get<int>());
                    }
                    if (o.contains("max-download-limit")) {
                        if (o["max-download-limit"].is_string()) options.speed_limit = static_cast<falcon::BytesPerSecond>(std::stoull(o["max-download-limit"].get<std::string>()));
                        if (o["max-download-limit"].is_number_integer()) options.speed_limit = static_cast<falcon::BytesPerSecond>(o["max-download-limit"].get<std::uint64_t>());
                    }

                    if (o.contains("header")) {
                        if (o["header"].is_array()) {
                            for (const auto& hv : o["header"]) {
                                if (!hv.is_string()) continue;
                                const std::string h = hv.get<std::string>();
                                auto pos = h.find(':');
                                if (pos == std::string::npos) continue;
                                options.headers[trim(h.substr(0, pos))] = trim(h.substr(pos + 1));
                            }
                        } else if (o["header"].is_string()) {
                            const std::string h = o["header"].get<std::string>();
                            auto pos = h.find(':');
                            if (pos != std::string::npos) {
                                options.headers[trim(h.substr(0, pos))] = trim(h.substr(pos + 1));
                            }
                        }
                    }
                }

                auto task = engine_->add_task(url, options);
                if (!task) {
                    return json{{"error", json{{"code", 1}, {"message", "Unsupported URL"}}}};
                }

#ifdef FALCON_HAS_SQLITE3
                // Persist task to database if storage is available
                if (storage_) {
                    TaskRecord record;
                    record.id = task->id();
                    record.url = task->url();
                    record.output_path = task->output_path();
                    record.status = task->status();
                    record.progress = task->progress();
                    record.total_bytes = task->total_bytes();
                    record.downloaded_bytes = task->downloaded_bytes();
                    record.speed = task->speed();
                    record.error_message = task->error_message();
                    record.options = options;
                    record.created_at = std::chrono::system_clock::now();
                    record.updated_at = std::chrono::system_clock::now();

                    storage_->create_task(record);
                }
#endif

                engine_->start_task(task->id());
                return task_id_to_gid(task->id());
            }

            if (m == "aria2.tellStatus") {
                if (!p.is_array() || p.empty() || !p[0].is_string()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                auto tid = gid_to_task_id(p[0].get<std::string>());
                if (!tid) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }
                // 引擎内存态优先；引擎查不到时（重启后终态历史）回落到 storage
                if (auto task = engine_->get_task(*tid)) {
                    return task_to_status_json(*task);
                }
#ifdef FALCON_HAS_SQLITE3
                if (storage_) {
                    if (auto record = storage_->get_task(*tid)) {
                        return task_record_to_status_json(*record);
                    }
                }
#endif
                return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
            }

            if (m == "aria2.getFiles" || m == "aria2.getUris" || m == "aria2.getOption") {
                if (!p.is_array() || p.empty() || !p[0].is_string()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                auto tid = gid_to_task_id(p[0].get<std::string>());
                if (!tid) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }

                auto task = engine_->get_task(*tid);
#ifdef FALCON_HAS_SQLITE3
                std::optional<TaskRecord> record;
                if (!task && storage_) {
                    record = storage_->get_task(*tid);
                }
                if (!task && !record) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }
                if (m == "aria2.getFiles") {
                    if (task) {
                        return files_json(task->output_path(), task->total_bytes(),
                                          task->downloaded_bytes(), task->url(), true);
                    }
                    return files_json(record->output_path, record->total_bytes,
                                      record->downloaded_bytes, record->url, true);
                }
                if (m == "aria2.getUris") {
                    const std::string& uri = task ? task->url() : record->url;
                    return json::array({json{{"uri", uri}, {"status", "used"}}});
                }
                return download_options_to_json(task ? task->options() : record->options);
#else
                if (!task) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }
                if (m == "aria2.getFiles") {
                    return files_json(task->output_path(), task->total_bytes(),
                                      task->downloaded_bytes(), task->url(), true);
                }
                if (m == "aria2.getUris") {
                    return json::array({json{{"uri", task->url()}, {"status", "used"}}});
                }
                return download_options_to_json(task->options());
#endif
            }

            if (m == "aria2.pause" || m == "aria2.forcePause" ||
                m == "aria2.unpause" ||
                m == "aria2.remove" || m == "aria2.forceRemove") {
                if (!p.is_array() || p.empty() || !p[0].is_string()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                const std::string gid = p[0].get<std::string>();
                auto tid = gid_to_task_id(gid);
                if (!tid) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }
                auto task = engine_->get_task(*tid);
                if (!task) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }

                if (m == "aria2.pause" || m == "aria2.forcePause") {
                    if (!engine_->pause_task(*tid)) {
                        return json{{"error", json{{"code", 1}, {"message", "Pause failed"}}}};
                    }
#ifdef FALCON_HAS_SQLITE3
                    // Update storage
                    if (storage_) storage_->update_status(*tid, TaskStatus::Paused);
#endif
                    return gid;
                }
                if (m == "aria2.unpause") {
                    if (!engine_->resume_task(*tid)) {
                        return json{{"error", json{{"code", 1}, {"message", "Resume failed"}}}};
                    }
#ifdef FALCON_HAS_SQLITE3
                    // Update storage - task will be in Downloading state
                    if (storage_) {
                        auto current_task = engine_->get_task(*tid);
                        if (current_task) {
                            storage_->update_status(*tid, current_task->status());
                        }
                    }
#endif
                    return gid;
                }
                // aria2.remove / aria2.forceRemove
                if (!engine_->cancel_task(*tid)) {
                    return json{{"error", json{{"code", 1}, {"message", "Remove failed"}}}};
                }
#ifdef FALCON_HAS_SQLITE3
                // Update storage
                if (storage_) storage_->update_status(*tid, TaskStatus::Cancelled);
#endif
                return gid;
            }

            if (m == "aria2.tellActive") {
                json out = json::array();
                for (const auto& t : engine_->get_active_tasks()) {
                    if (!t) continue;
                    out.push_back(task_to_status_json(*t));
                }
                return out;
            }

            if (m == "aria2.tellWaiting") {
                if (!p.is_array() || p.size() < 2) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                const int offset = p[0].is_number_integer() ? p[0].get<int>() : std::stoi(p[0].get<std::string>());
                const int num = p[1].is_number_integer() ? p[1].get<int>() : std::stoi(p[1].get<std::string>());

                std::vector<json> items;
                auto tasks = engine_->get_all_tasks();
                for (const auto& t : tasks) {
                    if (!t) continue;
                    if (t->status() == falcon::TaskStatus::Pending ||
                        t->status() == falcon::TaskStatus::Paused) {
                        items.push_back(task_to_status_json(*t));
                    }
                }
#ifdef FALCON_HAS_SQLITE3
                if (storage_) {
                    for (const auto& record : storage_extra_records(
                             storage_, tasks,
                             {falcon::TaskStatus::Pending, falcon::TaskStatus::Paused})) {
                        items.push_back(task_record_to_status_json(record));
                    }
                }
#endif
                return paginate_items(items, offset, num);
            }

            if (m == "aria2.tellStopped") {
                if (!p.is_array() || p.size() < 2) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                const int offset = p[0].is_number_integer() ? p[0].get<int>() : std::stoi(p[0].get<std::string>());
                const int num = p[1].is_number_integer() ? p[1].get<int>() : std::stoi(p[1].get<std::string>());

                std::vector<json> items;
                auto tasks = engine_->get_all_tasks();
                for (const auto& t : tasks) {
                    if (!t) continue;
                    if (t->status() == falcon::TaskStatus::Completed ||
                        t->status() == falcon::TaskStatus::Failed ||
                        t->status() == falcon::TaskStatus::Cancelled) {
                        items.push_back(task_to_status_json(*t));
                    }
                }
#ifdef FALCON_HAS_SQLITE3
                if (storage_) {
                    for (const auto& record : storage_extra_records(
                             storage_, tasks,
                             {falcon::TaskStatus::Completed, falcon::TaskStatus::Failed,
                              falcon::TaskStatus::Cancelled})) {
                        items.push_back(task_record_to_status_json(record));
                    }
                }
#endif
                return paginate_items(items, offset, num);
            }

            if (m == "aria2.removeDownloadResult") {
                if (!p.is_array() || p.empty() || !p[0].is_string()) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                auto tid = gid_to_task_id(p[0].get<std::string>());
                if (!tid) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }

                bool existed = false;
                if (auto task = engine_->get_task(*tid)) {
                    existed = true;
                    // 引擎只允许移除终态任务；活动任务返回失败
                    if (!engine_->remove_task(*tid)) {
                        return json{{"error", json{{"code", 1},
                                                   {"message", "Task cannot be removed while active"}}}};
                    }
                }
#ifdef FALCON_HAS_SQLITE3
                if (storage_) {
                    if (storage_->task_exists(*tid)) {
                        existed = true;
                        storage_->delete_task(*tid);
                    }
                }
#endif
                if (!existed) {
                    return json{{"error", json{{"code", 2}, {"message", "Task not found"}}}};
                }
                return "OK";
            }

            return json{{"error", json{{"code", -32601}, {"message", "Method not found"}}}};
        };

        json result = dispatch(method, params);
        if (result.is_object() && result.contains("error")) {
            resp.body = make_error(id, result["error"].value("code", -32000),
                                   result["error"].value("message", "Error"))
                            .dump();
            return resp;
        }

        resp.body = make_result(id, result).dump();
        return resp;
    } catch (const json::parse_error& e) {
        // -32700 专属于请求体不是合法 JSON 的情形
        resp.body = make_error(id, -32700, std::string("Parse error: ") + e.what()).dump();
        return resp;
    } catch (const std::exception& e) {
        // dispatch 内部的运行时失败（如输出目录不可写）是服务端错误，不是解析错误
        resp.body = make_error(id, -32603, std::string("Internal error: ") + e.what()).dump();
        return resp;
    }
}

} // namespace falcon::daemon::rpc
