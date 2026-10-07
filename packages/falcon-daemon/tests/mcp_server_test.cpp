/// MCP Streamable HTTP 端点测试（协议契约 + 工具翻译层）。
/// 设计文档 docs/design/mcp_server_design.md §2 契约逐条钉住：
/// 认证分级（403/401）、initialize 会话握手、会话校验 404、
/// 通知 202、DELETE 拆除 204、未知方法 405、GET/SSE 通知流
///（认证与会话准入 + data 行扇出 + 停机/断开收尾，阶段 2 增量 3）、
/// 解析/批量错误分级、tools/list 清单 schema、tools/call 翻译与
/// isError 映射。

#include "rpc/json_rpc_server.hpp"
#include "rpc/mcp_server.hpp"

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
#include <sys/time.h>
#include <unistd.h>
#endif

#include <falcon/download_task.hpp>
#include <falcon/protocol_handler.hpp>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace {

using json = nlohmann::json;
using falcon::daemon::rpc::JsonRpcServer;
using falcon::daemon::rpc::JsonRpcServerConfig;

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
#else
using recv_send_size_t = ssize_t;
static int socket_close(int fd) { return ::close(fd); }
static void ensure_winsock_started() {}
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

static std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
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
        recv_send_size_t n = ::send(fd, data.data() + off, chunk_len, 0);
        if (n <= 0) return false;
        off += static_cast<std::size_t>(n);
    }
    return true;
}

static std::optional<std::string> recv_all(int fd) {
    std::string buf;
    char tmp[4096];
    while (true) {
        recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) break;
        if (n < 0) return std::nullopt;
        buf.append(tmp, tmp + n);
        if (buf.size() > 1024 * 1024) return std::nullopt;
    }
    return buf;
}

/// SSE 流测试用：recv 不再无限阻塞（超时报错 → recv_until 返回 nullopt）
static void set_recv_timeout(int fd, unsigned seconds) {
#ifdef _WIN32
    DWORD ms = seconds * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{};
    tv.tv_sec = seconds;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/// 读到出现 delim（含）为止；已读字节累积在 buf（跨调用保留残段），
/// 命中后从 buf 头部截出返回。对端断开或读超时返回 nullopt。
static std::optional<std::string> recv_until(int fd, const std::string& delim,
                                             std::string& buf) {
    while (true) {
        auto pos = buf.find(delim);
        if (pos != std::string::npos) {
            std::string out = buf.substr(0, pos + delim.size());
            buf.erase(0, pos + delim.size());
            return out;
        }
        char tmp[1024];
        recv_send_size_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return std::nullopt;
        buf.append(tmp, tmp + n);
        if (buf.size() > 1024 * 1024) return std::nullopt;
    }
}

template <typename Pred>
static bool wait_for(
    Pred&& pred,
    std::chrono::milliseconds budget = std::chrono::milliseconds(5000)) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

static ScopedFd connect_loopback(uint16_t port) {
    ensure_winsock_started();
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    for (int attempt = 0; attempt < 50; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return ScopedFd{};
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0)
            return ScopedFd{fd};
        socket_close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return ScopedFd{};
}

struct HttpResp {
    int status = 0;
    std::string status_text;
    std::map<std::string, std::string> headers;  // 头键小写
    std::string body;
};

static std::optional<HttpResp> http_exchange(uint16_t port, const std::string& raw) {
    ScopedFd fd = connect_loopback(port);
    if (fd.fd < 0) return std::nullopt;
    if (!send_all(fd.fd, raw)) return std::nullopt;
    auto resp = recv_all(fd.fd);
    if (!resp) return std::nullopt;

    const std::string sep = "\r\n\r\n";
    auto pos = resp->find(sep);
    if (pos == std::string::npos) return std::nullopt;

    HttpResp out;
    out.body = resp->substr(pos + sep.size());
    const std::string head = resp->substr(0, pos);

    auto line_end = head.find("\r\n");
    const std::string status_line =
        head.substr(0, line_end == std::string::npos ? head.size() : line_end);
    const auto sp1 = status_line.find(' ');
    const auto sp2 = status_line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return std::nullopt;
    out.status = std::stoi(status_line.substr(sp1 + 1, sp2 - sp1 - 1));
    out.status_text = status_line.substr(sp2 + 1);

    size_t start = line_end == std::string::npos ? head.size() : line_end + 2;
    while (start < head.size()) {
        auto eol = head.find("\r\n", start);
        const std::string line =
            head.substr(start, eol == std::string::npos ? std::string::npos : eol - start);
        if (!line.empty()) {
            auto colon = line.find(':');
            if (colon != std::string::npos)
                out.headers[to_lower(trim(line.substr(0, colon)))] =
                    trim(line.substr(colon + 1));
        }
        if (eol == std::string::npos) break;
        start = eol + 2;
    }
    return out;
}

static std::optional<HttpResp> mcp_request(uint16_t port, const std::string& method,
                                           const std::string& bearer_token,
                                           const std::string& session,
                                           const std::string& body) {
    std::string http = method + " /mcp HTTP/1.1\r\n";
    http += "Host: 127.0.0.1\r\n";
    if (!bearer_token.empty())
        http += "Authorization: Bearer " + bearer_token + "\r\n";
    if (!session.empty())
        http += "Mcp-Session-Id: " + session + "\r\n";
    http += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    if (!body.empty())
        http += "Content-Type: application/json\r\n";
    http += "Connection: close\r\n\r\n";
    http += body;
    return http_exchange(port, http);
}

constexpr const char* kSecret = "s3cr3t";

static std::unique_ptr<JsonRpcServer> make_server(
    falcon::DownloadEngine& engine, bool mcp_enabled = true,
    const std::string& secret = kSecret, bool allow_origin_all = false) {
    JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.secret = secret;
    cfg.mcp_enabled = mcp_enabled;
    cfg.allow_origin_all = allow_origin_all;
    auto server = std::make_unique<JsonRpcServer>(&engine, cfg);
    EXPECT_TRUE(server->start());
    EXPECT_NE(server->port(), 0);
    return server;
}

static HttpResp initialize_session(uint16_t port, const std::string& token,
                                   const char* client_version = "2025-06-18") {
    json req = {
        {"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
        {"params", {{"protocolVersion", client_version},
                    {"capabilities", json::object()},
                    {"clientInfo", {{"name", "mcp-server-test"}, {"version", "0"}}}}}};
    auto resp = mcp_request(port, "POST", token, "", req.dump());
    EXPECT_TRUE(resp.has_value());
    return *resp;
}

/// initialize → 提取会话 id
static std::string open_session(uint16_t port, const std::string& token) {
    HttpResp resp = initialize_session(port, token);
    EXPECT_EQ(resp.status, 200);
    auto it = resp.headers.find("mcp-session-id");
    EXPECT_TRUE(it != resp.headers.end());
    return it != resp.headers.end() ? it->second : std::string();
}

static json rpc_body(int id, const char* method, json params) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
}

/// tools/call 往返，返回 MCP result 对象
static json call_tool(uint16_t port, const std::string& token,
                      const std::string& session, const char* tool, json args) {
    json req = rpc_body(7, "tools/call",
                        json{{"name", tool}, {"arguments", std::move(args)}});
    auto resp = mcp_request(port, "POST", token, session, req.dump());
    EXPECT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    json body = json::parse(resp->body);
    return body.at("result");
}

/// 必须是 32 hex 字符
void expect_session_id_shape(const std::string& sid) {
    ASSERT_EQ(sid.size(), 32u);
    for (char c : sid) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c))) << sid;
    }
}

} // namespace

