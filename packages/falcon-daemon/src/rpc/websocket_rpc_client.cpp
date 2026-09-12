/**
 * @file websocket_rpc_client.cpp
 * @brief WebSocketRpcClient 实现
 * @author Falcon Team
 * @date 2026-09-12
 */

#include "rpc/websocket_rpc_client.hpp"

#include "rpc/websocket_frame.hpp"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>

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
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace falcon::daemon::rpc {

namespace {

using falcon::daemon::rpc::WS_OP_CLOSE;
using falcon::daemon::rpc::WS_OP_PING;
using falcon::daemon::rpc::WS_OP_PONG;
using falcon::daemon::rpc::WS_OP_TEXT;

#ifdef _WIN32
using recv_send_size_t = int;
static int socket_close(int fd) { return ::closesocket(static_cast<SOCKET>(fd)); }
static int socket_shutdown(int fd) {
    return ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
}
static void ensure_winsock_started() {
    static std::once_flag once;
    std::call_once(once, []() {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    });
}
static void clear_recv_timeout(int fd) {
    const DWORD timeout = 0;  // 0 = 无限阻塞
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
static void set_recv_timeout_ms(int fd, int ms) {
    const DWORD timeout = static_cast<DWORD>(ms);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}
#else
using recv_send_size_t = ssize_t;
static int socket_close(int fd) { return ::close(fd); }
static int socket_shutdown(int fd) { return ::shutdown(fd, SHUT_RDWR); }
static void ensure_winsock_started() {}
static void clear_recv_timeout(int fd) {
    timeval tv{};  // 全零 = 无限阻塞
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
static void set_recv_timeout_ms(int fd, int ms) {
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
#endif

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

/// 阻塞式 TCP 连接（getaddrinfo 解析主机名；遍历全部候选地址）
static int open_tcp_connected(const std::string& host, const std::string& port) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* result = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0) {
        return -1;
    }
    int fd = -1;
    for (struct addrinfo* p = result; p; p = p->ai_next) {
        fd = static_cast<int>(::socket(p->ai_family, p->ai_socktype, p->ai_protocol));
        if (fd < 0) continue;
#ifdef _WIN32
        const int connect_rc =
            ::connect(fd, p->ai_addr, static_cast<int>(p->ai_addrlen));
#else
        const int connect_rc = ::connect(fd, p->ai_addr, p->ai_addrlen);
#endif
        if (connect_rc == 0) break;
        socket_close(fd);
        fd = -1;
    }
    ::freeaddrinfo(result);
    return fd;
}

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

JsonRpcError transport_error(const std::string& message) {
    return JsonRpcError{-32000, message};
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / 析构 / 连接管理
// ---------------------------------------------------------------------------

WebSocketRpcClient::WebSocketRpcClient(JsonRpcClientConfig config)
    : config_(std::move(config)) {
    ensure_winsock_started();
    endpoint_ = parse_url(config_.url);
}

WebSocketRpcClient::~WebSocketRpcClient() {
    disconnect();
}

void WebSocketRpcClient::set_url(const std::string& url) {
    disconnect();
    std::lock_guard<std::mutex> lock(connect_mutex_);
    endpoint_ = parse_url(url);
}

WebSocketRpcClient::Endpoint WebSocketRpcClient::parse_url(const std::string& url) {
    Endpoint ep;
    std::string rest = url;
    // 剥离 scheme：ws:// wss:// http:// https://（wss 不支持，按明文处理）
    const auto scheme_end = rest.find("://");
    if (scheme_end != std::string::npos) rest = rest.substr(scheme_end + 3);
    // 去掉 userinfo（桌面端 URL 不应含，容错处理）
    const auto at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);
    // path
    const auto slash = rest.find('/');
    if (slash != std::string::npos) {
        ep.path = rest.substr(slash);
        rest = rest.substr(0, slash);
    } else {
        ep.path = "/jsonrpc";
    }
    if (ep.path.empty() || ep.path == "/") ep.path = "/jsonrpc";
    // [IPv6]:port
    if (!rest.empty() && rest.front() == '[') {
        const auto close_bracket = rest.find(']');
        if (close_bracket != std::string::npos) {
            ep.host = rest.substr(1, close_bracket - 1);
            if (close_bracket + 1 < rest.size() && rest[close_bracket + 1] == ':') {
                ep.port = rest.substr(close_bracket + 2);
            }
        }
    } else {
        const auto colon = rest.rfind(':');
        if (colon != std::string::npos) {
            ep.host = rest.substr(0, colon);
            ep.port = rest.substr(colon + 1);
        } else {
            ep.host = rest;
        }
    }
    if (ep.port.empty()) ep.port = "6800"; // aria2 默认 RPC 端口
    return ep;
}

bool WebSocketRpcClient::connect() {
    std::lock_guard<std::mutex> lock(connect_mutex_);
    if (connected_.load()) return true;

    // 上一个会话线程可能还在退出中
    if (reader_.joinable()) reader_.join();

    const int fd = open_tcp_connected(endpoint_.host, endpoint_.port);
    if (fd < 0) return false;

    // 握手期间用配置超时防服务器不应答；握手完成后恢复无限阻塞
    set_recv_timeout_ms(fd, static_cast<int>(config_.timeout_seconds) * 1000);

    // 16 随机字节的 base64（RFC 6455 §4.1）
    std::mt19937_64 rng{std::random_device{}()};
    std::uint8_t raw[16];
    for (auto& b : raw) b = static_cast<std::uint8_t>(rng());
    const std::string key = ws_base64_encode(raw, sizeof(raw));

    std::string req;
    req += "GET " + endpoint_.path + " HTTP/1.1\r\n";
    req += "Host: " + endpoint_.host + ":" + endpoint_.port + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + key + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(fd, req)) {
        socket_close(fd);
        return false;
    }

    // 读到响应头结束（16 KiB 上限防滥用）
    std::string buf;
    char tmp[1024];
    while (buf.find("\r\n\r\n") == std::string::npos && buf.size() < 16 * 1024) {
        recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, static_cast<std::size_t>(n));
    }
    const auto header_end = buf.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        socket_close(fd);
        return false;
    }
    const std::string headers = buf.substr(0, header_end);
    std::string leftover = buf.substr(header_end + 4);

    // 状态行必须是 101
    const auto first_line_end = headers.find("\r\n");
    const std::string status_line =
        first_line_end == std::string::npos ? headers : headers.substr(0, first_line_end);
    if (status_line.find("101") == std::string::npos) {
        socket_close(fd);
        return false;
    }
    // Sec-WebSocket-Accept 校验（头名大小写不敏感）
    const std::string expected_accept = ws_compute_accept_key(key);
    bool accept_ok = false;
    std::size_t line_begin = first_line_end == std::string::npos ? headers.size() : first_line_end + 2;
    while (line_begin < headers.size()) {
        const auto line_end = headers.find("\r\n", line_begin);
        const std::size_t line_stop =
            line_end == std::string::npos ? headers.size() : line_end;
        const std::string line = headers.substr(line_begin, line_stop - line_begin);
        const auto colon = line.find(':');
        if (colon != std::string::npos &&
            to_lower(trim(line.substr(0, colon))) == "sec-websocket-accept") {
            accept_ok = trim(line.substr(colon + 1)) == expected_accept;
            break;
        }
        if (line_end == std::string::npos) break;
        line_begin = line_end + 2;
    }
    if (!accept_ok) {
        socket_close(fd);
        return false;
    }

    clear_recv_timeout(fd);

    // 先登记连接状态再启动读线程：读线程可能立即因对端关闭而收尾，
    // 它收尾时通过 fd_ 原子变量取到同一 fd
    fd_.store(fd);
    connected_.store(true);
    try {
        reader_ = std::thread([this, fd, leftover = std::move(leftover)]() mutable {
            session_loop(fd, std::move(leftover));
        });
    } catch (...) {
        connected_.store(false);
        fd_.store(-1);
        socket_close(fd);
        return false;
    }
    return true;
}

