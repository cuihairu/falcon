// ============================================================================
// SwarmWsSubscriber 实现（见 swarm_ws_subscriber.hpp 头注释）
// ============================================================================

#include "swarm_ws_subscriber.hpp"

#include <rpc/websocket_frame.hpp>

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using swarm_socket_t = SOCKET;
constexpr swarm_socket_t kSwarmInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using swarm_socket_t = int;
constexpr swarm_socket_t kSwarmInvalidSocket = -1;
#endif

namespace falcon::swarm {

namespace {

// 固定握手 key：RFC 6455 §1.3 示例值（收订客户端不 negotiate 子协议，
// 服务器只回 Accept；本客户端不校验 Accept——握手成败以 101 判定，
// 与测试基建 SwarmWsClient 同姿态）。
constexpr const char* kHandshakeKey = "dGhlIHNhbXBsZSBub25jZQ==";

void close_socket_fd(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(fd);
#endif
}

int open_socket_fd(swarm_socket_t raw) {
    return static_cast<int>(raw);
}

void shutdown_write_read(int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(fd), SD_BOTH);
#else
    ::shutdown(fd, SHUT_RDWR);
#endif
}

/// 发送全部字节（阻塞 send；EINTR 重试）。返回是否全部写出。
bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t sent = 0;
    while (sent < len) {
#ifdef _WIN32
        const int n = ::send(static_cast<SOCKET>(fd), data + sent,
                             static_cast<int>(len - sent), 0);
#else
        const ssize_t n = ::send(fd, data + sent, len - sent, MSG_NOSIGNAL);
#endif
        if (n <= 0) {
#ifdef _WIN32
            const int err = WSAGetLastError();
            if (err == WSAEINTR) continue;
#else
            if (errno == EINTR) continue;
#endif
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace

SwarmWsSubscriber::SwarmWsSubscriber(Options opt) : opt_(std::move(opt)) {}

SwarmWsSubscriber::~SwarmWsSubscriber() { stop(); }

void SwarmWsSubscriber::set_text_handler(TextHandler handler) {
    text_handler_ = std::move(handler);
}

bool SwarmWsSubscriber::start() {
    if (running_.load()) return true;
    if (opt_.port <= 0) return false;
    stop_.store(false);
    worker_ = std::thread([this] { run_loop(); });
    running_.store(true);
    return true;
}

void SwarmWsSubscriber::stop() {
    if (!running_.load()) return;
    stop_.store(true);
    {
        std::lock_guard<std::mutex> lock(fd_mutex_);
        shutdown_write_read(fd_);  // 唤醒阻塞 recv（close 不唤醒，Linux 先例）
    }
    if (worker_.joinable()) worker_.join();
    running_.store(false);
}

int SwarmWsSubscriber::connect_once(std::string* error) {
    connect_attempts_.fetch_add(1);

    // 非阻塞 connect + poll 等可写（connect 超时不受 SO_SNDTIMEO 约束，
    // 必须显式限时；V2 引擎 connect_socket 同形态）。
#ifdef _WIN32
    SOCKET fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (fd == kSwarmInvalidSocket) {
        if (error) *error = "socket() failed";
        return -1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(opt_.port));
    if (::inet_pton(AF_INET, opt_.host.c_str(), &addr.sin_addr) != 1) {
        // 主机名场景极少（回环 e2e 恒为 IP 字面量）；不支持域名解析，
        // 保持 client 零 DNS 依赖（curl 通道负责带域名的 HTTP RPC）。
        if (error) *error = "host is not an IPv4 literal: " + opt_.host;
        close_socket_fd(open_socket_fd(fd));
        return -1;
    }

#ifdef _WIN32
    u_long nonblocking = 1;
    ::ioctlsocket(fd, FIONBIO, &nonblocking);
    const int cr = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr));
    if (cr != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        if (error) *error = "connect failed";
        close_socket_fd(open_socket_fd(fd));
        return -1;
    }
#else
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int cr = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr),
                             sizeof(addr));
    if (cr != 0 && errno != EINPROGRESS) {
        if (error) *error = "connect failed";
        ::close(fd);
        return -1;
    }
#endif

    {
        pollfd pfd{};
#ifdef _WIN32
        pfd.fd = fd;
#else
        pfd.fd = fd;
#endif
        pfd.events = POLLOUT;
        const int pr = ::poll(&pfd, 1,
                              static_cast<int>(opt_.connect_timeout.count()));
        if (pr != 1) {
            if (error) *error = "connect timeout";
            close_socket_fd(open_socket_fd(fd));
            return -1;
        }
        int so_error = 0;
        socklen_t slen = sizeof(so_error);
        ::getsockopt(open_socket_fd(fd), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&so_error), &slen);
        if (so_error != 0) {
            if (error) *error = "connect error";
            close_socket_fd(open_socket_fd(fd));
            return -1;
        }
    }