// ---- 认证分级 ----

TEST(McpServerHttpTest, DisabledEndpointReturns404) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine, /*mcp_enabled=*/false);
    auto resp = mcp_request(server->port(), "POST", kSecret, "", "{}");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);
}

TEST(McpServerHttpTest, UnconfiguredSecretRejectsEverything403) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine, /*mcp_enabled=*/true, /*secret=*/"");
    auto resp = mcp_request(server->port(), "POST", "anything", "", "{}");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 403);
    EXPECT_NE(resp->body.find("rpc.secret"), std::string::npos);
}

TEST(McpServerHttpTest, MissingBearerTokenReturns401) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "POST", "", "", "{}");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 401);
    EXPECT_EQ(resp->headers.count("www-authenticate"), 1u);
}

TEST(McpServerHttpTest, WrongBearerTokenReturns401) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "POST", "wrong-token", "", "{}");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 401);
}

// ---- initialize 会话握手 ----

TEST(McpServerHttpTest, InitializeHandshakeRoundTrip) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    HttpResp resp = initialize_session(server->port(), kSecret);

    ASSERT_EQ(resp.status, 200);
    auto sid = resp.headers.find("mcp-session-id");
    ASSERT_TRUE(sid != resp.headers.end());
    expect_session_id_shape(sid->second);

    json body = json::parse(resp.body);
    ASSERT_TRUE(body.contains("result"));
    const json& result = body.at("result");
    EXPECT_EQ(result.at("protocolVersion"), "2025-06-18");
    ASSERT_TRUE(result.at("capabilities").contains("tools"));
    EXPECT_EQ(result.at("serverInfo").at("name"), "falcon");
    EXPECT_FALSE(result.at("serverInfo").at("version").get<std::string>().empty());
}

TEST(McpServerHttpTest, InitializeIgnoresClientSessionHeaderAndCreatesNew) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    // initialize 携带伪造会话头：规范要求忽略并新建
    json req = rpc_body(1, "initialize", json::object());
    auto resp = mcp_request(server->port(), "POST", kSecret, "forged-session",
                            req.dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    auto sid = resp->headers.find("mcp-session-id");
    ASSERT_TRUE(sid != resp->headers.end());
    EXPECT_NE(sid->second, "forged-session");
    expect_session_id_shape(sid->second);
}