void WebSocketRpcClient::disconnect() {
    {
        std::lock_guard<std::mutex> lock(connect_mutex_);
        const int fd = fd_.exchange(-1);
        if (fd >= 0) {
            socket_shutdown(fd); // 唤醒阻塞在 recv 的会话线程
        }
    }
    join_reader();
    connected_.store(false);
}

void WebSocketRpcClient::join_reader() {
    // 调用方持有 connect_mutex_（reader 不拿该锁，无死锁）
    if (reader_.joinable()) reader_.join();
}

void WebSocketRpcClient::set_notification_handler(NotificationHandler handler) {
    // 与分发互斥：返回后保证不再有在途 handler 调用（析构时序安全）
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler_ = std::move(handler);
}

// ---------------------------------------------------------------------------
// 会话读线程
// ---------------------------------------------------------------------------

void WebSocketRpcClient::session_loop(int fd, std::string leftover) {
    using falcon::daemon::rpc::WsFrame;
    using falcon::daemon::rpc::WsFrameParser;

    WsFrameParser parser;
    parser.feed(leftover.data(), leftover.size());
    leftover.clear();

    std::string buf(4096, '\0');
    bool closing = false;
    while (!closing && !parser.error()) {
        for (auto& msg : parser.pop_messages()) {
            switch (msg.opcode) {
                case WS_OP_TEXT:
                    dispatch_message(msg.payload);
                    break;
                case WS_OP_PING:
                    (void)send_frame(WS_OP_PONG, msg.payload);
                    break;
                case WS_OP_CLOSE:
                    // 回应 close 帧后结束会话
                    closing = true;
                    (void)send_frame(WS_OP_CLOSE, msg.payload);
                    break;
                default:
                    break; // pong / binary 忽略
            }
        }
        if (closing) break;

        recv_send_size_t n = ::recv(fd, &buf[0], buf.size(), 0);
        if (n <= 0) break; // 对端关闭或连接错误
        parser.feed(buf.data(), static_cast<std::size_t>(n));
    }

    socket_close(fd);
    fd_.store(-1);
    connected_.store(false);
    fail_pending(-32000, "connection closed");
}

