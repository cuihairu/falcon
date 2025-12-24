/**
 * @file socket_pool.hpp
 * @brief Socket 连接池 - aria2 风格
 * @author Falcon Team
 * @date 2025-12-24
 *
 * 设计参考: aria2 的 Socket 连接复用机制
 */

#pragma once

#include <falcon/types.hpp>
#include <falcon/logger.hpp>
#include <string>
#include <memory>
#include <map>
#include <chrono>
#include <sys/socket.h>
#include <unistd.h>

namespace falcon::net {

/**
 * @brief Socket 连接的键（用于连接复用）
 */
struct SocketKey {
    std::string host;
    uint16_t port;
    std::string username;
    std::string proxy;

    bool operator<(const SocketKey& other) const {
        return std::tie(host, port, username, proxy) <
               std::tie(other.host, other.port, other.username, other.proxy);
    }

    bool operator==(const SocketKey& other) const {
        return host == other.host &&
               port == other.port &&
               username == other.username &&
               proxy == other.proxy;
    }

    std::string to_string() const {
        return host + ":" + std::to_string(port);
    }
};

/**
 * @brief 池化的 Socket 连接
 */
class PooledSocket {
public:
    PooledSocket(int fd, const SocketKey& key)
        : fd_(fd)
        , key_(key)
        , last_used_(std::chrono::steady_clock::now())
    {
    }

    ~PooledSocket() {
        close_fd();
    }

    // 禁止拷贝
    PooledSocket(const PooledSocket&) = delete;
    PooledSocket& operator=(const PooledSocket&) = delete;

    // 支持移动
    PooledSocket(PooledSocket&& other) noexcept
        : fd_(other.fd_)
        , key_(std::move(other.key_))
        , last_used_(other.last_used_)
    {
        other.fd_ = -1;
    }

    PooledSocket& operator=(PooledSocket&& other) noexcept {
        if (this != &other) {
            close_fd();
            fd_ = other.fd_;
            key_ = std::move(other.key_);
            last_used_ = other.last_used_;
            other.fd_ = -1;
        }
        return *this;
    }

    int fd() const noexcept { return fd_; }
    const SocketKey& key() const { return key_; }

    /**
     * @brief 检查连接是否仍然有效
     */
    bool is_valid() const {
        if (fd_ < 0) return false;

        // 使用 getsockopt 检查连接状态
        int error = 0;
        socklen_t len = sizeof(error);
        return getsockopt(fd_, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0;
    }

    /**
     * @brief 获取空闲时间
     */
    std::chrono::seconds idle_time() const {
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - last_used_);
    }

    /**
     * @brief 更新最后使用时间
     */
    void touch() {
        last_used_ = std::chrono::steady_clock::now();
    }

    /**
     * @brief 关闭 Socket
     */
    void close_fd() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_;
    SocketKey key_;
    std::chrono::steady_clock::time_point last_used_;
};

/**
 * @brief Socket 连接池 - aria2 风格
 *
 * 复用已建立的连接以提高性能
 *
 * 特性:
 * - 自动连接复用
 * - 超时自动清理
 * - 支持多连接到同一服务器
 */
class SocketPool {
public:
    /**
     * @brief 构造函数
     *
     * @param timeout 空闲连接超时时间（秒）
     * @param max_idle 最大空闲连接数
     */
    explicit SocketPool(std::chrono::seconds timeout = std::chrono::seconds(30),
                       std::size_t max_idle = 16)
        : timeout_(timeout)
        , max_idle_(max_idle)
    {
        FALCON_LOG_INFO("创建 SocketPool: timeout=" << timeout.count() << "s");
    }

    ~SocketPool() {
        clear();
        FALCON_LOG_INFO("销毁 SocketPool");
    }

    // 禁止拷贝和移动
    SocketPool(const SocketPool&) = delete;
    SocketPool& operator=(const SocketPool&) = delete;

    /**
     * @brief 获取或创建连接
     *
     * @param key 连接键
     * @return Socket 连接，失败返回 nullptr
     */
    std::shared_ptr<PooledSocket> acquire(const SocketKey& key);