TEST(McpServerHttpTest, InitializeNegotiatesKnownVersionAndFallsBack) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);

    // 客户端声明旧支持版本 → 原样回显
    HttpResp older = initialize_session(server->port(), kSecret, "2025-03-26");
    ASSERT_EQ(older.status, 200);
    EXPECT_EQ(json::parse(older.body).at("result").at("protocolVersion"),
              "2025-03-26");

    // 客户端声明未知版本 → 回应服务器默认
    HttpResp unknown = initialize_session(server->port(), kSecret, "1999-01-01");
    ASSERT_EQ(unknown.status, 200);
    EXPECT_EQ(json::parse(unknown.body).at("result").at("protocolVersion"),
              "2025-06-18");
}

// ---- 会话校验 ----

TEST(McpServerHttpTest, RequestWithoutSessionReturns404) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "POST", kSecret, "",
                            rpc_body(2, "ping", json::object()).dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);
}

TEST(McpServerHttpTest, RequestWithInvalidSessionReturns404) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "POST", kSecret, "deadbeef",
                            rpc_body(2, "ping", json::object()).dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);
}

TEST(McpServerHttpTest, DeleteTeardownThenSessionInvalid) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    ASSERT_FALSE(sid.empty());

    // 无会话头的 DELETE → 404
    auto no_session = mcp_request(server->port(), "DELETE", kSecret, "", "");
    ASSERT_TRUE(no_session.has_value());
    EXPECT_EQ(no_session->status, 404);

    // 合法会话 DELETE → 204 空 body
    auto del = mcp_request(server->port(), "DELETE", kSecret, sid, "");
    ASSERT_TRUE(del.has_value());
    EXPECT_EQ(del->status, 204);
    EXPECT_TRUE(del->body.empty());

    // 删除后的会话再使用 → 404
    auto after = mcp_request(server->port(), "POST", kSecret, sid,
                             rpc_body(3, "ping", json::object()).dump());
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, 404);
}

// ---- HTTP 方法路由 ----
// GET /mcp 自阶段 2 增量 3 起被 JsonRpcServer 拦截为 SSE 通知流（见下节）；
// 405 形状仍服务于 GET 之外的未知方法（PUT 等）。

TEST(McpServerHttpTest, PutMethodNotAllowed) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "PUT", kSecret, "", "{}");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 405);
    auto allow = resp->headers.find("allow");
    ASSERT_TRUE(allow != resp->headers.end());
    EXPECT_EQ(allow->second, "POST, DELETE");
}

// ---- GET /mcp SSE 通知流（阶段 2 增量 3）----
// 流本体无 Content-Length 且成功路径不关连接——错误形状（403/401/404）
// 仍走普通响应（Connection: close + EOF），用 http_exchange；成功流用
// 裸 socket + recv_until。

// 准入与 POST/DELETE 同语义：未配 secret 整体 403（无凭据不放行）。
TEST(McpServerSseTest, GetSseUnconfiguredSecretReturns403) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine, true, "");
    auto resp = mcp_request(server->port(), "GET", "", "", "");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 403);
}

// 认证先于会话校验：缺 Bearer → 401 + WWW-Authenticate。
TEST(McpServerSseTest, GetSseMissingBearerReturns401) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "GET", "", "", "");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 401);
    auto auth = resp->headers.find("www-authenticate");
    ASSERT_TRUE(auth != resp->headers.end());
    EXPECT_EQ(auth->second, "Bearer");
}

TEST(McpServerSseTest, GetSseWrongBearerReturns401) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "GET", "wrong", "", "");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 401);
}

// 会话缺失/过期 → 404 + re-initialize 指引（与 POST 同语义）。
TEST(McpServerSseTest, GetSseWithoutSessionReturns404) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "GET", kSecret, "", "");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);
    EXPECT_NE(resp->body.find("re-initialize"), std::string::npos);
}

TEST(McpServerSseTest, GetSseWithInvalidSessionReturns404) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto resp = mcp_request(server->port(), "GET", kSecret, "deadbeef", "");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 404);
}

// DELETE 拆除后的会话再开流同样 404（流准入读同一会话表）。
TEST(McpServerSseTest, GetSseAfterDeleteSessionReturns404) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto sid = open_session(server->port(), kSecret);
    ASSERT_FALSE(sid.empty());
    auto resp = mcp_request(server->port(), "DELETE", kSecret, sid, "");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 204);
    auto stream = mcp_request(server->port(), "GET", kSecret, sid, "");
    ASSERT_TRUE(stream.has_value());
    EXPECT_EQ(stream->status, 404);
}

