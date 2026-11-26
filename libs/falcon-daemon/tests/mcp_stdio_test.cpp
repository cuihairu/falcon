/// MCP stdio 薄壳测试（阶段 2 增量 2）。
/// 两层：① fake HttpPostFn 单测钉桥逻辑（会话捕获/附带、404 清除、
/// Bearer 附带、通知 202 静默、传输失败信封、DELETE 拆除、空行/CR
/// 容错）；② 真 curl 经回环 daemon（JsonRpcServer + McpServer）端到
/// 端——initialize → tools/list → ping 三往返 + EOF DELETE 拆除的
/// 服务器侧证据（拆除后再请求得 404 错误体透传）。

#include "mcp_curl_post.hpp"
#include "mcp_stdio.hpp"
#include "rpc/json_rpc_server.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <falcon/download_task.hpp>
#include <falcon/protocol_handler.hpp>

#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using json = nlohmann::json;
using falcon::daemon::mcpstdio::HttpResult;
using falcon::daemon::mcpstdio::HttpPostFn;
using falcon::daemon::mcpstdio::McpStdioBridge;
using falcon::daemon::mcpstdio::McpStdioConfig;

/// fake 传输：记录全部调用，按剧本逐个应答；剧本耗尽后回 200 空体
struct Recorder {
    struct Call {
        std::string path;
        std::string method;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };

    std::vector<Call> calls;
    std::vector<HttpResult> script;
    size_t next = 0;

    HttpPostFn fn() {
        return [this](const std::string& path,
                      const std::vector<std::pair<std::string, std::string>>& headers,
                      const std::string& body,
                      const std::string& method) -> HttpResult {
            calls.push_back({path, method, headers, body});
            if (next < script.size()) return script[next++];
            return HttpResult{200, "", ""};
        };
    }

    const std::string* header(const Call& c, const std::string& name) const {
        for (const auto& h : c.headers)
            if (h.first == name) return &h.second;
        return nullptr;
    }
};

static std::string req_msg(int id, const char* method, json params) {
    return json{{"jsonrpc", "2.0"},
                {"id", id},
                {"method", method},
                {"params", std::move(params)}}
        .dump();
}

static std::string notification_msg(const char* method) {
    return json{{"jsonrpc", "2.0"}, {"method", method}}.dump();
}

static std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        out.push_back(line);
    }
    return out;
}

// ---- 桥层单测（fake 传输） ----

TEST(McpStdioBridgeTest, RunForwardsLinesAndCapturesSession) {
    Recorder rec;
    rec.script.push_back({200, R"({"jsonrpc":"2.0","id":1,"result":{}})", "sess-abc"});
    McpStdioConfig config;
    McpStdioBridge bridge(config, rec.fn());

    std::istringstream in(req_msg(1, "initialize", json::object()) + "\n" +
                          req_msg(2, "tools/list", json::object()) + "\n");
    std::ostringstream out;
    bridge.run(in, out);

    // 两次 POST + EOF 触发的 shutdown DELETE（会话非空）
    ASSERT_EQ(rec.calls.size(), 3u);
    EXPECT_EQ(rec.calls[0].path, "/mcp");
    EXPECT_EQ(rec.calls[0].method, "POST");
    EXPECT_EQ(rec.calls[0].body, req_msg(1, "initialize", json::object()));
    // initialize 请求不带会话头；响应会话头被捕获后，后续请求附带
    EXPECT_EQ(rec.header(rec.calls[0], "Mcp-Session-Id"), nullptr);
    ASSERT_NE(rec.header(rec.calls[1], "Mcp-Session-Id"), nullptr);
    EXPECT_EQ(*rec.header(rec.calls[1], "Mcp-Session-Id"), "sess-abc");
    EXPECT_EQ(rec.calls[2].method, "DELETE");
    EXPECT_EQ(*rec.header(rec.calls[2], "Mcp-Session-Id"), "sess-abc");

    // initialize 响应体透传；第二个响应为空体（剧本耗尽）不输出
    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], rec.script[0].body);
}

