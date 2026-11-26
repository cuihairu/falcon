#include "mcp_stdio.hpp"

#include <algorithm>
#include <istream>
#include <ostream>

namespace falcon::daemon::mcpstdio {

namespace {
constexpr const char* kMcpPath = "/mcp";
} // namespace

McpStdioBridge::McpStdioBridge(McpStdioConfig config, HttpPostFn post)
    : config_(std::move(config)), post_(std::move(post)) {}

std::vector<HttpHeader> McpStdioBridge::build_headers() const {
    std::vector<HttpHeader> headers;
    headers.emplace_back("Content-Type", "application/json");
    // MCP 规范要求 POST 的 Accept 含 application/json 与
    // text/event-stream；本壳只消费 JSON 响应（SSE 形态属阶段 2
    // 进度订阅），声明规范要求的集合不影响纯 JSON 服务器。
    headers.emplace_back("Accept", "application/json, text/event-stream");
    if (!config_.secret.empty())
        headers.emplace_back("Authorization", "Bearer " + config_.secret);
    if (!session_.empty())
        headers.emplace_back("Mcp-Session-Id", session_);
    return headers;
}

std::optional<std::string> McpStdioBridge::failure_envelope(
    const std::string& line, int status) const {
    // allow_exceptions=false：坏行静默跳过（host 违约发了非 JSON 行，
    // 转发给 daemon 也只会再吃一个 4xx，此处不放大）
    nlohmann::json parsed =
        nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    auto id_it = parsed.find("id");
    if (id_it == parsed.end() || id_it->is_null()) return std::nullopt;

    std::string message =
        status == 0 ? "falcon-mcp: cannot reach daemon at " + config_.rpc_url
                    : "falcon-mcp: daemon returned HTTP " +
                          std::to_string(status) + " with empty body";
    nlohmann::json envelope = {{"jsonrpc", "2.0"},
                               {"id", *id_it},
                               {"error", {{"code", -32603}, {"message", message}}}};
    return envelope.dump();
}

std::optional<std::string> McpStdioBridge::handle_message(
    const std::string& line) {
    // 剥行尾 CR/LF（Windows 管道与部分 host 以 CRLF 分帧）
    std::string msg = line;
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n'))
        msg.pop_back();
    if (msg.find_first_not_of(" \t") == std::string::npos) return std::nullopt;

    HttpResult result = post_(kMcpPath, build_headers(), msg, "POST");

    if (result.status == 200 || result.status == 202) {
        // daemon 只在 initialize 响应携带会话头；任何响应带上即采纳
        //（host 重新 initialize 时自然换新会话）
        if (!result.session_id.empty()) session_ = result.session_id;
        if (result.status == 202 || result.body.empty()) return std::nullopt;
        return result.body;
    }

    // 404 = 会话失效（daemon 重启/过期/已被拆）：清空后下个请求不再
    // 携带，host 可直接重新 initialize（daemon 对 initialize 恒新建
    // 会话，不拒绝带旧头的重连）。
    if (result.status == 404) session_.clear();

    // 有体透传（401/403/404/5xx 的错误体，host 据此自行收口）；
    // 无体合成错误信封，带 id 的请求不悬空。
    if (!result.body.empty()) return result.body;
    return failure_envelope(msg, result.status);
}

void McpStdioBridge::shutdown() {
    if (session_.empty()) return;  // 未握手/已失效，无从拆除
    post_(kMcpPath, build_headers(), "", "DELETE");
    session_.clear();
}

void McpStdioBridge::run(std::istream& in, std::ostream& out) {
    std::string line;
    while (std::getline(in, line)) {
        auto response = handle_message(line);
        if (response) {
            out << *response << '\n';
            out.flush();  // 请求-响应逐行可见，host 端不等批量
        }
    }
    shutdown();
}

} // namespace falcon::daemon::mcpstdio