// 主链路：合法会话开流 → 200 text/event-stream + 初始注释行 →
// broadcast_notification 扇出 `data: <JSON-RPC 信封>\n\n` → stop() 拆流。
TEST(McpServerSseTest, GetSseStreamOpensAndReceivesNotificationFanout) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto sid = open_session(server->port(), kSecret);
    ASSERT_FALSE(sid.empty());

    std::string http = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    http += "Authorization: Bearer " + std::string(kSecret) + "\r\n";
    http += "Mcp-Session-Id: " + sid + "\r\n\r\n";
    ScopedFd fd = connect_loopback(server->port());
    ASSERT_GE(fd.fd, 0);
    ASSERT_TRUE(send_all(fd.fd, http));
    set_recv_timeout(fd.fd, 10);

    std::string buf;
    // 流式响应头：200 + text/event-stream，且无 Content-Length（无界流）。
    auto head = recv_until(fd.fd, "\r\n\r\n", buf);
    ASSERT_TRUE(head.has_value());
    EXPECT_NE(head->find("200"), std::string::npos);
    EXPECT_NE(head->find("text/event-stream"), std::string::npos);
    EXPECT_EQ(head->find("Content-Length"), std::string::npos);
    // 初始注释行（`: falcon mcp stream`）——客户端确认流已开的锚点。
    auto comment = recv_until(fd.fd, "\r\n\r\n", buf);
    ASSERT_TRUE(comment.has_value());
    EXPECT_NE(comment->find(": falcon mcp stream"), std::string::npos);

    // 等连接线程完成注册，再从测试线程广播。
    ASSERT_TRUE(wait_for([&] { return server->sse_client_count() >= 1; }));
    server->broadcast_notification("aria2.onDownloadStart",
                                   R"([{"gid":"testgid"}])");
    auto line = recv_until(fd.fd, "\n\n", buf);
    ASSERT_TRUE(line.has_value());
    ASSERT_TRUE(line->rfind("data: ", 0) == 0)
        << "SSE 行必须以 data: 开头: " << *line;
    json notif = json::parse(line->substr(6));
    EXPECT_EQ(notif.at("jsonrpc"), "2.0");
    EXPECT_EQ(notif.at("method"), "aria2.onDownloadStart");
    EXPECT_EQ(notif.at("params").at(0).at("gid"), "testgid");

    // 第二条通知照常到达同一流（进度扩展通知同信封形状）。
    server->broadcast_notification(
        "falcon.onProgress",
        R"([{"gid":"testgid","completedLength":"1","totalLength":"2","speed":"0"}])");
    auto line2 = recv_until(fd.fd, "\n\n", buf);
    ASSERT_TRUE(line2.has_value());
    json notif2 = json::parse(line2->substr(6));
    EXPECT_EQ(notif2.at("method"), "falcon.onProgress");

    // 停机收口：stop() shutdown 唤醒，客户端读到 EOF，槽位归零。
    server->stop();
    char tmp[64];
    recv_send_size_t n = ::recv(fd.fd, tmp, sizeof(tmp), 0);
    EXPECT_TRUE(n <= 0);
    EXPECT_EQ(server->sse_client_count(), 0u);
}

// 客户端单方面断开：会话线程 recv 到 EOF 自行注销，不依赖 stop()。
TEST(McpServerSseTest, GetSseClientDisconnectReleasesSlot) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    auto sid = open_session(server->port(), kSecret);
    ASSERT_FALSE(sid.empty());

    // ScopedFd 无移交接口，裸 fd 手动管理（本用例自收尾）。
    int cfd = -1;
    {
        ScopedFd tmp = connect_loopback(server->port());
        cfd = tmp.fd;
        tmp.fd = -1;
    }
    ASSERT_GE(cfd, 0);
    std::string http = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    http += "Authorization: Bearer " + std::string(kSecret) + "\r\n";
    http += "Mcp-Session-Id: " + sid + "\r\n\r\n";
    ASSERT_TRUE(send_all(cfd, http));
    set_recv_timeout(cfd, 5);
    std::string buf;
    ASSERT_TRUE(recv_until(cfd, "\r\n\r\n", buf).has_value());
    ASSERT_TRUE(wait_for([&] { return server->sse_client_count() >= 1; }));

    socket_close(cfd);
    ASSERT_TRUE(wait_for([&] { return server->sse_client_count() == 0; }));
}

// CORS 放开时 SSE 流式响应头同样回显 Access-Control-Allow-Origin
// （与 WS 握手回显同语义）。
TEST(McpServerSseTest, GetSseCorsHeaderEchoedWhenAllowed) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine, true, kSecret, /*allow_origin_all=*/true);
    auto sid = open_session(server->port(), kSecret);
    ASSERT_FALSE(sid.empty());

    std::string http = "GET /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    http += "Origin: http://example.com\r\n";
    http += "Authorization: Bearer " + std::string(kSecret) + "\r\n";
    http += "Mcp-Session-Id: " + sid + "\r\n\r\n";
    ScopedFd fd = connect_loopback(server->port());
    ASSERT_GE(fd.fd, 0);
    ASSERT_TRUE(send_all(fd.fd, http));
    set_recv_timeout(fd.fd, 10);

    std::string buf;
    auto head = recv_until(fd.fd, "\r\n\r\n", buf);
    ASSERT_TRUE(head.has_value());
    EXPECT_NE(head->find("Access-Control-Allow-Origin: *"), std::string::npos);
}


