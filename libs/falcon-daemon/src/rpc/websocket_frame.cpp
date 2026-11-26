/**
 * @file websocket_frame.cpp
 * @brief WebSocket (RFC 6455) 握手应答与帧编解码实现
 * @author Falcon Team
 * @date 2026-09-12
 */

#include "rpc/websocket_frame.hpp"

#include <algorithm>
#include <cstring>
#include <random>

namespace falcon::daemon::rpc {
namespace {

// ---------------------------------------------------------------------------
// SHA-1（RFC 3174）— 仅用于 WebSocket 握手应答计算，非安全用途
// ---------------------------------------------------------------------------

class Sha1 {
public:
    Sha1()
        : h_{0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u} {}

    void update(const char* data, std::size_t n) {
        total_len_ += n;
        while (n > 0) {
            const std::size_t take = std::min(n, std::size_t(64) - buf_used_);
            std::memcpy(block_ + buf_used_, data, take);
            buf_used_ += static_cast<std::uint8_t>(take);
            data += take;
            n -= take;
            if (buf_used_ == 64) {
                process_block(block_);
                buf_used_ = 0;
            }
        }
    }

    // 一次性：调用后对象不可继续 update
    std::array<std::uint8_t, 20> finish() {
        const std::uint64_t bit_len = total_len_ * 8;
        const char pad = static_cast<char>(0x80);
        update(&pad, 1);
        const char zero = 0;
        while (buf_used_ != 56) {
            update(&zero, 1);
        }
        char len_be[8];
        for (int i = 0; i < 8; ++i) {
            len_be[i] = static_cast<char>((bit_len >> (56 - 8 * i)) & 0xFF);
        }
        update(len_be, 8);

        std::array<std::uint8_t, 20> out{};
        for (int i = 0; i < 5; ++i) {
            out[static_cast<std::size_t>(i) * 4] =
                static_cast<std::uint8_t>(h_[static_cast<std::size_t>(i)] >> 24);
            out[static_cast<std::size_t>(i) * 4 + 1] =
                static_cast<std::uint8_t>(h_[static_cast<std::size_t>(i)] >> 16);
            out[static_cast<std::size_t>(i) * 4 + 2] =
                static_cast<std::uint8_t>(h_[static_cast<std::size_t>(i)] >> 8);
            out[static_cast<std::size_t>(i) * 4 + 3] =
                static_cast<std::uint8_t>(h_[static_cast<std::size_t>(i)]);
        }
        return out;
    }

private:
    void process_block(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (std::uint32_t(p[i * 4]) << 24) |
                   (std::uint32_t(p[i * 4 + 1]) << 16) |
                   (std::uint32_t(p[i * 4 + 2]) << 8) |
                   std::uint32_t(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            const std::uint32_t v = w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16];
            w[i] = (v << 1) | (v >> 31);
        }

        std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t tmp =
                ((a << 5) | (a >> 27)) + f + e + k + w[i];
            e = d;
            d = c;
            c = (b << 30) | (b >> 2);
            b = a;
            a = tmp;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    std::array<std::uint32_t, 5> h_;
    std::uint8_t block_[64]{};
    std::uint8_t buf_used_ = 0;
    std::uint64_t total_len_ = 0;
};

std::string base64_encode(const std::uint8_t* data, std::size_t n) {
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        std::uint32_t v = std::uint32_t(data[i]) << 16;
        const bool has1 = i + 1 < n;
        const bool has2 = i + 2 < n;
        if (has1) v |= std::uint32_t(data[i + 1]) << 8;
        if (has2) v |= std::uint32_t(data[i + 2]);

        out += kTable[(v >> 18) & 63];
        out += kTable[(v >> 12) & 63];
        out += has1 ? kTable[(v >> 6) & 63] : '=';
        out += has2 ? kTable[v & 63] : '=';
    }
    return out;
}

void append_be16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

} // namespace

std::string ws_compute_accept_key(const std::string& client_key) {
    static const char kWsGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

    Sha1 sha1;
    sha1.update(client_key.data(), client_key.size());
    sha1.update(kWsGuid, std::strlen(kWsGuid));
    const auto digest = sha1.finish();
    return base64_encode(digest.data(), digest.size());
}

std::string ws_base64_encode(const std::uint8_t* data, std::size_t size) {
    return base64_encode(data, size);
}

std::string ws_encode_frame(std::uint8_t opcode, const std::string& payload) {
    std::string frame;
    frame.reserve(payload.size() + 10);

    frame.push_back(static_cast<char>(0x80u | opcode)); // FIN=1
    const std::size_t n = payload.size();
    if (n < 126) {
        frame.push_back(static_cast<char>(n));
    } else if (n < 65536) {
        frame.push_back(static_cast<char>(126));
        append_be16(frame, static_cast<std::uint16_t>(n));
    } else {
        frame.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
        }
    }
    frame += payload;
    return frame;
}

