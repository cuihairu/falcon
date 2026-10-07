#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
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
/// 阶段边界：GET（SSE）不支持（回 405，阶段 2 进度订阅落地时接入）；
/// 批量请求不支持（回 400）；工具面为设计文档 §3 的 13 个只读+控制
/// 工具（阶段 1 的 10 个 + 阶段 2 增量 1 的全局选项 2 个与做种 1 个），
/// 无资源/提示词面。
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

private:
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

} // namespace falcon::daemon::rpc
