#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace falcon::daemon::rpc {

/// MCP Streamable HTTP 协议层（设计文档 docs/design/mcp_server_design.md）。
///
/// JsonRpcServer 把 /mcp 路径的原始请求转成 Request，由本类按 MCP 契约
/// 处理：initialize 建会话（Mcp-Session-Id 响应头）→ 会话校验 →
/// tools/list / tools/call。业务方法分发经 DispatchFn 回到
/// JsonRpcServer::dispatch_rpc（aria2 方法面）；业务错误
/// {"error":{code,message}} 映射为 MCP result.isError 形状，
/// 不占用 JSON-RPC 协议错误码。
///
/// 阶段边界：GET（SSE 服务器→客户端通知流）经 validate_sse_request
/// 准入、流本体由 JsonRpcServer 连接线程承载（通知扇出复用
/// broadcast_notification，阶段 2 增量 3）；批量请求不支持（回 400）；
/// 工具面为设计文档 §3 的 13 个只读+控制工具（阶段 1 的 10 个 +
/// 阶段 2 增量 1 的全局选项 2 个与做种 1 个），无资源/提示词面。
class McpServer {
public:
    struct Request {
        std::string method;  // HTTP 方法（POST/GET/DELETE/...）
        std::map<std::string, std::string> headers;  // 头键已小写
        std::string body;
    };

    struct Response {
        int status_code = 200;
        std::string status_text = "OK";
        std::map<std::string, std::string> headers;
        std::string body;
    };

    /// aria2 方法面分发回调（method, params）→ result 或
    /// {"error":{code,message}}。线程安全要求与 JSON-RPC 主路径一致。
    using DispatchFn =
        std::function<nlohmann::json(const std::string&, nlohmann::json)>;

    /// 处理一次 /mcp 请求。auth_secret 为空时拒绝一切请求
    ///（secret 未配置则认证不可能成立，端点整体不可用）。
    Response handle_request(const Request& req, const std::string& auth_secret,
                            const DispatchFn& dispatch);

    /// GET /mcp（SSE 服务器→客户端通知流）准入校验：认证与会话校验
    /// 与 POST/DELETE 同语义（未配 secret 403 / Bearer 失败 401 +
    /// WWW-Authenticate / 会话缺失或过期 404）。通过时返回
    /// status_code==200 的空响应，SSE 响应头与会话循环由
    /// JsonRpcServer 连接线程直写——Response 形状无法表达无界流。
    Response validate_sse_request(const Request& req,
                                  const std::string& auth_secret);

private:
    /// 认证分级检查（handle_request 与 validate_sse_request 共用）；
    /// 通过返回 nullopt，否则返回应回发的错误响应。
    static std::optional<Response> check_auth(const Request& req,
                                              const std::string& auth_secret);

    bool session_valid(const std::string& session_id);
    void insert_session(std::string session_id);
    void remove_session(const std::string& session_id);
    static std::string new_session_id();

    std::mutex sessions_mutex_;
    std::deque<std::string> sessions_;  // FIFO，超 kMaxSessions 淘汰最旧
};

/// tools/list 清单（13 工具，静态数据——描述不拼接运行时数据，
/// 防提示注入放大）。独立自由函数便于单测直接断言 schema 形状。
nlohmann::json mcp_tools_manifest();

/// initialize serverInfo.version 上报值。单一事实源 = 本包 project(VERSION)
/// （构建期经 FALCON_DAEMON_VERSION 注入，沿 falcon-cli FALCON_CLI_VERSION /
/// falcon-mcp FALCON_MCP_VERSION 注入先例）。独立自由函数便于一致性
/// 钉子断言（响应值 == 注入值，回改任何硬编码字面量即红）。
std::string mcp_server_version();

} // namespace falcon::daemon::rpc
