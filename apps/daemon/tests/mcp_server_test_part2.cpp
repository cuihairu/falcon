/// MCP Streamable HTTP 端点测试（协议契约 + 工具翻译层）。
/// 设计文档 docs/developer/mcp-server.md §2 契约逐条钉住：
/// 认证分级（403/401）、initialize 会话握手、会话校验 404、
/// 通知 202、DELETE 拆除 204、未知方法 405、GET/SSE 通知流
///（认证与会话准入 + data 行扇出 + 停机/断开收尾，阶段 2 增量 3）、
/// 解析/批量错误分级、tools/list 清单 schema、tools/call 翻译与
/// isError 映射。

#include "rpc/json_rpc_server.hpp"
#include "rpc/mcp_server.hpp"

#include <csignal>
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
/// 本文件回环基建含「写已断连 socket」剧本（客户端失败收口主动 abort
/// 连接、服务器按剧本 drop 短 body/超限请求、超时清理关 fd 后仍有在途
/// 写）——POSIX 默认对 socket 写的 SIGPIPE 做进程级终止，任一写命中
/// RST 即杀死整个测试进程（RangeLiarServer / TlsTestServer /
/// ProxyTestServer 同族红面先例）。进程级忽略让写失败以 EPIPE 返回
/// 值出现（各 send_all 的 n<=0 分支既有吞错路径正常收口）；Windows 无
/// SIGPIPE 语义无需处理
struct SigpipeImmunity {
    SigpipeImmunity() {
#if !defined(_WIN32)
        ::signal(SIGPIPE, SIG_IGN);
#endif
    }
};

const SigpipeImmunity falcon_test_sigpipe_immunity;

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

// ===========================================================================
// falcon_list_tasks 边界：limit/offset 校验、dispatch 非数组错误
// ===========================================================================

// 非数字 / 负 limit 与 offset → 协议层 -32602。
TEST(McpServerHttpTest, ListTasksInvalidLimitOffsetReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    for (const auto& bad : {json("abc"), json(-1), json(1.5), json(true)}) {
        auto resp = mcp_request(
            server->port(), "POST", kSecret, sid,
            rpc_body(17, "tools/call",
                     json{{"name", "falcon_list_tasks"},
                          {"arguments", json{{"limit", bad}}}})
                .dump());
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602)
            << bad.dump();
    }
    for (const auto& bad : {json("x"), json(-2)}) {
        auto resp = mcp_request(
            server->port(), "POST", kSecret, sid,
            rpc_body(18, "tools/call",
                     json{{"name", "falcon_list_tasks"},
                          {"arguments", json{{"offset", bad}}}})
                .dump());
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602)
            << bad.dump();
    }
}

// status 非字符串 → -32602。
TEST(McpServerHttpTest, ListTasksStatusNonStringReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(19, "tools/call",
                 json{{"name", "falcon_list_tasks"},
                      {"arguments", json{{"status", 42}}}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602);
}

// falcon_add_download 参数形状错误：urls 非数组 / 空数组 / 含非字符串、
// output_dir 非字符串、filename 非字符串、options 非对象。
TEST(McpServerHttpTest, AddDownloadParamShapeErrorsReturn32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);

    const std::vector<std::pair<const char*, json>> cases = {
        {"urls", json("not-an-array")},
        {"urls", json::array()},
        {"urls", json::array({42})},
        {"output_dir", json(7)},
        {"filename", json(9)},
    };
    for (const auto& [key, val] : cases) {
        json args = json::object();
        if (std::string(key) == "urls") args["urls"] = val;
        else {
            args["urls"] = json::array({"http://127.0.0.1:9/x.bin"});
            args[key] = val;
        }
        auto resp = mcp_request(
            server->port(), "POST", kSecret, sid,
            rpc_body(20, "tools/call",
                     json{{"name", "falcon_add_download"}, {"arguments", args}})
                .dump());
        ASSERT_TRUE(resp.has_value()) << key;
        EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602)
            << key << ": " << resp->body;
    }

    // options 非对象
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(21, "tools/call",
                 json{{"name", "falcon_add_download"},
                      {"arguments", json{{"urls", json::array({"http://x"})},
                                         {"options", "not-object"}}}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602);

    // filename 与 options.out 冲突 → 业务错误 isError。
    json conflict = call_tool(
        server->port(), kSecret, sid, "falcon_add_download",
        json{{"urls", json::array({"http://127.0.0.1:9/x.bin"})},
             {"filename", "/tmp/a"},
             {"options", json{{"out", "/tmp/b"}}}});
    EXPECT_EQ(conflict.at("isError"), true);
    EXPECT_NE(conflict.at("content").at(0).at("text").get<std::string>().find(
                  "Conflicting arguments"),
              std::string::npos);
}