void WebSocketRpcClient::dispatch_message(const std::string& payload) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(payload);
    } catch (const nlohmann::json::exception&) {
        return; // 非 JSON 帧忽略
    }
    if (!parsed.is_object()) return;

    // 带 id 的消息是请求响应（本客户端只发数字 id）
    if (parsed.contains("id")) {
        const auto& rid = parsed["id"];
        if (!rid.is_number_unsigned()) return;
        std::shared_ptr<PendingCall> slot;
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            const auto it = pending_.find(rid.get<std::uint64_t>());
            if (it == pending_.end()) return; // 超时已被移除：迟到响应丢弃
            slot = std::move(it->second);
            pending_.erase(it);
        }
        {
            std::lock_guard<std::mutex> lock(slot->mutex);
            slot->response = std::move(parsed);
            slot->done = true;
        }
        slot->cv.notify_all();
        return;
    }

    // 无 id：服务器通知
    if (!parsed.contains("method") || !parsed["method"].is_string()) return;
    std::string params_text = "[]";
    if (parsed.contains("params")) {
        try {
            params_text = parsed["params"].dump();
        } catch (const nlohmann::json::exception&) {
            return;
        }
    }
    // 持锁调用：set_notification_handler 返回后不会再有在途调用。
    // handler 内允许调用 call()（不取 handler_mutex_），但不允许 disconnect()。
    std::lock_guard<std::mutex> lock(handler_mutex_);
    if (handler_) handler_(parsed["method"].get<std::string>(), params_text);
}

