#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace falcon::daemon::mcpstdio {

/// 一次 HTTP 交互的结果。status 为 0 表示传输层失败（连不上/超时）。
struct HttpResult {
    int status = 0;
    std::string body;
    std::string session_id;  // Mcp-Session-Id 响应头（无则空）
};

using HttpHeader = std::pair<std::string, std::string>;

/// (path, headers, body, http_method) → 结果。桥为单线程逐行驱动，
/// 实现无需并发安全。
using HttpPostFn = std::function<HttpResult(
    const std::string& path, const std::vector<HttpHeader>& headers,
    const std::string& body, const std::string& http_method)>;

struct McpStdioConfig {
    std::string rpc_url = "http://127.0.0.1:6800";  // daemon 基址
    std::string secret;                             // daemon rpc.secret（可空）
    int timeout_seconds = 120;                      // 单请求上界（传输实现可忽略）
};

/// MCP stdio ↔ daemon /mcp 桥（阶段 2 薄壳，设计文档 docs/design/
/// mcp_server_design.md §2.1/§4）。
///
/// 职责仅三件：① 逐行读 stdin 的 JSON-RPC 消息转发 daemon /mcp；
/// ② 维护 Mcp-Session-Id（initialize 响应捕获、后续请求附带、404 清除、
/// EOF 时 DELETE 拆除）；③ 通知 202/空体静默，失败时对带 id 的消息
/// 合成 JSON-RPC 错误信封（防 host 端无限等待本壳的响应行）。
/// 不链 libfalcon-core——传输经 HttpPostFn 注入（curl 实现见
/// mcp_curl_post.hpp），单元测试以 fake 注入走同一桥逻辑。
class McpStdioBridge {
public:
    McpStdioBridge(McpStdioConfig config, HttpPostFn post);

    /// 处理一行 stdin 消息，返回应写往 stdout 的响应体（空 = 无输出：
    /// 空白行/通知 202/空体响应/传输失败且消息不带 id）。
    std::optional<std::string> handle_message(const std::string& line);

    /// EOF 收尾：DELETE /mcp 拆除会话（best-effort，静默吞错）；
    /// 无会话（未握手或已失效）则为无操作。
    void shutdown();

    /// 驱动循环：逐行读 in 直到 EOF，非空行交 handle_message，有输出
    /// 即写 out（补换行并 flush）；EOF 后 shutdown()。
    void run(std::istream& in, std::ostream& out);

private:
    std::vector<HttpHeader> build_headers() const;
    /// 对带 id 的请求消息合成 -32603 错误信封；通知（无 id）返回空。
    std::optional<std::string> failure_envelope(const std::string& line,
                                                int status) const;

    McpStdioConfig config_;
    HttpPostFn post_;
    std::string session_;  // daemon 下发的 Mcp-Session-Id（404/DELETE 后清空）
};

} // namespace falcon::daemon::mcpstdio