TEST(McpStdioBridgeTest, BearerAndStaticHeadersAttached) {
    Recorder rec;
    McpStdioConfig config;
    config.secret = "tok";
    McpStdioBridge bridge(config, rec.fn());

    std::ostringstream out;
    std::istringstream in(req_msg(1, "ping", json::object()) + "\n");
    bridge.run(in, out);

    ASSERT_EQ(rec.calls.size(), 1u);
    const std::string* auth = rec.header(rec.calls[0], "Authorization");
    ASSERT_NE(auth, nullptr);
    EXPECT_EQ(*auth, "Bearer tok");
    const std::string* ct = rec.header(rec.calls[0], "Content-Type");
    ASSERT_NE(ct, nullptr);
    EXPECT_EQ(*ct, "application/json");
    const std::string* accept = rec.header(rec.calls[0], "Accept");
    ASSERT_NE(accept, nullptr);
    EXPECT_EQ(*accept, "application/json, text/event-stream");

    // 未配 secret：无 Authorization 头
    Recorder rec2;
    McpStdioBridge bridge2(McpStdioConfig{}, rec2.fn());
    std::ostringstream out2;
    std::istringstream in2(req_msg(1, "ping", json::object()) + "\n");
    bridge2.run(in2, out2);
    ASSERT_EQ(rec2.calls.size(), 1u);
    EXPECT_EQ(rec2.header(rec2.calls[0], "Authorization"), nullptr);
}

TEST(McpStdioBridgeTest, Notification202ProducesNoOutput) {
    Recorder rec;
    rec.script.push_back({202, "", ""});
    McpStdioBridge bridge(McpStdioConfig{}, rec.fn());

    std::ostringstream out;
    std::istringstream in(notification_msg("notifications/initialized") + "\n");
    bridge.run(in, out);

    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].method, "POST");
    EXPECT_TRUE(out.str().empty());
}

TEST(McpStdioBridgeTest, NotFoundClearsSessionForSubsequentRequests) {
    Recorder rec;
    rec.script.push_back({200, R"({"jsonrpc":"2.0","id":1,"result":{}})", "sess-1"});
    rec.script.push_back(
        {404, R"({"error":"unknown or expired MCP session; re-initialize"})", ""});
    McpStdioBridge bridge(McpStdioConfig{}, rec.fn());

    std::ostringstream out;
    std::istringstream in(req_msg(1, "ping", json::object()) + "\n" +
                          req_msg(2, "ping", json::object()) + "\n" +
                          req_msg(3, "ping", json::object()) + "\n");
    bridge.run(in, out);

    ASSERT_EQ(rec.calls.size(), 3u);
    ASSERT_NE(rec.header(rec.calls[1], "Mcp-Session-Id"), nullptr);
    // 404 后会话清空：第三个请求不再携带旧会话头
    EXPECT_EQ(rec.header(rec.calls[2], "Mcp-Session-Id"), nullptr);

    // 404 错误体透传给 host
    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 2u);
    json forwarded = json::parse(lines[1]);
    ASSERT_TRUE(forwarded.contains("error"));
    EXPECT_EQ(forwarded.at("error").get<std::string>(),
              "unknown or expired MCP session; re-initialize");
}

TEST(McpStdioBridgeTest, TransportFailureEnvelopeForRequestNotForNotification) {
    Recorder rec;
    rec.script.push_back({0, "", ""});
    rec.script.push_back({0, "", ""});
    McpStdioConfig config;
    config.rpc_url = "http://10.1.2.3:6800";
    McpStdioBridge bridge(config, rec.fn());

    std::ostringstream out;
    std::istringstream in(req_msg(5, "tools/list", json::object()) + "\n" +
                          notification_msg("notifications/cancelled") + "\n");
    bridge.run(in, out);

    ASSERT_EQ(rec.calls.size(), 2u);
    // 带 id 的请求：合成 -32603 信封，id 原样回填
    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 1u);
    json envelope = json::parse(lines[0]);
    EXPECT_EQ(envelope.at("jsonrpc").get<std::string>(), "2.0");
    EXPECT_EQ(envelope.at("id").get<int>(), 5);
    EXPECT_EQ(envelope.at("error").at("code").get<int>(), -32603);
    const std::string message = envelope.at("error").at("message").get<std::string>();
    EXPECT_NE(message.find("http://10.1.2.3:6800"), std::string::npos);
    // 通知（无 id）：静默
}

TEST(McpStdioBridgeTest, ShutdownDeletesSessionBestEffort) {
    Recorder rec;
    rec.script.push_back({200, R"({"jsonrpc":"2.0","id":1,"result":{}})", "sess-9"});
    McpStdioBridge bridge(McpStdioConfig{}, rec.fn());

    std::ostringstream out;
    std::istringstream in(req_msg(1, "ping", json::object()) + "\n");
    bridge.run(in, out);  // EOF → shutdown()

    ASSERT_GE(rec.calls.size(), 2u);
    const Recorder::Call& del = rec.calls.back();
    EXPECT_EQ(del.method, "DELETE");
    EXPECT_TRUE(del.body.empty());
    const std::string* sid = rec.header(del, "Mcp-Session-Id");
    ASSERT_NE(sid, nullptr);
    EXPECT_EQ(*sid, "sess-9");

    // 拆除后会话已清：再处理消息不带旧头
    bridge.handle_message(req_msg(2, "ping", json::object()));
    ASSERT_EQ(rec.calls.size(), 3u);
    EXPECT_EQ(rec.header(rec.calls[2], "Mcp-Session-Id"), nullptr);

    // 无会话的 shutdown 是无操作
    Recorder rec2;
    McpStdioBridge fresh(McpStdioConfig{}, rec2.fn());
    fresh.shutdown();
    EXPECT_TRUE(rec2.calls.empty());
}

