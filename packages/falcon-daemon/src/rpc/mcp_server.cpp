#include "rpc/mcp_server.hpp"

#include <falcon/version.hpp>

#include <algorithm>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <utility>

namespace falcon::daemon::rpc {

namespace {

using json = nlohmann::json;

/// 会话表容量上限：超限 FIFO 淘汰最旧会话（有界，防无界增长）。
constexpr std::size_t kMaxSessions = 64;

/// 版本协商支持集（MCP 规范版本）；不认识/缺失时回应默认版本。
constexpr const char* kSupportedVersions[] = {"2025-06-18", "2025-03-26"};
constexpr const char* kDefaultProtocolVersion = "2025-06-18";

// ---- JSON-RPC 2.0 信封 ----

json rpc_result(const json& id, json result) {
    json r = json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = std::move(result);
    return r;
}

json rpc_error(const json& id, int code, std::string message) {
    json e = json::object();
    e["code"] = code;
    e["message"] = std::move(message);
    json r = json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"] = std::move(e);
    return r;
}

// ---- 响应辅助 ----

McpServer::Response json_response(int status, std::string status_text,
                                  const json& payload) {
    McpServer::Response r;
    r.status_code = status;
    r.status_text = std::move(status_text);
    r.body = payload.dump();
    r.headers["Content-Type"] = "application/json";
    return r;
}

McpServer::Response empty_response(int status, std::string status_text) {
    McpServer::Response r;
    r.status_code = status;
    r.status_text = std::move(status_text);
    return r;
}

McpServer::Response method_not_allowed() {
    McpServer::Response r = json_response(
        405, "Method Not Allowed",
        json{{"error", "method not allowed; use POST (JSON-RPC) or DELETE (session teardown)"}});
    r.headers["Allow"] = "POST, DELETE";
    return r;
}

McpServer::Response session_not_found() {
    return json_response(404, "Not Found",
                         json{{"error", "unknown or expired MCP session; re-initialize"}});
}

// ---- tools/call 翻译层 ----

/// 参数形状错误：协议层 -32602（JSON-RPC error，不进 result.isError）。
struct ProtocolError {
    int code;
    std::string message;
};

[[noreturn]] void throw_invalid(std::string message) {
    throw ProtocolError{-32602, std::move(message)};
}

/// 语义性参数错误（白名单外键/别名冲突）：业务错误形状，
/// 由 wrap_tool_result 映射为 MCP result.isError。
json business_error(std::string message) {
    json e = json::object();
    e["code"] = 1;
    e["message"] = std::move(message);
    json r = json::object();
    r["error"] = std::move(e);
    return r;
}

json slice_array(const json& arr, int64_t offset, int64_t limit) {
    json out = json::array();
    const int64_t n = static_cast<int64_t>(arr.size());
    for (int64_t i = offset; i < n && static_cast<int64_t>(out.size()) < limit; ++i)
        out.push_back(arr[static_cast<std::size_t>(i)]);
    return out;
}

/// dispatch 业务错误 {"error":{code,message}} → MCP result
/// {isError:true, content:[text "Error(<code>): <message>"]}；
/// 成功 → {content:[text], structuredContent: 结果}。
json wrap_tool_result(json&& dispatched) {
    if (dispatched.is_object() && dispatched.contains("error")) {
        const json& err = dispatched.at("error");
        const int code = err.contains("code") && err.at("code").is_number_integer()
                             ? err.at("code").get<int>()
                             : 1;
        std::string message = err.contains("message") && err.at("message").is_string()
                                  ? err.at("message").get<std::string>()
                                  : std::string("unknown error");
        json content = json::array();
        json item = json::object();
        item["type"] = "text";
        item["text"] = "Error(" + std::to_string(code) + "): " + message;
        content.push_back(std::move(item));
        json r = json::object();
        r["content"] = std::move(content);
        r["isError"] = true;
        return r;
    }
    std::string text = dispatched.is_string() ? dispatched.get<std::string>()
                                              : dispatched.dump();
    json content = json::array();
    json item = json::object();
    item["type"] = "text";
    item["text"] = text;
    content.push_back(std::move(item));
    json r = json::object();
    r["content"] = std::move(content);
    r["structuredContent"] = std::move(dispatched);
    return r;
}

/// addUri 的 per-download 选项透传白名单（aria2 兼容键位，设计文档 §3）。
const std::set<std::string>& allowed_add_uri_options() {
    static const std::set<std::string> kAllowed = {
        "dir", "out", "header", "index-out", "user-agent", "referer",
        "max-connection-per-server", "split", "continue", "max-download-limit",
        "allow-overwrite", "auto-file-renaming", "certificate", "private-key",
        "seed-ratio", "seed-time"};
    return kAllowed;
}

/// 工具名 → aria2 方法翻译（设计文档 §3 翻译表）。
/// 参数形状错误抛 ProtocolError；语义性参数错误与 dispatch 业务错误
/// 以 {"error":{code,message}} 形状返回（调用方包成 result.isError）。
json translate_tool_call(const std::string& tool, const json& args,
                         const McpServer::DispatchFn& dispatch) {
    auto require_string = [&](const char* key) {
        auto it = args.find(key);
        if (it == args.end() || !it->is_string())
            throw_invalid(std::string("Invalid params: '") + key + "' (string) is required");
        return it->get<std::string>();
    };

    if (tool == "falcon_add_download") {
        auto urls_it = args.find("urls");
        if (urls_it == args.end() || !urls_it->is_array() || urls_it->empty())
            throw_invalid("Invalid params: 'urls' must be a non-empty array of strings");
        for (const auto& u : *urls_it)
            if (!u.is_string())
                throw_invalid("Invalid params: 'urls' must be a non-empty array of strings");

        const bool has_dir = args.contains("output_dir");
        const bool has_out = args.contains("filename");
        if (has_dir && !args.at("output_dir").is_string())
            throw_invalid("Invalid params: 'output_dir' must be a string");
        if (has_out && !args.at("filename").is_string())
            throw_invalid("Invalid params: 'filename' must be a string");

        json extra = json::object();
        if (auto e = args.find("options"); e != args.end()) {
            if (!e->is_object())
                throw_invalid("Invalid params: 'options' must be an object");
            extra = *e;
        }
        for (const auto& entry : extra.items()) {
            if (entry.key() == "dir" && has_dir)
                return business_error("Conflicting arguments: output_dir and options.dir");
            if (entry.key() == "out" && has_out)
                return business_error("Conflicting arguments: filename and options.out");
            if (!allowed_add_uri_options().count(entry.key()))
                return business_error("Unsupported option: " + entry.key());
        }
        json options = extra;
        if (has_dir) options["dir"] = args.at("output_dir");
        if (has_out) options["out"] = args.at("filename");
        return dispatch("aria2.addUri", json::array({*urls_it, options}));
    }

    if (tool == "falcon_list_tasks") {
        std::string status = "all";
        if (auto s = args.find("status"); s != args.end()) {
            if (!s->is_string())
                throw_invalid("Invalid params: 'status' must be one of active|waiting|stopped|all");
            const std::string v = s->get<std::string>();
            if (v != "active" && v != "waiting" && v != "stopped" && v != "all")
                throw_invalid("Invalid params: 'status' must be one of active|waiting|stopped|all");
            status = v;
        }
        int64_t limit = 50;
        int64_t offset = 0;
        for (const auto& [key, target] :
             std::initializer_list<std::pair<const char*, int64_t*>>{
                 {"limit", &limit}, {"offset", &offset}}) {
            auto it = args.find(key);
            if (it == args.end()) continue;
            if (!it->is_number_integer() || it->get<int64_t>() < 0)
                throw_invalid(std::string("Invalid params: '") + key +
                              "' must be a non-negative integer");
            *target = it->get<int64_t>();
        }

        json tasks = json::array();
        json error;
        auto append = [&](json r) {
            if (r.is_object() && r.contains("error")) {
                error = std::move(r);
                return;
            }
            if (!r.is_array()) {
                error = business_error("Unexpected non-array response from daemon query");
                return;
            }
            for (auto& t : r) tasks.push_back(std::move(t));
        };
        if (status == "active") {
            append(dispatch("aria2.tellActive", json::array()));
        } else if (status == "waiting") {
            append(dispatch("aria2.tellWaiting", json::array({offset, limit})));
        } else if (status == "stopped") {
            append(dispatch("aria2.tellStopped", json::array({offset, limit})));
        } else {  // all：三队列合并后统一切片
            append(dispatch("aria2.tellActive", json::array()));
            if (error.is_null())
                append(dispatch("aria2.tellWaiting", json::array({0, 100000})));
            if (error.is_null())
                append(dispatch("aria2.tellStopped", json::array({0, 100000})));
        }
        if (!error.is_null())
            return error;
        const int64_t total = static_cast<int64_t>(tasks.size());
        if (status == "all" || status == "active")
            tasks = slice_array(tasks, offset, limit);
        json r = json::object();
        r["tasks"] = std::move(tasks);
        r["total"] = total;
        return r;
    }

    if (tool == "falcon_get_task")
        return dispatch("aria2.tellStatus", json::array({require_string("gid")}));
    if (tool == "falcon_pause_task")
        return dispatch("aria2.forcePause", json::array({require_string("gid")}));
    if (tool == "falcon_resume_task")
        return dispatch("aria2.unpause", json::array({require_string("gid")}));
    if (tool == "falcon_get_task_files")
        return dispatch("aria2.getFiles", json::array({require_string("gid")}));
    if (tool == "falcon_pause_all")
        return dispatch("aria2.pauseAll", json::array());
    if (tool == "falcon_resume_all")
        return dispatch("aria2.unpauseAll", json::array());
    if (tool == "falcon_get_global_stats")
        return dispatch("aria2.getGlobalStat", json::array());

    if (tool == "falcon_remove_task") {
        const std::string gid = require_string("gid");
        bool force = false;
        if (auto f = args.find("force"); f != args.end()) {
            if (!f->is_boolean())
                throw_invalid("Invalid params: 'force' must be a boolean");
            force = f->get<bool>();
        }
        // force 默认 false：remove 对活动任务拒绝（业务错误），
        // force=true 才走 forceRemove 强拆。
        return dispatch(force ? "aria2.forceRemove" : "aria2.remove",
                        json::array({gid}));
    }

    // 兜底（handle_request 已先验白名单，正常到不了这里）
    throw_invalid("Unknown tool: " + tool);
}

bool is_known_tool(const std::string& name) {
    static const std::set<std::string> kTools = {
        "falcon_add_download",   "falcon_list_tasks",     "falcon_get_task",
        "falcon_pause_task",     "falcon_resume_task",    "falcon_remove_task",
        "falcon_pause_all",      "falcon_resume_all",     "falcon_get_global_stats",
        "falcon_get_task_files"};
    return kTools.count(name) != 0;
}

} // namespace

// ---- 会话表 ----

bool McpServer::session_valid(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    return std::find(sessions_.begin(), sessions_.end(), session_id) != sessions_.end();
}

void McpServer::insert_session(std::string session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    if (sessions_.size() >= kMaxSessions)
        sessions_.pop_front();
    sessions_.push_back(std::move(session_id));
}

void McpServer::remove_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions_.erase(std::remove(sessions_.begin(), sessions_.end(), session_id),
                    sessions_.end());
}

