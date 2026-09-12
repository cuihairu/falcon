/**
 * @file websocket_frame.hpp
 * @brief WebSocket (RFC 6455) 握手应答与帧编解码 — 纯协议层，无 socket 依赖
 * @author Falcon Team
 * @date 2026-09-12
 *
 * 只覆盖 daemon 作为服务端所需的最小子集：
 * - Sec-WebSocket-Accept 计算（SHA1 + base64，自实现，不引入 OpenSSL 依赖）
 * - 服务端帧编码（服务端 → 客户端不掩码）
 * - 增量式帧解析（客户端 → 服务端帧必须掩码；支持 126/127 扩展长度与
 *   text 分片聚合；ping/pong/close 原样透传）
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace falcon::daemon::rpc {

/// RFC 6455 帧操作码
enum WsOpcode : std::uint8_t {
    WS_OP_CONTINUATION = 0x0,
    WS_OP_TEXT = 0x1,
    WS_OP_BINARY = 0x2,
    WS_OP_CLOSE = 0x8,
    WS_OP_PING = 0x9,
    WS_OP_PONG = 0xA,
};

/// Sec-WebSocket-Accept = base64(SHA1(client_key + GUID))
std::string ws_compute_accept_key(const std::string& client_key);

/// 标准 base64 编码（客户端握手 Sec-WebSocket-Key 也需要）
std::string ws_base64_encode(const std::uint8_t* data, std::size_t size);

/// 编码服务端帧（服务端 → 客户端方向不掩码）
std::string ws_encode_frame(std::uint8_t opcode, const std::string& payload);

/// 编码客户端帧（客户端 → 服务端方向必须掩码，RFC 6455 §5.3）。
/// 掩码 key 每帧随机生成（thread_local mt19937_64）。
std::string ws_encode_client_frame(std::uint8_t opcode, const std::string& payload);

/// 解析出的单条 WebSocket 消息
struct WsFrame {
    std::uint8_t opcode = 0;
    std::string payload;
};

/// 增量式帧解析器：feed 任意分段字节流，pop 出已完整的消息。
class WsFrameParser {
public:
    /// 单条消息（含聚合中的分片）字节上限，超限进入错误状态
    static constexpr std::size_t kMaxMessageBytes = 1u << 20; // 1 MiB

    /// 追加收到的一段字节
    void feed(const char* data, std::size_t size);

    /// 取出已完整收到的消息（text/binary 分片聚合成一条起始操作码的消息）
    std::vector<WsFrame> pop_messages();

    /// 协议错误（非法操作码 / 超长消息 / 分片序列错误），之后应关闭连接
    bool error() const { return error_; }

private:
    /// 尝试从 buffer_ 头部解析一帧；数据不足时返回 false
    bool step();

    std::string buffer_;
    std::vector<WsFrame> messages_;
    bool fragmented_ = false;
    std::uint8_t fragment_opcode_ = 0;
    std::string fragment_data_;
    bool error_ = false;
};

} // namespace falcon::daemon::rpc
