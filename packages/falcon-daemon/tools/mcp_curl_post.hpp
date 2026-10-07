#pragma once

#include "mcp_stdio.hpp"

namespace falcon::daemon::mcpstdio {

/// libcurl 实现的 HttpPostFn。每请求独立 easy handle——本壳单线程低频，
/// 不为连接复用引入句柄生命周期管理（沿 S3 批次「handle 复用残留」
/// 教训：POSTFIELDS/CUSTOMREQUEST 等选项在复用句柄上必须显式清设，
/// 逐请求新建则天然无残留）。curl_global_init 由本函数首次调用时
/// 幂等完成（进程生命周期不清理，沿引擎 WSAStartup 同姿态）。
HttpPostFn make_curl_http_post(const McpStdioConfig& config);

} // namespace falcon::daemon::mcpstdio
