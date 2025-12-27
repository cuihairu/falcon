/**
 * @file websocket_server.hpp
 * @brief WebSocket 服务器实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <thread>
#include <atomic>

namespace falcon {
namespace daemon {
namespace rpc {

/**
 * @enum WebSocketEventType
 * @brief WebSocket 事件类型
 */
enum class WebSocketEventType {
    DownloadStart,
    DownloadPause,
    DownloadComplete,
    DownloadError,
    DownloadProgress,
    TaskAdded,
    TaskRemoved,
    BTMetadataComplete,
    // 更多事件类型...
};

/**
 * @struct WebSocketEvent
 * @brief WebSocket 事件
 */
struct WebSocketEvent {
    WebSocketEventType type;
    std::string gid;           // 任务 GID
    std::map<std::string, std::string> data;  // 事件数据
};

/**
 * @typedef WebSocketMessageHandler
 * @brief WebSocket 消息处理器
 */
using WebSocketMessageHandler = std::function<void(const std::string& message)>;

/**
 * @typedef WebSocketConnectionHandler
 * @brief WebSocket 连接处理器
 */
using WebSocketConnectionHandler = std::function<void(int connectionId)>;

/**
 * @class WebSocketServer
 * @brief WebSocket 服务器
 *
 * 提供实时事件推送功能，支持以下特性：
 * - 下载进度实时更新
 * - 任务状态变化通知
 * - BitTorrent 元数据完成通知
 * - 双向通信（客户端发送命令）
 */
class WebSocketServer {
public:
    /**
     * @brief 构造函数
     */
    WebSocketServer();

    /**
     * @brief 析构函数
     */
    ~WebSocketServer();

    /**
     * @brief 启动服务器
     */
    bool start(int port);

    /**
     * @brief 停止服务器
     */
    void stop();

    /**
     * @brief 广播事件到所有连接的客户端
     */
    void broadcastEvent(const WebSocketEvent& event);

    /**
     * @brief 发送事件到指定连接
     */
    void sendEvent(int connectionId, const WebSocketEvent& event);

    /**
     * @brief 注册消息处理器
     */
    void setMessageHandler(WebSocketMessageHandler handler);

    /**
     * @brief 注册连接处理器
     */
    void setConnectionHandler(WebSocketConnectionHandler onConnect,
                             WebSocketConnectionHandler onDisconnect);

    /**
     * @brief 获取连接的客户端数量
     */
    int getConnectionCount() const;

private:
    int port_;
    std::atomic<bool> running_;
    std::thread serverThread_;

    int serverSocket_;
    std::map<int, std::string> connections_;  // connectionId -> client info
    mutable std::mutex connectionsMutex_;

    WebSocketMessageHandler messageHandler_;
    WebSocketConnectionHandler onConnect_;
    WebSocketConnectionHandler onDisconnect_;

    std::atomic<int> nextConnectionId_;

    /**
     * @brief 服务器主循环
     */
    void serverLoop();

    /**
     * @brief 接受新连接
     */
    void acceptConnection();

    /**
     * @brief 处理连接
     */
    void handleConnection(int connectionId);

    /**
     * @brief WebSocket 握手
     */
    bool performHandshake(int sockfd);

    /**
     * @brief 解析 WebSocket 帧
     */
    std::string parseFrame(const std::vector<uint8_t>& frame);

    /**
     * @brief 创建 WebSocket 帧
     */
    std::vector<uint8_t> createFrame(const std::string& data);

    /**
     * @brief 生成事件 JSON
     */
    std::string generateEventJson(const WebSocketEvent& event);

    /**
     * @brief 事件类型转字符串
     */
    static std::string eventTypeToString(WebSocketEventType type);
};

} // namespace rpc
} // namespace daemon
} // namespace falcon