    /**
     * @brief 归还连接到池中
     *
     * @param socket Socket 连接
     */
    void release(std::shared_ptr<PooledSocket> socket);

    /**
     * @brief 清理过期连接
     *
     * @return 清理的连接数量
     */
    std::size_t cleanup_expired();

    /**
     * @brief 清空所有连接
     */
    void clear();

    /**
     * @brief 获取池中连接数量
     */
    std::size_t size() const {
        return pool_.size();
    }

    /**
     * @brief 获取统计信息
     */
    struct Stats {
        std::size_t total_connections = 0;
        std::size_t active_connections = 0;
        std::size_t idle_connections = 0;
    };
    Stats get_stats() const;

private:
    std::chrono::seconds timeout_;
    std::size_t max_idle_;

    // 连接池：key -> 连接列表（支持多连接到同一服务器）
    std::map<SocketKey, std::vector<std::shared_ptr<PooledSocket>>> pool_;

    /**
     * @brief 查找可用连接
     */
    std::shared_ptr<PooledSocket> find_available(const SocketKey& key);

    /**
     * @brief 创建新连接
     */
    std::shared_ptr<PooledSocket> create_connection(const SocketKey& key);
};

//==============================================================================
// 内联函数实现
//==============================================================================

inline std::shared_ptr<PooledSocket> SocketPool::acquire(const SocketKey& key) {
    // 首先尝试查找可用的空闲连接
    auto socket = find_available(key);
    if (socket && socket->is_valid()) {
        FALCON_LOG_DEBUG("复用 Socket 连接: " << key.to_string());
        socket->touch();
        return socket;
    }

    // 没有可用连接，创建新的
    FALCON_LOG_DEBUG("创建新 Socket 连接: " << key.to_string());
    return create_connection(key);
}

inline std::shared_ptr<PooledSocket> SocketPool::find_available(const SocketKey& key) {
    auto it = pool_.find(key);
    if (it == pool_.end() || it->second.empty()) {
        return nullptr;
    }

    // 查找空闲且有效的连接
    for (auto& socket : it->second) {
        if (socket && socket.use_count() == 1 && socket->is_valid()) {
            return socket;
        }
    }

    return nullptr;
}

inline void SocketPool::release(std::shared_ptr<PooledSocket> socket) {
    if (!socket) return;

    const SocketKey& key = socket->key();
    pool_[key].push_back(socket);

    FALCON_LOG_DEBUG("归还 Socket 连接: " << key.to_string());

    // 如果连接数超过限制，清理最老的连接
    if (pool_[key].size() > max_idle_) {
        cleanup_expired();
    }
}

inline std::size_t SocketPool::cleanup_expired() {
    std::size_t cleaned = 0;
    auto now = std::chrono::steady_clock::now();

    for (auto& pair : pool_) {
        auto& sockets = pair.second;

        // 移除过期或无效的连接
        sockets.erase(
            std::remove_if(sockets.begin(), sockets.end(),
                [now, this](const std::shared_ptr<PooledSocket>& socket) {
                    if (!socket || socket.use_count() > 1) {
                        return false;  // 仍在使用中
                    }
                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                        now - std::chrono::steady_clock::time_point{});  // 简化处理
                    return socket->idle_time() > timeout_ || !socket->is_valid();
                }),
            sockets.end());

        cleaned += sockets.size();
    }

    if (cleaned > 0) {
        FALCON_LOG_DEBUG("清理过期 Socket 连接: count=" << cleaned);
    }

    return cleaned;
}

inline void SocketPool::clear() {
    pool_.clear();
    FALCON_LOG_DEBUG("清空 Socket 连接池");
}

inline SocketPool::Stats SocketPool::get_stats() const {
    Stats stats;
    for (const auto& pair : pool_) {
        stats.total_connections += pair.second.size();
        for (const auto& socket : pair.second) {
            if (socket && socket->is_valid()) {
                if (socket.use_count() == 1) {
                    stats.idle_connections++;
                } else {
                    stats.active_connections++;
                }
            }
        }
    }
    return stats;
}

} // namespace falcon::net