TEST(McpStdioBridgeTest, BlankLinesAndCarriageReturnTolerated) {
    Recorder rec;
    McpStdioBridge bridge(McpStdioConfig{}, rec.fn());

    std::ostringstream out;
    std::istringstream in("\n   \t  \n" + req_msg(1, "ping", json::object()) + "\r\n");
    bridge.run(in, out);

    ASSERT_EQ(rec.calls.size(), 1u);
    // 行尾 CR 已剥：转发体是干净的单行 JSON
    EXPECT_EQ(rec.calls[0].body, req_msg(1, "ping", json::object()));
}

TEST(McpStdioBridgeTest, NonJsonGarbageLineForwardedNotSynthesized) {
    // 非 JSON 行照常转发（daemon 校验请求形状），失败信封只在传输
    // 失败且带 id 时合成——本壳不预校验消息体（薄适配）。
    Recorder rec;
    McpStdioBridge bridge(McpStdioConfig{}, rec.fn());

    std::ostringstream out;
    std::istringstream in("not-json at all\n");
    bridge.run(in, out);

    ASSERT_EQ(rec.calls.size(), 1u);
    EXPECT_EQ(rec.calls[0].body, "not-json at all");
    // fake 返回 200 空体 → 无输出
    EXPECT_TRUE(out.str().empty());
}

// ---- curl 回环 e2e（真实 daemon 服务器） ----

static std::unique_ptr<falcon::daemon::rpc::JsonRpcServer> make_loopback_server(
    falcon::DownloadEngine& engine) {
    falcon::daemon::rpc::JsonRpcServerConfig cfg;
    cfg.listen_port = 0;
    cfg.secret = "s3cr3t";
    cfg.mcp_enabled = true;
    auto server = std::make_unique<falcon::daemon::rpc::JsonRpcServer>(&engine, cfg);
    EXPECT_TRUE(server->start());
    EXPECT_NE(server->port(), 0);
    return server;
}

TEST(McpStdioCurlLoopbackTest, EndToEndThroughRealCurlAndDaemon) {
    falcon::DownloadEngine engine;
    auto server = make_loopback_server(engine);

    McpStdioConfig config;
    config.rpc_url = "http://127.0.0.1:" + std::to_string(server->port());
    config.secret = "s3cr3t";
    McpStdioBridge bridge(config, make_curl_http_post(config));

    std::ostringstream out;
    std::istringstream in(req_msg(1, "initialize",
                                  json{{"protocolVersion", "2025-06-18"},
                                       {"capabilities", json::object()},
                                       {"clientInfo",
                                        {{"name", "falcon-mcp-test"}, {"version", "0"}}}}) +
                          "\n" +
                          req_msg(2, "tools/list", json::object()) + "\n" +
                          req_msg(3, "ping", json::object()) + "\n");
    bridge.run(in, out);

    // 三往返逐行可解析：会话捕获/附带经真 curl + 真 daemon 全链生效
    //（tools/list 在 initialize 之后的请求上若未附带会话头必然 404）
    const auto lines = split_lines(out.str());
    ASSERT_EQ(lines.size(), 3u);
    const json r1 = json::parse(lines[0]);
    EXPECT_EQ(r1.at("result").at("serverInfo").at("name").get<std::string>(), "falcon");
    const json r2 = json::parse(lines[1]);
    EXPECT_EQ(r2.at("result").at("tools").size(), 13u);
    const json r3 = json::parse(lines[2]);
    EXPECT_EQ(r3.at("result"), json::object());

    // EOF 已触发 DELETE：会话在服务器侧拆除——再请求得 404 错误体
    // 透传（若 DELETE 未生效，会话仍有效则这里会是成功 result）
    std::ostringstream out2;
    std::istringstream in2(req_msg(4, "tools/list", json::object()) + "\n");
    bridge.run(in2, out2);
    const auto lines2 = split_lines(out2.str());
    ASSERT_EQ(lines2.size(), 1u);
    const json stale = json::parse(lines2[0]);
    ASSERT_TRUE(stale.contains("error"));
    const std::string stale_error = stale.at("error").get<std::string>();
    EXPECT_NE(stale_error.find("re-initialize"), std::string::npos);
}

} // namespace