// ---- JSON-RPC 解析分级 ----

TEST(McpServerHttpTest, ParseErrorReturns400With32700) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(server->port(), "POST", kSecret, sid, "{not json");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 400);
    json body = json::parse(resp->body);
    EXPECT_EQ(body.at("error").at("code"), -32700);
}

TEST(McpServerHttpTest, BatchRequestRejected400) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(server->port(), "POST", kSecret, sid,
                            R"([{"jsonrpc":"2.0","id":1,"method":"ping"}])");
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 400);
    json body = json::parse(resp->body);
    EXPECT_EQ(body.at("error").at("code"), -32600);
}

TEST(McpServerHttpTest, NonObjectAndMissingMethodReturn200With32600) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    auto non_object = mcp_request(server->port(), "POST", kSecret, sid, "\"str\"");
    ASSERT_TRUE(non_object.has_value());
    EXPECT_EQ(non_object->status, 200);
    EXPECT_EQ(json::parse(non_object->body).at("error").at("code"), -32600);

    auto missing_method = mcp_request(server->port(), "POST", kSecret, sid,
                                      R"({"jsonrpc":"2.0","id":5})");
    ASSERT_TRUE(missing_method.has_value());
    EXPECT_EQ(missing_method->status, 200);
    json body = json::parse(missing_method->body);
    EXPECT_EQ(body.at("error").at("code"), -32600);
    EXPECT_EQ(body.at("id"), 5);
}

TEST(McpServerHttpTest, NotificationAccepted202WithoutExecution) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    json note = {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
    auto resp = mcp_request(server->port(), "POST", kSecret, sid, note.dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 202);
    EXPECT_TRUE(resp->body.empty());

    // id 显式 null 的请求同样按通知受理
    json null_id = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"method", "ping"}};
    auto null_resp = mcp_request(server->port(), "POST", kSecret, sid, null_id.dump());
    ASSERT_TRUE(null_resp.has_value());
    EXPECT_EQ(null_resp->status, 202);
}

// ---- ping / tools/list / 未知方法 ----

TEST(McpServerHttpTest, PingRoundTrip) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(server->port(), "POST", kSecret, sid,
                            rpc_body(9, "ping", json::object()).dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    json body = json::parse(resp->body);
    EXPECT_TRUE(body.at("result").is_object());
}

TEST(McpServerHttpTest, UnknownMethodReturns32601) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(server->port(), "POST", kSecret, sid,
                            rpc_body(10, "resources/list", json::object()).dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    json body = json::parse(resp->body);
    EXPECT_EQ(body.at("error").at("code"), -32601);
}

// ---- tools/list 清单 schema ----

TEST(McpToolsManifestTest, ManifestSchemaShape) {
    json tools = falcon::daemon::rpc::mcp_tools_manifest();
    ASSERT_TRUE(tools.is_array());
    ASSERT_EQ(tools.size(), 13u);

    std::set<std::string> names;
    for (const auto& t : tools) {
        ASSERT_TRUE(t.contains("name") && t.at("name").is_string());
        std::string name = t.at("name").get<std::string>();
        EXPECT_TRUE(names.insert(name).second) << "duplicate tool: " << name;
        ASSERT_TRUE(t.contains("description") && t.at("description").is_string());
        EXPECT_FALSE(t.at("description").get<std::string>().empty());

        const json& schema = t.at("inputSchema");
        EXPECT_EQ(schema.at("type"), "object");
        EXPECT_EQ(schema.at("additionalProperties"), false);
        EXPECT_TRUE(schema.at("properties").is_object());
        EXPECT_TRUE(schema.at("required").is_array());

        const json& ann = t.at("annotations");
        ASSERT_TRUE(ann.contains("title") && ann.at("title").is_string());
        ASSERT_TRUE(ann.contains("readOnlyHint") && ann.at("readOnlyHint").is_boolean());
    }

    const std::set<std::string> kExpected = {
        "falcon_add_download",      "falcon_list_tasks",
        "falcon_get_task",          "falcon_pause_task",
        "falcon_resume_task",       "falcon_remove_task",
        "falcon_pause_all",         "falcon_resume_all",
        "falcon_get_global_stats",  "falcon_get_task_files",
        "falcon_get_global_option", "falcon_set_global_option",
        "falcon_stop_seeding"};
    EXPECT_EQ(names, kExpected);

    // 破坏性标注：remove / pause_all
    for (const auto& t : tools) {
        if (t.at("name") == "falcon_remove_task" || t.at("name") == "falcon_pause_all") {
            EXPECT_EQ(t.at("annotations").at("destructiveHint"), true)
                << t.at("name");
        } else {
            EXPECT_FALSE(t.at("annotations").contains("destructiveHint") &&
                         t.at("annotations").at("destructiveHint").get<bool>())
                << t.at("name");
        }
    }

    // add_download 必填 urls；list_tasks 无必填
    for (const auto& t : tools) {
        if (t.at("name") == "falcon_add_download") {
            const json& req = t.at("inputSchema").at("required");
            ASSERT_EQ(req.size(), 1u);
            EXPECT_EQ(req.at(0), "urls");
        }
        if (t.at("name") == "falcon_list_tasks") {
            EXPECT_TRUE(t.at("inputSchema").at("required").empty());
        }
    }
}

