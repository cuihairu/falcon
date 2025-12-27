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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#include <cstring>

namespace falcon::net {

//==============================================================================
// SocketPool 实现
//==============================================================================

std::shared_ptr<PooledSocket> SocketPool::create_connection(const SocketKey& key) {
    // 1. DNS 解析
    struct addrinfo hints, *result = nullptr, *rp;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     // IPv4 或 IPv6
    hints.ai_socktype = SOCK_STREAM; // TCP

    int ret = getaddrinfo(key.host.c_str(), std::to_string(key.port).c_str(), &hints, &result);
    if (ret != 0) {
        FALCON_LOG_ERROR("DNS 解析失败: " << key.host << ": " << gai_strerror(ret));
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
        int flags = fcntl(socket_fd, F_GETFL, 0);
        if (flags < 0 || fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            ::close(socket_fd);
            socket_fd = -1;
            continue;
        }

        // 尝试连接
        if (connect(socket_fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; // 连接成功
        }

        // 对于非阻塞 socket，连接进行中返回 EINPROGRESS
        if (errno == EINPROGRESS) {
            break; // 连接进行中
        }

        // 连接失败，关闭 socket 并尝试下一个地址
        ::close(socket_fd);
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