void WebSocketRpcClient::fail_pending(int code, const std::string& message) {
    std::map<std::uint64_t, std::shared_ptr<PendingCall>> abandoned;
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        abandoned = std::move(pending_);
        pending_.clear();
    }
    for (auto& [id, slot] : abandoned) {
        (void)id;
        nlohmann::json response = {
            {"jsonrpc", "2.0"},
            {"id", nullptr},
            {"error", {{"code", code}, {"message", message}}},
        };
        {
            std::lock_guard<std::mutex> slot_lock(slot->mutex);
            slot->response = std::move(response);
            slot->done = true;
        }
        slot->cv.notify_all();
    }
}

// ---------------------------------------------------------------------------
// 请求
// ---------------------------------------------------------------------------

bool WebSocketRpcClient::send_frame(std::uint8_t opcode, const std::string& payload) {
    const int fd = fd_.load();
    if (fd < 0) return false;
    std::string frame = ws_encode_client_frame(opcode, payload);
    std::lock_guard<std::mutex> lock(write_mutex_);
    return send_all(fd, frame);
}

std::optional<nlohmann::json> WebSocketRpcClient::call(const std::string& method,
                                                       nlohmann::json params,
                                                       JsonRpcError* err) {
    const auto fail = [&err](JsonRpcError e) {
        if (err) *err = std::move(e);
        return std::nullopt;
    };

    if (!params.is_array()) {
        params = nlohmann::json::array();
    }
    if (!config_.secret.empty()) {
        params.insert(params.begin(), "token:" + config_.secret);
    }

    if (!connected_.load() && !connect()) {
        return fail(transport_error("WebSocket not connected"));
    }

    const auto id = next_id_.fetch_add(1);
    nlohmann::json request = {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"method", method},
        {"params", std::move(params)},
    };

    auto slot = std::make_shared<PendingCall>();
    {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_[id] = slot;
    }

    if (!send_frame(WS_OP_TEXT, request.dump())) {
        // 发送失败：连接已坏。读线程大概率也会 recv 失败并自行收尾；
        // 此处 shutdown 加速其退出
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_.erase(id);
        }
        const int fd = fd_.load();
        if (fd >= 0) socket_shutdown(fd);
        return fail(transport_error("WebSocket send failed"));
    }

    bool timed_out = false;
    {
        std::unique_lock<std::mutex> slot_lock(slot->mutex);
        if (!slot->cv.wait_for(slot_lock, std::chrono::seconds(config_.timeout_seconds),
                               [&slot]() { return slot->done; })) {
            timed_out = true;
        }
    }
    if (timed_out) {
        // 移除等待槽；迟到的响应会因条目不存在而被丢弃
        std::lock_guard<std::mutex> lock(pending_mutex_);
        pending_.erase(id);
        return fail(transport_error("WebSocket request timed out"));
    }

    // 响应处理（与 JsonRpcClient::call 相同的约定）
    nlohmann::json& response = slot->response;
    if (!response.is_object()) {
        return fail(transport_error("Invalid response frame"));
    }
    if (response.contains("error")) {
        const auto& e = response["error"];
        JsonRpcError rpc_err;
        rpc_err.code = e.value("code", -32000);
        rpc_err.message = e.value("message", "Unknown error");
        return fail(std::move(rpc_err));
    }
    if (!response.contains("result")) {
        return fail(JsonRpcError{-32600, "Response has neither result nor error"});
    }

    if (err) *err = JsonRpcError{};
    return std::optional<nlohmann::json>(std::move(response["result"]));
}

// ---------------------------------------------------------------------------
// 便捷封装（与 JsonRpcClient 平行）
// ---------------------------------------------------------------------------