std::string ws_encode_client_frame(std::uint8_t opcode, const std::string& payload) {
    // 掩码 key 不可预测（RFC 6455 §5.3）；本地 RPC 场景 mt19937_64 足够
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    const std::uint64_t v = rng();
    const std::uint8_t key[4] = {static_cast<std::uint8_t>(v >> 56),
                                 static_cast<std::uint8_t>(v >> 48),
                                 static_cast<std::uint8_t>(v >> 40),
                                 static_cast<std::uint8_t>(v >> 32)};

    std::string frame;
    frame.reserve(payload.size() + 14);
    frame.push_back(static_cast<char>(0x80u | opcode)); // FIN=1
    const std::size_t n = payload.size();
    if (n < 126) {
        frame.push_back(static_cast<char>(0x80u | n));
    } else if (n < 65536) {
        frame.push_back(static_cast<char>(0x80u | 126));
        append_be16(frame, static_cast<std::uint16_t>(n));
    } else {
        frame.push_back(static_cast<char>(0x80u | 127));
        for (int i = 7; i >= 0; --i) {
            frame.push_back(static_cast<char>((n >> (i * 8)) & 0xFF));
        }
    }
    frame.append(reinterpret_cast<const char*>(key), 4);
    frame += payload;
    // 就地异或去掩码（编码 = 解码，对称运算）
    char* out = frame.data() + frame.size() - n;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = static_cast<char>(out[i] ^ key[i % 4]);
    }
    return frame;
}

void WsFrameParser::feed(const char* data, std::size_t size) {
    if (error_) return;
    buffer_.append(data, size);
    while (!error_ && step()) {
    }
}

std::vector<WsFrame> WsFrameParser::pop_messages() {
    std::vector<WsFrame> out;
    out.swap(messages_);
    return out;
}

bool WsFrameParser::step() {
    if (buffer_.size() < 2) return false;

    const auto b0 = static_cast<std::uint8_t>(buffer_[0]);
    const auto b1 = static_cast<std::uint8_t>(buffer_[1]);
    const bool fin = (b0 & 0x80u) != 0;
    const auto opcode = static_cast<std::uint8_t>(b0 & 0x0Fu);
    const bool masked = (b1 & 0x80u) != 0;
    std::uint64_t payload_len = b1 & 0x7Fu;

    switch (opcode) {
        case WS_OP_CONTINUATION:
        case WS_OP_TEXT:
        case WS_OP_BINARY:
        case WS_OP_CLOSE:
        case WS_OP_PING:
        case WS_OP_PONG:
            break;
        default:
            error_ = true;
            return false;
    }

    std::size_t header_len = 2;
    if (payload_len == 126) {
        if (buffer_.size() < 4) return false;
        const auto hi = static_cast<std::uint8_t>(buffer_[2]);
        const auto lo = static_cast<std::uint8_t>(buffer_[3]);
        payload_len = (std::uint64_t(hi) << 8) | std::uint64_t(lo);
        header_len = 4;
    } else if (payload_len == 127) {
        if (buffer_.size() < 10) return false;
        payload_len = 0;
        for (std::size_t i = 2; i < 10; ++i) {
            payload_len = (payload_len << 8) |
                          std::uint64_t(static_cast<std::uint8_t>(buffer_[i]));
        }
        header_len = 10;
    }

    if (masked) header_len += 4;

    // 限制消息体积，防止恶意超长帧耗尽内存
    std::uint64_t total = payload_len;
    if (fragmented_ && opcode == WS_OP_CONTINUATION) {
        total += fragment_data_.size();
    }
    if (total > kMaxMessageBytes) {
        error_ = true;
        return false;
    }

    if (buffer_.size() < header_len + payload_len) return false;

    std::uint8_t mask_key[4]{};
    if (masked) {
        for (int i = 0; i < 4; ++i) {
            mask_key[i] = static_cast<std::uint8_t>(
                buffer_[header_len - 4 + static_cast<std::size_t>(i)]);
        }
    }

    WsFrame msg;
    msg.opcode = opcode;
    msg.payload.resize(static_cast<std::size_t>(payload_len));
    const std::size_t payload_off = header_len;
    for (std::size_t i = 0; i < msg.payload.size(); ++i) {
        auto byte = static_cast<std::uint8_t>(buffer_[payload_off + i]);
        if (masked) byte ^= mask_key[i % 4];
        msg.payload[i] = static_cast<char>(byte);
    }
    buffer_.erase(0, static_cast<std::size_t>(header_len + payload_len));

    // 控制帧（ping/pong/close）不参与分片，原样透传
    if (opcode == WS_OP_PING || opcode == WS_OP_PONG || opcode == WS_OP_CLOSE) {
        messages_.push_back(std::move(msg));
        return true;
    }

    if (opcode == WS_OP_CONTINUATION) {
        if (!fragmented_) {
            error_ = true; // 孤立的 continuation 帧
            return false;
        }
        fragment_data_ += msg.payload;
        if (fin) {
            messages_.push_back(
                WsFrame{fragment_opcode_, std::move(fragment_data_)});
            fragmented_ = false;
            fragment_data_.clear();
            fragment_opcode_ = 0;
        }
        return true;
    }

    // 新数据帧（text/binary）
    if (fragmented_) {
        error_ = true; // 分片未结束又开始新数据帧
        return false;
    }
    if (fin) {
        messages_.push_back(std::move(msg));
    } else {
        fragmented_ = true;
        fragment_opcode_ = opcode;
        fragment_data_ = std::move(msg.payload);
    }
    return true;
}

} // namespace falcon::daemon::rpc
