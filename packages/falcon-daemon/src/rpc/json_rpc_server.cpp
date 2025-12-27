#include "json_rpc_server.hpp"

#include <falcon/download_task.hpp>
#include <falcon/logger.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace falcon::daemon::rpc {
namespace {

using json = nlohmann::json;

struct ScopedFd {
    int fd = -1;
    ~ScopedFd() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;
    ScopedFd() = default;
    explicit ScopedFd(int f) : fd(f) {}
    ScopedFd(ScopedFd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            if (fd >= 0) ::close(fd);
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

static std::string aria2_status_from_task(const falcon::DownloadTask& task) {
    switch (task.status()) {
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

static json task_to_status_json(const falcon::DownloadTask& task) {
    json out;
    out["gid"] = task_id_to_gid(task.id());
    out["status"] = aria2_status_from_task(task);
    out["totalLength"] = std::to_string(task.total_bytes());
    out["completedLength"] = std::to_string(task.downloaded_bytes());
    out["downloadSpeed"] = std::to_string(task.speed());
    out["errorMessage"] = task.error_message();
    out["files"] = json::array(
        {json{
            {"path", task.output_path()},
            {"length", std::to_string(task.total_bytes())},
            {"completedLength", std::to_string(task.downloaded_bytes())},
            {"uris", json::array({json{{"uri", task.url()}}})},
        }});
    return out;
}

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

} // namespace

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

JsonRpcServer::JsonRpcServer(falcon::DownloadEngine* engine, JsonRpcServerConfig config)
    : engine_(engine), config_(std::move(config)) {}

JsonRpcServer::~JsonRpcServer() {
    stop();
}

bool JsonRpcServer::start() {
    if (!engine_) {
        FALCON_LOG_ERROR("JsonRpcServer start failed: engine is null");
        return false;
    }
    if (accept_thread_.joinable()) {
        return true;
    }

    stop_requested_ = false;

    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        FALCON_LOG_ERROR("socket() failed: " << std::strerror(errno));
        return false;
    }

    int yes = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.listen_port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
        FALCON_LOG_ERROR("inet_pton() failed for bind_address=" << config_.bind_address);
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        FALCON_LOG_ERROR("bind() failed: " << std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // If port was 0, OS picks an ephemeral port; query the actual port.
    if (config_.listen_port == 0) {
        sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
            config_.listen_port = ntohs(bound.sin_port);
        }
    }

    if (::listen(listen_fd_, 128) < 0) {
        FALCON_LOG_ERROR("listen() failed: " << std::strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    accept_thread_ = std::thread([this] { accept_loop(); });
    FALCON_LOG_INFO("JSON-RPC server listening on " << config_.bind_address << ":" << config_.listen_port);
    return true;
}

void JsonRpcServer::stop() {
    stop_requested_ = true;

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
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
}

void JsonRpcServer::accept_loop() {
    while (!stop_requested_.load()) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
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
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
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
        ssize_t n = ::send(fd, data.data() + off, data.size() - off, 0);
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

    auto resp = handle_http_request(*req);
    const std::string out = format_http_response(resp);
    send_all(fd.fd, out);
}

JsonRpcServer::HttpResponse JsonRpcServer::handle_http_request(const HttpRequest& req) {
    HttpResponse resp;
    resp.headers["Server"] = "falcon-daemon";
    resp.headers["Content-Type"] = "application/json";
    resp.headers["Connection"] = "close";

    if (config_.allow_origin_all) {
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
    if (config_.allow_origin_all) {
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
        if (!validate_and_strip_token(params, config_.secret)) {
            resp.body = make_error(id, -32001, "Unauthorized").dump();
            return resp;
        }

        std::function<json(const std::string&, json)> dispatch;
        dispatch = [&](const std::string& m, json p) -> json {
            // For system.multicall inner calls, some clients may redundantly include token again.
            maybe_strip_token(p, config_.secret);

            if (m == "system.listMethods") {
                return json::array({
                    "aria2.addUri",
                    "aria2.pause",
                    "aria2.unpause",
                    "aria2.remove",
                    "aria2.tellStatus",
                    "aria2.tellActive",
                    "aria2.tellWaiting",
                    "aria2.tellStopped",
                    "aria2.getGlobalStat",
                    "aria2.getVersion",
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
                    if (!call.is_object()) {
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
                engine_->start_task(task->id());
                return task_id_to_gid(task->id());
            }

            if (m == "aria2.pause" || m == "aria2.unpause" || m == "aria2.remove" || m == "aria2.tellStatus") {
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

                if (m == "aria2.pause") {
                    if (!engine_->pause_task(*tid)) {
                        return json{{"error", json{{"code", 1}, {"message", "Pause failed"}}}};
                    }
                    return gid;
                }
                if (m == "aria2.unpause") {
                    if (!engine_->resume_task(*tid)) {
                        return json{{"error", json{{"code", 1}, {"message", "Resume failed"}}}};
                    }
                    return gid;
                }
                if (m == "aria2.remove") {
                    if (!engine_->cancel_task(*tid)) {
                        return json{{"error", json{{"code", 1}, {"message", "Remove failed"}}}};
                    }
                    return gid;
                }
                return task_to_status_json(*task);
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

                std::vector<falcon::DownloadTask::Ptr> waiting;
                auto pending = engine_->get_tasks_by_status(falcon::TaskStatus::Pending);
                auto paused = engine_->get_tasks_by_status(falcon::TaskStatus::Paused);
                waiting.reserve(pending.size() + paused.size());
                waiting.insert(waiting.end(), pending.begin(), pending.end());
                waiting.insert(waiting.end(), paused.begin(), paused.end());

                json out = json::array();
                int start = std::max(0, offset);
                int end = std::min<int>(static_cast<int>(waiting.size()), start + std::max(0, num));
                for (int i = start; i < end; ++i) {
                    if (!waiting[i]) continue;
                    out.push_back(task_to_status_json(*waiting[i]));
                }
                return out;
            }

            if (m == "aria2.tellStopped") {
                if (!p.is_array() || p.size() < 2) {
                    return json{{"error", json{{"code", -32602}, {"message", "Invalid params"}}}};
                }
                const int offset = p[0].is_number_integer() ? p[0].get<int>() : std::stoi(p[0].get<std::string>());
                const int num = p[1].is_number_integer() ? p[1].get<int>() : std::stoi(p[1].get<std::string>());

                std::vector<falcon::DownloadTask::Ptr> stopped;
                auto completed = engine_->get_tasks_by_status(falcon::TaskStatus::Completed);
                auto failed = engine_->get_tasks_by_status(falcon::TaskStatus::Failed);
                auto cancelled = engine_->get_tasks_by_status(falcon::TaskStatus::Cancelled);
                stopped.reserve(completed.size() + failed.size() + cancelled.size());
                stopped.insert(stopped.end(), completed.begin(), completed.end());
                stopped.insert(stopped.end(), failed.begin(), failed.end());
                stopped.insert(stopped.end(), cancelled.begin(), cancelled.end());

                json out = json::array();
                int start = std::max(0, offset);
                int end = std::min<int>(static_cast<int>(stopped.size()), start + std::max(0, num));
                for (int i = start; i < end; ++i) {
                    if (!stopped[i]) continue;
                    out.push_back(task_to_status_json(*stopped[i]));
                }
                return out;
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
    } catch (const std::exception& e) {
        resp.body = make_error(id, -32700, std::string("Parse error: ") + e.what()).dump();
        return resp;
    }
}

} // namespace falcon::daemon::rpc
