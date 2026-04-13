/**
 * @file socket_pool.cpp
 * @brief Socket 连接池实现
 * @author Falcon Team
 * @date 2025-12-25
 */

#include <falcon/net/socket_pool.hpp>
#include <falcon/logger.hpp>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cstring>
#include <cctype>

namespace falcon::net {

//==============================================================================
// SocketPool 实现
//==============================================================================

std::shared_ptr<PooledSocket> SocketPool::create_connection(const SocketKey& key) {
    if (key.host.empty() || key.port == 0) {
        FALCON_LOG_ERROR("连接参数无效: " << key.to_string());
        return nullptr;
    }
    if (std::any_of(key.host.begin(), key.host.end(),
                    [](unsigned char ch) { return std::isspace(ch) != 0; })) {
        FALCON_LOG_ERROR("主机名包含空白字符: " << key.host);
        return nullptr;
    }

    // 1. DNS 解析
    struct addrinfo hints, *result = nullptr, *rp;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP

    int ret = getaddrinfo(key.host.c_str(), std::to_string(key.port).c_str(), &hints, &result);
    if (ret != 0) {
#ifdef _WIN32
        FALCON_LOG_ERROR("DNS 解析失败: " << key.host << ": error=" << ret);
#else
        FALCON_LOG_ERROR("DNS 解析失败: " << key.host << ": " << gai_strerror(ret));
#endif
        return nullptr;
    }

    // 2. 尝试连接到每个地址
    int socket_fd = -1;
    for (rp = result; rp != nullptr; rp = rp->ai_next) {
        socket_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (socket_fd < 0) {
            continue; // 失败，尝试下一个地址
        }

        // 设置为非阻塞模式
#ifdef _WIN32
        u_long mode = 1;
        if (ioctlsocket(socket_fd, FIONBIO, &mode) != 0) {
            closesocket(socket_fd);
            socket_fd = -1;
            continue;
        }
#else
        int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }
#endif

        // 尝试连接
        if (connect(socket_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // 连接成功
        }

        // 对于非阻塞 socket，等待连接建立完成
#ifdef _WIN32
        if (WSAGetLastError() == WSAEWOULDBLOCK) {
#else
        if (errno == EINPROGRESS) {
#endif
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(socket_fd, &write_fds);

            timeval timeout{};
            timeout.tv_sec = std::min<std::chrono::seconds>(timeout_, std::chrono::seconds(1)).count();
            timeout.tv_usec = 0;

            int ready = select(socket_fd + 1, nullptr, &write_fds, nullptr, &timeout);
            if (ready > 0 && FD_ISSET(socket_fd, &write_fds)) {
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
#ifdef _WIN32
                if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char*>(&socket_error), &error_len) == 0 &&
                    socket_error == 0) {
#else
                if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) == 0 &&
                    socket_error == 0) {
#endif
                    break;
                }
            }

#ifdef _WIN32
            closesocket(socket_fd);
#else
            ::close(socket_fd);
#endif
            socket_fd = -1;
            continue;
        }

        // 连接失败，关闭 socket 并尝试下一个地址
#ifdef _WIN32
        closesocket(socket_fd);
#else
        ::close(socket_fd);
#endif
        socket_fd = -1;
    }

    freeaddrinfo(result);

    if (socket_fd < 0) {
        FALCON_LOG_ERROR("连接失败: " << key.to_string());
        return nullptr;
    }

    FALCON_LOG_DEBUG("创建新 Socket 连接: " << key.to_string() << ", fd=" << socket_fd);

    // 3. 创建 PooledSocket 对象
    return std::make_shared<PooledSocket>(socket_fd, key);
}

} // namespace falcon::net