#ifdef _WIN32
    u_long blocking = 0;
    ::ioctlsocket(fd, FIONBIO, &blocking);
#else
    ::fcntl(fd, F_SETFL, flags);  // 恢复阻塞
#endif

    // ---- WS 握手 ----
    std::string req;
    req.reserve(256);
    req += "GET /jsonrpc HTTP/1.1\r\n";
    req += "Host: " + opt_.host + ":" + std::to_string(opt_.port) + "\r\n";
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += std::string("Sec-WebSocket-Key: ") + kHandshakeKey + "\r\n";
    req += "Sec-WebSocket-Version: 13\r\n";
    if (!opt_.bearer_token.empty()) {
        req += "Authorization: Bearer " + opt_.bearer_token + "\r\n";
    }
    req += "\r\n";
    if (!send_all(open_socket_fd(fd), req.data(), req.size())) {
        if (error) *error = "handshake send failed";
        close_socket_fd(open_socket_fd(fd));
        return -1;
    }

    // 收响应头直到 \r\n\r\n（服务器一发即达，小块即可）
    std::string resp;
    char buf[512];
    while (resp.find("\r\n\r\n") == std::string::npos &&
           resp.size() < 4096) {
#ifdef _WIN32
        const int n = ::recv(static_cast<SOCKET>(fd), buf, sizeof(buf), 0);
#else
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0) {
            if (error) *error = "handshake response incomplete";
            close_socket_fd(open_socket_fd(fd));
            return -1;
        }
        resp.append(buf, static_cast<std::size_t>(n));
    }
    if (resp.rfind("HTTP/1.1 101", 0) != 0 &&
        resp.rfind("HTTP/1.0 101", 0) != 0) {
        if (error) *error = "upgrade rejected: " + resp.substr(0, 32);
        close_socket_fd(open_socket_fd(fd));
        return -1;
    }

    connected_count_.fetch_add(1);
    return open_socket_fd(fd);
}

bool SwarmWsSubscriber::send_frame(int fd, uint8_t opcode,
                                   const std::string& payload) {
    std::lock_guard<std::mutex> lock(fd_mutex_);
    if (fd_ != fd) return false;  // 已被 stop() 收走
    const std::string frame =
        daemon::rpc::ws_encode_client_frame(opcode, payload);
    return send_all(fd, frame.data(), frame.size());
}

void SwarmWsSubscriber::recv_loop(int fd) {
    daemon::rpc::WsFrameParser parser;
    char buf[4096];
    bool open = true;
    while (open && !stop_.load()) {
#ifdef _WIN32
        const int n = ::recv(static_cast<SOCKET>(fd), buf, sizeof(buf), 0);
#else
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;  // 断连（stop() 的 shutdown 也落这里）
        parser.feed(buf, static_cast<std::size_t>(n));
        if (parser.error()) break;  // 帧协议违规 → 重连
        for (const daemon::rpc::WsFrame& f : parser.pop_messages()) {
            switch (f.opcode) {
                case daemon::rpc::WS_OP_TEXT:
                    if (text_handler_) text_handler_(f.payload);
                    break;
                case daemon::rpc::WS_OP_PING:
                    send_frame(fd, daemon::rpc::WS_OP_PONG, f.payload);
                    break;
                case daemon::rpc::WS_OP_CLOSE:
                    open = false;
                    break;
                default:
                    break;  // PONG/BINARY/CONTINUATION（聚合已在 parser 内）
            }
            if (!open) break;
        }
    }
}

void SwarmWsSubscriber::run_loop() {
    while (!stop_.load()) {
        std::string error;
        const int fd = connect_once(&error);
        if (fd < 0) {
            // 退避等待（stop 立即唤醒）
            const auto deadline = std::chrono::steady_clock::now() +
                                  opt_.reconnect_delay;
            while (!stop_.load() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            fd_ = fd;
        }
        recv_loop(fd);

        // 连接收尾：先在锁内置失效（fd 所有权归本线程），再发 CLOSE 帧
        // ——send_frame 内部也要取 fd_mutex_，锁内直接调即自死锁
        bool owned = false;
        {
            std::lock_guard<std::mutex> lock(fd_mutex_);
            if (fd_ == fd) {
                fd_ = -1;
                owned = true;
            }
        }
        if (owned && !stop_.load()) {
            const std::string close_frame =
                daemon::rpc::ws_encode_client_frame(
                    daemon::rpc::WS_OP_CLOSE, "");
            send_all(fd, close_frame.data(), close_frame.size());
        }
        close_socket_fd(fd);
    }
}

}  // namespace falcon::swarm