std::string McpServer::new_session_id() {
    // 32 hex 字符（128 bit 随机），mt19937_64 × 2
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < 2; ++i)
        oss << std::setw(16) << rng();
    return oss.str();
}

// ---- 请求处理 ----

McpServer::Response McpServer::handle_request(const Request& req,
                                              const std::string& auth_secret,
                                              const DispatchFn& dispatch) {
    // 认证先于一切路由：未配 secret 时认证不可能成立，端点整体拒绝；
    // 配置了 secret 时要求 Authorization: Bearer <secret>。
    if (auth_secret.empty())
        return json_response(
            403, "Forbidden",
            json{{"error", "mcp endpoint requires rpc.secret to be configured"}});

    static const std::string kBearer = "Bearer ";
    std::string presented;
    if (auto it = req.headers.find("authorization"); it != req.headers.end())
        presented = it->second;
    if (presented.compare(0, kBearer.size(), kBearer) != 0 ||
        presented.substr(kBearer.size()) != auth_secret) {
        McpServer::Response r =
            json_response(401, "Unauthorized", json{{"error", "unauthorized"}});
        r.headers["WWW-Authenticate"] = "Bearer";
        return r;
    }

    // HTTP 方法路由：POST（JSON-RPC 请求/通知）与 DELETE（会话拆除）；
    // 阶段 1 无 GET/SSE 面。
    if (req.method != "POST" && req.method != "DELETE")
        return method_not_allowed();

    if (req.method == "DELETE") {
        auto sid = req.headers.find("mcp-session-id");
        if (sid == req.headers.end() || !session_valid(sid->second))
            return session_not_found();
        remove_session(sid->second);
        return empty_response(204, "No Content");
    }

    // ---- POST：JSON-RPC 解析（批次明确拒绝，不逐条处理） ----
    json parsed = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded())
        return json_response(400, "Bad Request",
                             rpc_error(json(nullptr), -32700, "Parse error"));
    if (parsed.is_array())
        return json_response(
            400, "Bad Request",
            rpc_error(json(nullptr), -32600, "Invalid Request: batch requests not supported"));
    if (!parsed.is_object())
        return json_response(200, "OK",
                             rpc_error(json(nullptr), -32600, "Invalid Request"));

    const json id = parsed.contains("id") ? parsed.at("id") : json(nullptr);
    const bool notification = id.is_null();
    if (!parsed.contains("method") || !parsed.at("method").is_string()) {
        if (notification)
            return empty_response(202, "Accepted");
        return json_response(200, "OK",
                             rpc_error(id, -32600, "Invalid Request: missing method"));
    }
    const std::string method = parsed.at("method").get<std::string>();
    json params = json::object();
    if (auto p = parsed.find("params"); p != parsed.end() && p->is_object())
        params = *p;

    // initialize 忽略请求携带的会话头，总是新建会话（MCP 规范）。
    if (method == "initialize") {
        std::string negotiated = kDefaultProtocolVersion;
        if (auto v = params.find("protocolVersion");
            v != params.end() && v->is_string()) {
            for (const char* supported : kSupportedVersions)
                if (v->get<std::string>() == supported)
                    negotiated = supported;
        }
        std::string sid = new_session_id();
        insert_session(sid);

        json caps = json::object();
        caps["tools"] = json::object();
        json info = json::object();
        info["name"] = "falcon";
        info["version"] = FALCON_VERSION_STRING;
        json result = json::object();
        result["protocolVersion"] = negotiated;
        result["capabilities"] = std::move(caps);
        result["serverInfo"] = std::move(info);

        McpServer::Response r = json_response(200, "OK", rpc_result(id, std::move(result)));
        r.headers["Mcp-Session-Id"] = std::move(sid);
        return r;
    }

    // 其余方法要求有效会话
    auto sid = req.headers.find("mcp-session-id");
    if (sid == req.headers.end() || !session_valid(sid->second))
        return session_not_found();

    // 通知（无 id / id null）一律受理不执行：工具面无服务器→客户端
    // 资源更新，客户端通知没有可落的语义。
    if (notification)
        return empty_response(202, "Accepted");

    if (method == "ping")
        return json_response(200, "OK", rpc_result(id, json::object()));
    if (method == "tools/list")
        return json_response(200, "OK",
                             rpc_result(id, json{{"tools", mcp_tools_manifest()}}));
    if (method == "tools/call") {
        auto name_it = params.find("name");
        if (name_it == params.end() || !name_it->is_string())
            return json_response(
                200, "OK",
                rpc_error(id, -32602, "Invalid params: 'name' (string) is required"));
        const std::string tool = name_it->get<std::string>();
        json args = json::object();
        if (auto a = params.find("arguments"); a != params.end()) {
            if (!a->is_object())
                return json_response(
                    200, "OK",
                    rpc_error(id, -32602, "Invalid params: 'arguments' must be an object"));
            args = *a;
        }
        if (!is_known_tool(tool))
            return json_response(200, "OK",
                                 rpc_error(id, -32602, "Unknown tool: " + tool));
        try {
            json business = translate_tool_call(tool, args, dispatch);
            return json_response(200, "OK",
                                 rpc_result(id, wrap_tool_result(std::move(business))));
        } catch (const ProtocolError& e) {
            return json_response(200, "OK", rpc_error(id, e.code, e.message));
        }
    }
    return json_response(200, "OK", rpc_error(id, -32601, "Method not found"));
}

