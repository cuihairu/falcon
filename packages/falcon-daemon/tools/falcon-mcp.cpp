/// falcon-mcp：MCP stdio 薄壳（阶段 2 增量 2，设计文档 §4）。
///
/// stdin 逐行读 MCP JSON-RPC 消息 → 转发 daemon /mcp（Streamable
/// HTTP）→ stdout 回写响应行；EOF 时 DELETE 拆除会话。不链
/// libfalcon-core——纯转发进程，无引擎形态。
///
/// 用法（MCP host 的 mcpServers 配置里 args 传参）：
///   falcon-mcp [--rpc-url URL] [--secret TOKEN]
#include "mcp_curl_post.hpp"
#include "mcp_stdio.hpp"

#include <iostream>
#include <string>
#include <utility>

#ifndef FALCON_MCP_VERSION
#define FALCON_MCP_VERSION "unknown"
#endif

using falcon::daemon::mcpstdio::McpStdioBridge;
using falcon::daemon::mcpstdio::McpStdioConfig;

namespace {

void print_usage(std::ostream& out) {
    out << "falcon-mcp - Falcon MCP stdio 薄壳（转发 daemon /mcp）\n\n"
           "用法: falcon-mcp [--rpc-url URL] [--secret TOKEN]\n"
           "  --rpc-url URL   daemon 基址（默认 http://127.0.0.1:6800）\n"
           "  --secret TOKEN  Bearer 凭据（daemon rpc.secret）\n"
           "  -h, --help      显示本帮助\n"
           "  --version       显示版本\n";
}

} // namespace

int main(int argc, char** argv) {
    McpStdioConfig config;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        }
        if (arg == "--version") {
            std::cout << "falcon-mcp " << FALCON_MCP_VERSION << "\n";
            return 0;
        }
        // 带值旗标：缺值按未知参数收口
        if (arg == "--rpc-url" || arg == "--secret") {
            if (i + 1 >= argc) {
                std::cerr << "falcon-mcp: " << arg << " 需要一个值\n";
                return 2;
            }
            const std::string value = argv[++i];
            if (arg == "--rpc-url")
                config.rpc_url = value;
            else
                config.secret = value;
            continue;
        }
        std::cerr << "falcon-mcp: 未知参数: " << arg << "\n";
        print_usage(std::cerr);
        return 2;
    }

    McpStdioBridge bridge(std::move(config), make_curl_http_post(config));
    bridge.run(std::cin, std::cout);
    return 0;
}