namespace {

std::optional<std::string> as_gid(std::optional<nlohmann::json> result,
                                  JsonRpcError* err) {
    if (!result) return std::nullopt;
    if (!result->is_string()) {
        if (err) *err = JsonRpcError{-32600, "Expected gid string in response"};
        return std::nullopt;
    }
    return result->get<std::string>();
}

bool expect_ok(std::optional<nlohmann::json> result, JsonRpcError* err) {
    if (!result) return false;
    if (!result->is_string() || result->get<std::string>() != "OK") {
        if (err) *err = JsonRpcError{-32600, "Expected \"OK\" in response"};
        return false;
    }
    return true;
}

} // namespace

std::optional<std::string> WebSocketRpcClient::add_uri(const std::vector<std::string>& uris,
                                                       const nlohmann::json& options,
                                                       JsonRpcError* err) {
    nlohmann::json params = nlohmann::json::array({uris});
    if (!options.is_null()) params.push_back(options);
    return as_gid(call("aria2.addUri", std::move(params), err), err);
}

std::optional<std::string> WebSocketRpcClient::pause(const std::string& gid,
                                                     JsonRpcError* err) {
    return as_gid(call("aria2.pause", nlohmann::json::array({gid}), err), err);
}

std::optional<std::string> WebSocketRpcClient::unpause(const std::string& gid,
                                                       JsonRpcError* err) {
    return as_gid(call("aria2.unpause", nlohmann::json::array({gid}), err), err);
}

std::optional<std::string> WebSocketRpcClient::remove(const std::string& gid,
                                                      JsonRpcError* err) {
    return as_gid(call("aria2.remove", nlohmann::json::array({gid}), err), err);
}

std::optional<std::string> WebSocketRpcClient::change_priority(const std::string& gid,
                                                               int priority,
                                                               JsonRpcError* err) {
    return as_gid(call("aria2.changePriority", nlohmann::json::array({gid, priority}), err),
                  err);
}

std::optional<nlohmann::json> WebSocketRpcClient::tell_status(const std::string& gid,
                                                              JsonRpcError* err) {
    return call("aria2.tellStatus", nlohmann::json::array({gid}), err);
}

std::optional<nlohmann::json> WebSocketRpcClient::tell_active(JsonRpcError* err) {
    return call("aria2.tellActive", nlohmann::json::array(), err);
}

std::optional<nlohmann::json> WebSocketRpcClient::tell_waiting(JsonRpcError* err) {
    return call("aria2.tellWaiting", nlohmann::json::array({0, 10000}), err);
}

std::optional<nlohmann::json> WebSocketRpcClient::tell_stopped(JsonRpcError* err) {
    return call("aria2.tellStopped", nlohmann::json::array({0, 10000}), err);
}

std::optional<nlohmann::json> WebSocketRpcClient::get_global_stat(JsonRpcError* err) {
    return call("aria2.getGlobalStat", nlohmann::json::array(), err);
}

bool WebSocketRpcClient::change_global_option(const nlohmann::json& options,
                                              JsonRpcError* err) {
    return expect_ok(call("aria2.changeGlobalOption",
                          nlohmann::json::array({options}), err),
                     err);
}

bool WebSocketRpcClient::save_session(JsonRpcError* err) {
    return expect_ok(call("aria2.saveSession", nlohmann::json::array(), err), err);
}

bool WebSocketRpcClient::purge_download_result(JsonRpcError* err) {
    return expect_ok(call("aria2.purgeDownloadResult", nlohmann::json::array(), err), err);
}

bool WebSocketRpcClient::remove_download_result(const std::string& gid,
                                                JsonRpcError* err) {
    return expect_ok(call("aria2.removeDownloadResult",
                          nlohmann::json::array({gid}), err),
                     err);
}

bool WebSocketRpcClient::shutdown(JsonRpcError* err) {
    return expect_ok(call("aria2.forceShutdown", nlohmann::json::array(), err), err);
}

} // namespace falcon::daemon::rpc