// falcon_remove_task 的 force 参数非布尔 → -32602；force=true 走强拆。
TEST(McpServerHttpTest, RemoveTaskForceNonBooleanReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    auto resp = mcp_request(
        server->port(), "POST", kSecret, sid,
        rpc_body(22, "tools/call",
                 json{{"name", "falcon_remove_task"},
                      {"arguments", json{{"gid", "0000000000000001"},
                                         {"force", "yes"}}}})
            .dump());
    ASSERT_TRUE(resp.has_value());
    EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602);
}

// falcon_set_global_option 的 options 缺失 / 非对象 → -32602。
TEST(McpServerHttpTest, SetGlobalOptionBadShapeReturns32602) {
    falcon::DownloadEngine engine;
    auto server = make_server(engine);
    std::string sid = open_session(server->port(), kSecret);
    for (const auto& args : {json::object(), json{{"options", "nope"}},
                             json{{"options", json::array()}}}) {
        auto resp = mcp_request(
            server->port(), "POST", kSecret, sid,
            rpc_body(23, "tools/call",
                     json{{"name", "falcon_set_global_option"},
                          {"arguments", args}})
                .dump());
        ASSERT_TRUE(resp.has_value());
        EXPECT_EQ(json::parse(resp->body).at("error").at("code"), -32602)
            << args.dump();
    }
}


// ---- 直接调用 handle_request：mock dispatch 返回非数组响应分支 ----

// falcon_list_tasks 的 dispatch 返回非数组 → 业务错误（Unexpected non-array
// response），覆盖 translate_tool_call 的 append 容错分支。
TEST(McpServerDirectTest, ListTasksNonArrayDispatchYieldsBusinessError) {
    falcon::daemon::rpc::McpServer server;

    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    // initialize 建立会话
    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    auto it = r.headers.find("Mcp-Session-Id");
    ASSERT_TRUE(it != r.headers.end());
    const std::string sid = it->second;

    // dispatch 对 tellActive 返回非数组 → 容错分支
    auto mock_dispatch = [](const std::string& method, json) -> json {
        if (method == "aria2.tellActive") return json{{"weird", "object"}};
        return json::array();
    };
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"falcon_list_tasks","arguments":{"status":"active"}}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    json body = json::parse(r.body);
    ASSERT_TRUE(body.contains("result"));
    EXPECT_EQ(body["result"].at("isError"), true);
    const std::string text = body["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("non-array"), std::string::npos) << text;
}

// falcon_list_tasks 的 dispatch 返回 {"error":...} → 直接回传业务错误。
TEST(McpServerDirectTest, ListTasksDispatchErrorPropagates) {
    falcon::daemon::rpc::McpServer server;

    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    auto it = r.headers.find("Mcp-Session-Id");
    ASSERT_TRUE(it != r.headers.end());
    const std::string sid = it->second;

    auto mock_dispatch = [](const std::string& method, json) -> json {
        if (method == "aria2.tellActive")
            return json{{"error", json{{"code", 2}, {"message", "boom"}}}};
        return json::array();
    };
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"falcon_list_tasks","arguments":{"status":"active"}}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    json body = json::parse(r.body);
    ASSERT_TRUE(body.contains("result"));
    EXPECT_EQ(body["result"].at("isError"), true);
    const std::string text = body["result"]["content"][0]["text"].get<std::string>();
    EXPECT_NE(text.find("boom"), std::string::npos) << text;
}


// falcon_list_tasks status=all：三队列 dispatch 全成功 → 合并后切片。
TEST(McpServerDirectTest, ListTasksAllQueuesMergeAndSlice) {
    falcon::daemon::rpc::McpServer server;
    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    const std::string sid = r.headers.at("Mcp-Session-Id");

    auto mock_dispatch = [](const std::string& method, json) -> json {
        if (method == "aria2.tellActive") return json::array({json{{"gid","a"}}});
        if (method == "aria2.tellWaiting") return json::array({json{{"gid","w"}}});
        if (method == "aria2.tellStopped") return json::array({json{{"gid","s"}}});
        return json::array();
    };
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"falcon_list_tasks","arguments":{"status":"all","limit":2}}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    json body = json::parse(r.body);
    ASSERT_TRUE(body.contains("result"));
    const json res = body["result"].at("structuredContent");
    EXPECT_EQ(res.at("total"), 3);
    ASSERT_TRUE(res.contains("tasks"));
    EXPECT_EQ(res["tasks"].size(), 2u);
}

// falcon_remove_task force=true：dispatch 路由到 forceRemove。
TEST(McpServerDirectTest, RemoveTaskForceRoutesToForceRemove) {
    falcon::daemon::rpc::McpServer server;
    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    const std::string sid = r.headers.at("Mcp-Session-Id");

    std::string called_method;
    auto mock_dispatch = [&called_method](const std::string& method, json) -> json {
        called_method = method;
        return json{{"result", "removed"}};
    };
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"falcon_remove_task","arguments":{"gid":"0000000000000001","force":true}}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    EXPECT_EQ(called_method, "aria2.forceRemove");
}