TEST(McpServerHttpTest, ToolsListReturnsManifestOverHttp) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(server->port(), "POST", kSecret, sid,
                            rpc_body(11, "tools/list", json::object()).dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    json body = json::parse(resp->body);
    ASSERT_TRUE(body.at("result").at("tools").is_array());
    EXPECT_EQ(body.at("result").at("tools").size(), 13u);
}

// ---- tools/call 翻译层 ----

TEST(McpServerHttpTest, UnknownToolReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(12, "tools/call",
                 json{{"name", "falcon_nope"}, {"arguments", json::object()}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(resp->status, 200);
    json body = json::parse(resp->body);
    EXPECT_EQ(body.at("error").at("code"), -32602);
    EXPECT_NE(body.at("error").at("message").get<std::string>().find("falcon_nope"),
              std::string::npos);
}

TEST(McpServerHttpTest, MissingRequiredArgReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    // 32602 是协议层错误：HTTP 200 + JSON-RPC error 形状（无 result），
    // 不经 call_tool（它断言 result 存在，会假红）
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(13, "tools/call", json{{"name", "falcon_get_task"}}).dump());
    ASSERT_TRUE(resp.has_value());
    json body = json::parse(resp->body);
    EXPECT_EQ(body.at("error").at("code"), -32602);
}

TEST(McpServerHttpTest, BusinessErrorMapsToIsErrorResult) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    // 不存在的 gid：aria2 业务错误 code 2（gid 不存在）→ MCP isError
    json result = call_tool(server->port(), kSecret, sid, "falcon_get_task",
                            json{{"gid", "00000000000000ff"}});
    EXPECT_EQ(result.at("isError"), true);
    ASSERT_TRUE(result.at("content").is_array() && result.at("content").size() == 1);
    const std::string text = result.at("content").at(0).at("text").get<std::string>();
    EXPECT_EQ(result.at("content").at(0).at("type"), "text");
    EXPECT_NE(text.find("Error(2)"), std::string::npos);
    EXPECT_NE(text.find("Task not found"), std::string::npos);
    EXPECT_FALSE(result.contains("structuredContent"));
}

TEST(McpServerHttpTest, AddOptionWhitelistAndConflictsSurfaceAsIsError) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    // 白名单外键 → 业务错误（isError），不进协议错误码
    json bad_option = call_tool(
        server->port(), kSecret, sid, "falcon_add_download",
        json{{"urls", json::array({"http://127.0.0.1:9/x.bin"})},
             {"options", json{{"bogus-key", 1}}}});
    EXPECT_EQ(bad_option.at("isError"), true);
    EXPECT_NE(bad_option.at("content").at(0).at("text").get<std::string>().find(
                  "Unsupported option: bogus-key"),
              std::string::npos);

    // 别名与 options.dir 冲突 → 业务错误
    json conflict = call_tool(
        server->port(), kSecret, sid, "falcon_add_download",
        json{{"urls", json::array({"http://127.0.0.1:9/x.bin"})},
             {"output_dir", "/tmp/a"},
             {"options", json{{"dir", "/tmp/b"}}}});
    EXPECT_EQ(conflict.at("isError"), true);
    EXPECT_NE(conflict.at("content").at(0).at("text").get<std::string>().find(
                  "Conflicting arguments"),
              std::string::npos);

    // 参数形状错误（urls 缺失）→ 协议错误 -32602
    auto shape = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(14, "tools/call",
                 json{{"name", "falcon_add_download"}, {"arguments", json::object()}})
            .dump());
    ASSERT_TRUE(shape.has_value());
    EXPECT_EQ(json::parse(shape->body).at("error").at("code"), -32602);
}