// ---- 工具清单（静态数据；描述英文，注释中文） ----

nlohmann::json mcp_tools_manifest() {
    auto tool = [](std::string name, std::string description, json properties,
                   json required, json annotations) {
        json schema = json::object();
        schema["type"] = "object";
        schema["properties"] = std::move(properties);
        schema["required"] = std::move(required);
        schema["additionalProperties"] = false;
        json t = json::object();
        t["name"] = std::move(name);
        t["description"] = std::move(description);
        t["inputSchema"] = std::move(schema);
        t["annotations"] = std::move(annotations);
        return t;
    };

    auto prop = [](std::string type, std::string description) {
        json p = json::object();
        p["type"] = std::move(type);
        p["description"] = std::move(description);
        return p;
    };

    return json::array({
        tool(
            "falcon_add_download",
            "Add a download task to the Falcon daemon. Returns the new task id (gid). "
            "Downloads run asynchronously: poll falcon_get_task with the gid until "
            "status is \"complete\".",
            [&] {
                json p = json::object();
                json urls = json::object();
                urls["type"] = "array";
                urls["items"] = json{{"type", "string"}};
                urls["description"] =
                    "Download URIs (http/https/ftp/sftp/magnet or any supported scheme).";
                p["urls"] = std::move(urls);
                p["output_dir"] =
                    prop("string", "Directory to save the file to (aria2 dir option).");
                p["filename"] = prop("string", "File name for the download (aria2 out option).");
                p["options"] = prop(
                    "object",
                    "Extra per-download options (aria2 key names). Allowed keys: dir, out, "
                    "header, index-out, user-agent, referer, max-connection-per-server, "
                    "split, continue, max-download-limit, allow-overwrite, "
                    "auto-file-renaming, certificate, private-key, seed-ratio, seed-time.");
                return p;
            }(),
            json::array({"urls"}),
            json{{"title", "Add download"}, {"readOnlyHint", false}, {"openWorldHint", false}}),

        tool(
            "falcon_list_tasks",
            "List download tasks with a progress snapshot. status selects the queue: "
            "active, waiting, stopped, or all (default).",
            [&] {
                json p = json::object();
                json status = json::object();
                status["type"] = "string";
                status["enum"] = json::array({"active", "waiting", "stopped", "all"});
                status["description"] = "Which queue to list (default all).";
                p["status"] = std::move(status);
                json limit = prop("integer", "Maximum number of tasks to return (default 50).");
                limit["minimum"] = 0;
                p["limit"] = std::move(limit);
                json offset = prop("integer", "Skip the first offset tasks (default 0).");
                offset["minimum"] = 0;
                p["offset"] = std::move(offset);
                return p;
            }(),
            json::array(),
            json{{"title", "List tasks"}, {"readOnlyHint", true}, {"openWorldHint", false}}),

        tool(
            "falcon_get_task",
            "Get detailed status of one task by gid: progress, byte counts (with "
            "explicit _bytes suffixes), speed, and error message on failure.",
            [&] {
                json p = json::object();
                p["gid"] = prop("string", "Task id returned by falcon_add_download.");
                return p;
            }(),
            json::array({"gid"}),
            json{{"title", "Get task status"}, {"readOnlyHint", true}, {"openWorldHint", false}}),

        tool(
            "falcon_pause_task",
            "Pause one task by gid. Failed or completed tasks cannot be paused.",
            [&] {
                json p = json::object();
                p["gid"] = prop("string", "Task id to pause.");
                return p;
            }(),
            json::array({"gid"}),
            json{{"title", "Pause task"}, {"readOnlyHint", false}, {"openWorldHint", false}}),

        tool(
            "falcon_resume_task",
            "Resume a paused task by gid.",
            [&] {
                json p = json::object();
                p["gid"] = prop("string", "Task id to resume.");
                return p;
            }(),
            json::array({"gid"}),
            json{{"title", "Resume task"}, {"readOnlyHint", false}, {"openWorldHint", false}}),

        tool(
            "falcon_remove_task",
            "Remove a task by gid. Completed downloads keep their files; active tasks "
            "refuse removal unless force is true.",
            [&] {
                json p = json::object();
                p["gid"] = prop("string", "Task id to remove.");
                p["force"] = prop("boolean",
                                  "Force removal even if the task is active (default false).");
                return p;
            }(),
            json::array({"gid"}),
            json{{"title", "Remove task"},
                 {"readOnlyHint", false},
                 {"destructiveHint", true},
                 {"openWorldHint", false}}),

        tool("falcon_pause_all", "Pause every active task.", json::object(),
             json::array(),
             json{{"title", "Pause all tasks"},
                  {"readOnlyHint", false},
                  {"destructiveHint", true},
                  {"openWorldHint", false}}),

        tool("falcon_resume_all", "Resume every paused task.", json::object(),
             json::array(),
             json{{"title", "Resume all tasks"}, {"readOnlyHint", false}, {"openWorldHint", false}}),

        tool(
            "falcon_get_global_stats",
            "Get global transfer statistics: active/waiting/stopped task counts and "
            "overall download/upload speed (bytes per second).",
            json::object(), json::array(),
            json{{"title", "Get global stats"}, {"readOnlyHint", true}, {"openWorldHint", false}}),

        tool(
            "falcon_get_task_files",
            "List the files of a task: path, total size and completed bytes "
            "(fields carry explicit _bytes suffixes).",
            [&] {
                json p = json::object();
                p["gid"] = prop("string", "Task id to inspect.");
                return p;
            }(),
            json::array({"gid"}),
            json{{"title", "Get task files"}, {"readOnlyHint", true}, {"openWorldHint", false}}),
    });
}

} // namespace falcon::daemon::rpc
