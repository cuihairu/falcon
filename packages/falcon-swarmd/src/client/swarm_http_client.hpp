#pragma once

// ============================================================================
// SwarmHttpClient：节点 → 目录服务器的 HTTP 传输（libcurl 薄封装）
//
// 职责单一：POST JSON 到 RPC 端点（带 Bearer 头）+ GET /v1/health 探测。
// 每次调用独立 curl_easy handle（handle 复用残留教训：CURLOPT_* 在
// 复用句柄上粘连——S3 批次 NOBODY/CUSTOMREQUEST 互斥清设的同一类坑），
// CURLOPT_NOSIGNAL=1 恒设（多线程 + DNS 解析的信号安全先例）。
//
// 错误模型：transport_ok=false 即传输层失败（DNS/连接/超时），http_status
// 为 0、transport_error 带原因；transport_ok=true 时服务器语义错误走
// RPC envelope 的 error_code（不在本层解读）。
// ============================================================================

#include <string>

namespace falcon::swarm {

struct SwarmHttpReply {
    bool transport_ok = false;
    long http_status = 0;
    std::string body;
    std::string transport_error;
};

/// 逐请求构造的轻量客户端（无内部状态，可多线程并发使用）。
class SwarmHttpClient {
public:
    struct Options {
        std::string host = "127.0.0.1";
        int port = 0;                 // 服务器实际监听端口
        std::string bearer_token;     // 非空才带 Authorization 头
        long timeout_ms = 5000;       // 单请求上限
    };

    explicit SwarmHttpClient(Options opt);

    /// POST <rpc_path>，body 为 JSON 文本；响应体原文返回（envelope 解析
    /// 归调用方）。
    SwarmHttpReply post_json(const std::string& rpc_path,
                             const std::string& json_body);

    /// GET /v1/health（无鉴权端点）：transport_ok 且 2xx 即存活。
    SwarmHttpReply get_health();

private:
    Options opt_;
};

}  // namespace falcon::swarm