/// test:// 协议的阻塞 handler（json_rpc_storage_test.cpp 同款形态）。
/// 测试二进制不引用 protocols 符号时 GNU ld 归档单次扫描会让 core 的
/// weak 空 stub 生效（load_builtin_handlers 注册 0 个 handler），任何
/// http:// URL 都抛 UnsupportedProtocolException——注册自定义 handler
/// 既有确定性又规避该链接陷阱。download 阻塞至任务终态/暂停。
class BlockingTestHandler final : public falcon::IProtocolHandler {
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
        info.filename = "lifecycle.bin";
        info.total_size = 1000;
        info.supports_resume = true;
        return info;
    }
    void download(falcon::DownloadTask::Ptr task, falcon::IEventListener*) override {
        running_.fetch_add(1);
        while (!task->is_finished() && task->status() != falcon::TaskStatus::Paused)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
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

    void wait_until_idle() {
        while (running_.load() > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

private:
    std::atomic<int> running_{0};
};

TEST(McpServerHttpTest, DownloadLifecycleOverTools) {
    falcon::DownloadEngine engine;
    auto handler = std::make_unique<BlockingTestHandler>();
    BlockingTestHandler* handler_ptr = handler.get();
    engine.register_handler(std::move(handler));
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    const std::string url = "test://lifecycle.bin";

    // add → gid（structuredContent 直通字符串）
    json added = call_tool(server->port(), kSecret, sid, "falcon_add_download",
                           json{{"urls", json::array({url})}});
    ASSERT_FALSE(added.value("isError", false)) << added.dump();
    const std::string gid = added.at("structuredContent").get<std::string>();
    EXPECT_FALSE(gid.empty());

    // list(all) 含该任务
    json listing = call_tool(server->port(), kSecret, sid, "falcon_list_tasks",
                             json{{"status", "all"}});
    ASSERT_FALSE(listing.value("isError", false)) << listing.dump();
    const json& tasks = listing.at("structuredContent").at("tasks");
    ASSERT_TRUE(tasks.is_array());
    bool found = false;
    for (const auto& t : tasks)
        if (t.at("gid").get<std::string>() == gid) found = true;
    EXPECT_TRUE(found) << listing.dump();
    EXPECT_GE(listing.at("structuredContent").at("total").get<int>(), 1);

    // get → tellStatus 快照
    json status = call_tool(server->port(), kSecret, sid, "falcon_get_task",
                            json{{"gid", gid}});
    ASSERT_FALSE(status.value("isError", false)) << status.dump();
    EXPECT_EQ(status.at("structuredContent").at("gid"), gid);
    EXPECT_TRUE(status.at("structuredContent").contains("status"));

    // files → getFiles 数组
    json files = call_tool(server->port(), kSecret, sid, "falcon_get_task_files",
                           json{{"gid", gid}});
    ASSERT_FALSE(files.value("isError", false)) << files.dump();
    EXPECT_TRUE(files.at("structuredContent").is_array());

    // remove（活动任务默认路径即 aria2.remove → cancel，成功）→ gid 回显
    json removed = call_tool(server->port(), kSecret, sid, "falcon_remove_task",
                             json{{"gid", gid}});
    ASSERT_FALSE(removed.value("isError", false)) << removed.dump();
    EXPECT_EQ(removed.at("structuredContent"), gid);

    handler_ptr->wait_until_idle();

    // 业务错误映射：不存在的 gid → dispatch 返回 {error:{code:2}} →
    // isError=true + content[0] 携带 "Error(2)"（本测试无 storage，
    // remove 后任务以 Cancelled 留存引擎内存，故「移除后查 gid」
    // 不构成业务错误——用必然不存在的 gid 钉 isError 分支）
    json gone = call_tool(server->port(), kSecret, sid, "falcon_get_task",
                          json{{"gid", std::string("ffffffffffffffff")}});
    EXPECT_EQ(gone.at("isError"), true);
    ASSERT_TRUE(gone.at("content").is_array() && !gone.at("content").empty());
    EXPECT_NE(gone.at("content").at(0).at("text").get<std::string>()
                  .find("Error(2)"),
              std::string::npos)
        << gone.dump();
}

TEST(McpServerHttpTest, GlobalOptionToolsRoundTrip) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    // get：全量选项表（aria2 形状，字符串值）
    json got = call_tool(server->port(), kSecret, sid, "falcon_get_global_option",
                         json::object());
    ASSERT_FALSE(got.value("isError", false)) << got.dump();
    const json& opts = got.at("structuredContent");
    ASSERT_TRUE(opts.is_object());
    ASSERT_TRUE(opts.at("max-overall-download-limit").is_string());
    ASSERT_TRUE(opts.at("max-concurrent-downloads").is_string());
    ASSERT_TRUE(opts.contains("dir"));

    // set：整数并发数 → "OK" → get 回读生效
    json set = call_tool(server->port(), kSecret, sid, "falcon_set_global_option",
                         json{{"options", json{{"max-concurrent-downloads", 3}}}});
    ASSERT_FALSE(set.value("isError", false)) << set.dump();
    EXPECT_EQ(set.at("structuredContent"), "OK");
    json reread = call_tool(server->port(), kSecret, sid, "falcon_get_global_option",
                            json::object());
    EXPECT_EQ(reread.at("structuredContent").at("max-concurrent-downloads"), "3");

    // 限速键位：字符串 "none" = 不限速（合法值不报错）
    json limit_off = call_tool(server->port(), kSecret, sid, "falcon_set_global_option",
                               json{{"options",
                                     json{{"max-overall-download-limit", "none"}}}});
    ASSERT_FALSE(limit_off.value("isError", false)) << limit_off.dump();
    EXPECT_EQ(limit_off.at("structuredContent"), "OK");

    // 白名单外键位（dir 只可查不可设）→ daemon 业务错误 code 1 → isError
    json unsupported = call_tool(server->port(), kSecret, sid,
                                 "falcon_set_global_option",
                                 json{{"options", json{{"dir", "/tmp"}}}});
    EXPECT_EQ(unsupported.at("isError"), true);
    EXPECT_NE(unsupported.at("content").at(0).at("text").get<std::string>()
                  .find("Option not supported: dir"),
              std::string::npos)
        << unsupported.dump();

    // 非法值（非数字限速）→ 业务错误 "Invalid option value"
    json invalid = call_tool(server->port(), kSecret, sid, "falcon_set_global_option",
                             json{{"options",
                                   json{{"max-overall-download-limit", "abc"}}}});
    EXPECT_EQ(invalid.at("isError"), true);
    EXPECT_NE(invalid.at("content").at(0).at("text").get<std::string>()
                  .find("Invalid option value: max-overall-download-limit"),
              std::string::npos)
        << invalid.dump();

    // 缺 options → 协议层 -32602（不经 call_tool，它断言 result 存在）
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(30, "tools/call",
                 json{{"name", "falcon_set_global_option"},
                      {"arguments", json::object()}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602);
}

TEST(McpServerHttpTest, StopSeedingToolMapsSeedErrors) {
    falcon::DownloadEngine engine;
    auto handler = std::make_unique<BlockingTestHandler>();
    BlockingTestHandler* handler_ptr = handler.get();
    engine.register_handler(std::move(handler));
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    // 非做种任务：get_seed_info 默认 seeding_active=false → 业务错误 code 1
    json added = call_tool(server->port(), kSecret, sid, "falcon_add_download",
                           json{{"urls", json::array({"test://seed.bin"})}});
    ASSERT_FALSE(added.value("isError", false)) << added.dump();
    const std::string gid = added.at("structuredContent").get<std::string>();

    json not_seeding = call_tool(server->port(), kSecret, sid, "falcon_stop_seeding",
                                 json{{"gid", gid}});
    EXPECT_EQ(not_seeding.at("isError"), true);
    ASSERT_TRUE(not_seeding.at("content").is_array() &&
                !not_seeding.at("content").empty());
    const std::string text =
        not_seeding.at("content").at(0).at("text").get<std::string>();
    EXPECT_NE(text.find("Error(1)"), std::string::npos) << text;
    EXPECT_NE(text.find("Task is not seeding"), std::string::npos) << text;

    // gid 不存在（合法 16 hex 形态）→ 业务错误 code 2
    json gone = call_tool(server->port(), kSecret, sid, "falcon_stop_seeding",
                          json{{"gid", std::string("ffffffffffffffff")}});
    EXPECT_EQ(gone.at("isError"), true);
    EXPECT_NE(gone.at("content").at(0).at("text").get<std::string>().find("Error(2)"),
              std::string::npos)
        << gone.dump();

    // 非法 gid 形态 → 协议层 -32602
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(31, "tools/call",
                 json{{"name", "falcon_stop_seeding"}, {"arguments", json::object()}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602);

    // 收尾：暂停放行阻塞的 download 线程再汇合（沿 DownloadLifecycleOverTools
    // 的既有约定——任务留在 Downloading 会让 wait_until_idle 永等）
    json paused = call_tool(server->port(), kSecret, sid, "falcon_pause_task",
                            json{{"gid", gid}});
    ASSERT_FALSE(paused.value("isError", false)) << paused.dump();
    handler_ptr->wait_until_idle();
}

TEST(McpServerHttpTest, ListTasksInvalidStatusReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(15, "tools/call",
                 json{{"name", "falcon_list_tasks"},
                      {"arguments", json{{"status", "bogus"}}}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602);
}

TEST(McpServerHttpTest, GlobalStatsRoundTrip) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    json stats = call_tool(server->port(), kSecret, sid, "falcon_get_global_stats",
                           json::object());
    ASSERT_FALSE(stats.value("isError", false)) << stats.dump();
    // getGlobalStat 返回对象（aria2 形状：字符串数字字段）
    EXPECT_TRUE(stats.at("structuredContent").is_object());
}

TEST(McpServerHttpTest, SessionTableEvictsOldestAtCapacity) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);

    std::string oldest;
    for (int i = 0; i < 70; ++i) {
        std::string sid = open_session(server->port(), kSecret);
        ASSERT_FALSE(sid.empty());
        if (i == 0) oldest = sid;
    }

    // 最旧会话被 FIFO 淘汰 → 404
    auto stale = mcp_request(server->port(), "POST", kSecret, oldest,
                             rpc_body(16, "ping", json::object()).dump());
    ASSERT_TRUE(stale.has_value());
    EXPECT_EQ(stale->status, 404);
}