// falcon_remove_task force 缺省：dispatch 路由到 remove（非强拆）。
TEST(McpServerDirectTest, RemoveTaskDefaultRoutesToRemove) {
    falcon::daemon::rpc::McpServer server;
    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    const std::string sid = r.headers.at("Mcp-Session-Id");

    std::string called_method;
    auto mock_dispatch = [&called_method](const std::string& method, json) -> json {
        called_method = method;
        return json{{"result", "removed"}};
    };
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"falcon_remove_task","arguments":{"gid":"0000000000000001"}}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    EXPECT_EQ(called_method, "aria2.remove");
}

// falcon_pause_all / falcon_resume_all / falcon_get_global_stats：无参工具
// 直接路由到对应 aria2 dispatch，不要求 arguments。
TEST(McpServerDirectTest, NoArgControlToolsRouteToDispatch) {
    falcon::daemon::rpc::McpServer server;
    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    const std::string sid = r.headers.at("Mcp-Session-Id");

    const std::vector<std::pair<std::string, std::string>> cases = {
        {"falcon_pause_all", "aria2.pauseAll"},
        {"falcon_resume_all", "aria2.unpauseAll"},
        {"falcon_get_global_stats", "aria2.getGlobalStat"},
    };
    int64_t id = 100;
    for (const auto& [tool, expected] : cases) {
        std::string called_method;
        auto mock_dispatch = [&called_method](const std::string& method, json) -> json {
            called_method = method;
            return json{{"result", "ok"}};
        };
        Req call;
        call.method = "POST";
        call.headers["authorization"] = "Bearer " + token;
        call.headers["mcp-session-id"] = sid;
        const std::string body =
            R"({"jsonrpc":"2.0","id":)" + std::to_string(++id) +
            R"(,"method":"tools/call","params":{"name":")" + tool + R"("}})";
        call.body = body;
        r = server.handle_request(call, token, mock_dispatch);
        ASSERT_EQ(r.status_code, 200);
        EXPECT_EQ(called_method, expected) << tool;
        EXPECT_NE(r.body.find("result"), std::string::npos) << tool;
    }
}

// tools/call 参数形状错误：name 缺失/非字符串、arguments 非对象 →
// 协议层 -32602（HTTP 200，无 result）。
TEST(McpServerDirectTest, ToolsCallParamShapeErrorsReturn32602) {
    falcon::daemon::rpc::McpServer server;
    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    const std::string sid = r.headers.at("Mcp-Session-Id");

    auto mock_dispatch = [](const std::string&, json) -> json {
        return json::object();
    };

    // name 缺失
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    EXPECT_NE(r.body.find("-32602"), std::string::npos) << r.body;

    // name 非字符串
    call.body = R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":42}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    EXPECT_NE(r.body.find("-32602"), std::string::npos) << r.body;

    // arguments 非对象
    call.body = R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"falcon_get_task","arguments":"not-an-object"}})";
    r = server.handle_request(call, token, mock_dispatch);
    ASSERT_EQ(r.status_code, 200);
    EXPECT_NE(r.body.find("-32602"), std::string::npos) << r.body;
}

// 通知（id 为 null）→ 202 Accepted：无方法（缺 method）与有方法两种
// 形态都受理不执行（MCP 规范：客户端通知无返回语义）。
TEST(McpServerDirectTest, NotificationAlwaysAccepted202) {
    falcon::daemon::rpc::McpServer server;
    using Req = falcon::daemon::rpc::McpServer::Request;
    using Resp = falcon::daemon::rpc::McpServer::Response;
    const std::string token = "direct-secret";

    Req init;
    init.method = "POST";
    init.headers["authorization"] = "Bearer " + token;
    init.body = R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"direct","version":"0"}}})";
    Resp r = server.handle_request(init, token,
        [](const std::string&, json) { return json::object(); });
    ASSERT_EQ(r.status_code, 200);
    const std::string sid = r.headers.at("Mcp-Session-Id");

    auto mock_dispatch = [](const std::string&, json) -> json {
        return json::object();
    };

    // 通知 + 缺失 method → 202
    Req call;
    call.method = "POST";
    call.headers["authorization"] = "Bearer " + token;
    call.headers["mcp-session-id"] = sid;
    call.body = R"({"jsonrpc":"2.0","id":null,"params":{}})";
    r = server.handle_request(call, token, mock_dispatch);
    EXPECT_EQ(r.status_code, 202);

    // 通知 + 有效 method（tools/call）→ 202，不执行 dispatch
    bool dispatch_called = false;
    auto counting_dispatch = [&dispatch_called](const std::string&, json) -> json {
        dispatch_called = true;
        return json::object();
    };
    call.body = R"({"jsonrpc":"2.0","id":null,"method":"tools/call","params":{"name":"falcon_get_task","arguments":{"gid":"1"}}})";
    r = server.handle_request(call, token, counting_dispatch);
    EXPECT_EQ(r.status_code, 202);
    EXPECT_FALSE(dispatch_called);
}
