/**
 * @file websocket_server.cpp
 * @brief WebSocket 服务器实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "websocket_server.hpp"
#include <falcon/logger.hpp>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <openssl/sha.h>
#include <base64.h>
#include <nlohmann/json.hpp>

namespace falcon {
namespace daemon {
namespace rpc {

using json = nlohmann::json;

// ============================================================================
// WebSocketServer 实现
// ============================================================================

WebSocketServer::WebSocketServer()
    : port_(6801)
    , running_(false)
    , serverSocket_(-1)
    , nextConnectionId_(1) {
}

WebSocketServer::~WebSocketServer() {
    stop();
}

bool WebSocketServer::start(int port) {
    if (running_.load()) {
        FALCON_LOG_WARN("WebSocket server already running");
        return true;
    }

    port_ = port;

    // 创建服务器套接字
    serverSocket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSocket_ < 0) {
        FALCON_LOG_ERROR("Failed to create socket: {}", strerror(errno));
        return false;
    }

    // 设置套接字选项
    int opt = 1;
    if (setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        FALCON_LOG_ERROR("Failed to set socket options: {}", strerror(errno));
        close(serverSocket_);
        return false;
    }

    // 绑定地址
    struct sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);

    if (bind(serverSocket_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        FALCON_LOG_ERROR("Failed to bind socket: {}", strerror(errno));
        close(serverSocket_);
        return false;
    }

    // 开始监听
    if (listen(serverSocket_, 10) < 0) {
        FALCON_LOG_ERROR("Failed to listen on socket: {}", strerror(errno));
        close(serverSocket_);
        return false;
    }

    running_.store(true);

    // 启动服务器线程
    serverThread_ = std::thread(&WebSocketServer::serverLoop, this);

    FALCON_LOG_INFO("WebSocket server started on port {}", port_);
    return true;
}

void WebSocketServer::stop() {
    if (!running_.load()) {
        return;
    }

    running_.store(false);

    // 关闭所有连接
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (const auto& pair : connections_) {
        close(pair.first);
    }
    connections_.clear();

    if (serverSocket_ >= 0) {
        close(serverSocket_);
        serverSocket_ = -1;
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    FALCON_LOG_INFO("WebSocket server stopped");
}

void WebSocketServer::serverLoop() {
    while (running_.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSocket_, &readfds);

        int maxFd = serverSocket_;

        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            for (const auto& pair : connections_) {
                FD_SET(pair.first, &readfds);
                if (pair.first > maxFd) {
                    maxFd = pair.first;
                }
            }
        }

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(maxFd + 1, &readfds, nullptr, nullptr, &timeout);

        if (activity < 0) {
            if (errno != EINTR) {
                FALCON_LOG_ERROR("select error: {}", strerror(errno));
            }
            continue;
        }

        if (activity == 0) {
            continue;  // 超时
        }

        // 新连接
        if (FD_ISSET(serverSocket_, &readfds)) {
            acceptConnection();
        }

        // 处理现有连接
        std::vector<int> connectionsToClose;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            for (const auto& pair : connections_) {
                int sockfd = pair.first;
                if (FD_ISSET(sockfd, &readfds)) {
                    std::vector<uint8_t> buffer(4096);
                    ssize_t bytesRead = recv(sockfd, buffer.data(), buffer.size(), 0);

                    if (bytesRead <= 0) {
                        connectionsToClose.push_back(sockfd);
                    } else {
                        buffer.resize(bytesRead);

                        // 处理 WebSocket 帧
                        try {
                            std::string message = parseFrame(buffer);
                            if (!message.empty() && messageHandler_) {
                                messageHandler_(message);
                            }
                        } catch (const std::exception& e) {
                            FALCON_LOG_ERROR("Error handling WebSocket message: {}", e.what());
                        }
                    }
                }
            }
        }

        // 关闭断开的连接
        for (int sockfd : connectionsToClose) {
            std::lock_guard<std::mutex> lock(connectionsMutex_);
            connections_.erase(sockfd);
            close(sockfd);
            if (onDisconnect_) {
                onDisconnect_(sockfd);
            }
            FALCON_LOG_DEBUG("WebSocket client disconnected: {}", sockfd);
        }
    }
}

void WebSocketServer::acceptConnection() {
    struct sockaddr_in clientAddr;
    socklen_t clientLen = sizeof(clientAddr);

    int clientSocket = accept(serverSocket_, (struct sockaddr*)&clientAddr, &clientLen);
    if (clientSocket < 0) {
        FALCON_LOG_ERROR("Failed to accept connection: {}", strerror(errno));
        return;
    }

    // 执行 WebSocket 握手
    if (!performHandshake(clientSocket)) {
        close(clientSocket);
        return;
    }

    int connectionId = nextConnectionId_++;

    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_[clientSocket] = inet_ntoa(clientAddr.sin_addr);
    }

    if (onConnect_) {
        onConnect_(connectionId);
    }

    FALCON_LOG_INFO("WebSocket client connected: {} (fd: {})",
                   inet_ntoa(clientAddr.sin_addr), clientSocket);
}

bool WebSocketServer::performHandshake(int sockfd) {
    char buffer[1024];
    ssize_t bytesRead = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
        return false;
    }
    buffer[bytesRead] = '\0';

    std::string request(buffer);

    // 提取 Sec-WebSocket-Key
    std::string key;
    std::regex keyRegex("Sec-WebSocket-Key: ([^\\r\\n]+)");
    std::smatch match;
    if (std::regex_search(request, match, keyRegex)) {
        key = match[1].str();
    }

    if (key.empty()) {
        FALCON_LOG_ERROR("No Sec-WebSocket-Key found");
        return false;
    }

    // 计算接受键
    std::string guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string acceptKey = key + guid;

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char*>(acceptKey.c_str()),
         acceptKey.length(), hash);

    std::string encodedKey = base64_encode(hash, SHA_DIGEST_LENGTH);

    // 发送握手响应
    std::ostringstream response;
    response << "HTTP/1.1 101 Switching Protocols\r\n";
    response << "Upgrade: websocket\r\n";
    response << "Connection: Upgrade\r\n";
    response << "Sec-WebSocket-Accept: " << encodedKey << "\r\n";
    response << "\r\n";

    std::string responseStr = response.str();
    send(sockfd, responseStr.c_str(), responseStr.length(), 0);

    return true;
}

std::string WebSocketServer::parseFrame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 2) {
        return "";
    }

    uint8_t fin = frame[0] & 0x80;
    uint8_t opcode = frame[0] & 0x0F;
    uint8_t mask = frame[1] & 0x80;
    uint64_t payloadLen = frame[1] & 0x7F;

    size_t headerLen = 2;

    if (payloadLen == 126) {
        if (frame.size() < 4) return "";
        payloadLen = (frame[2] << 8) | frame[3];
        headerLen = 4;
    } else if (payloadLen == 127) {
        if (frame.size() < 10) return "";
        payloadLen = 0;
        for (int i = 0; i < 8; ++i) {
            payloadLen = (payloadLen << 8) | frame[2 + i];
        }
        headerLen = 10;
    }

    std::vector<uint8_t> maskingKey;
    if (mask) {
        if (frame.size() < headerLen + 4) return "";
        maskingKey.assign(frame.begin() + headerLen, frame.begin() + headerLen + 4);
        headerLen += 4;
    }

    if (frame.size() < headerLen + payloadLen) {
        return "";
    }

    std::string payload(frame.begin() + headerLen, frame.begin() + headerLen + payloadLen);

    if (mask) {
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] ^= maskingKey[i % 4];
        }
    }

    return payload;
}

std::vector<uint8_t> WebSocketServer::createFrame(const std::string& data) {
    std::vector<uint8_t> frame;

    uint8_t firstByte = 0x80;  // FIN
    firstByte |= 0x01;         // Text frame
    frame.push_back(firstByte);

    size_t dataLen = data.size();
    if (dataLen < 126) {
        frame.push_back(static_cast<uint8_t>(dataLen));
    } else if (dataLen < 65536) {
        frame.push_back(126);
        frame.push_back((dataLen >> 8) & 0xFF);
        frame.push_back(dataLen & 0xFF);
    } else {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i) {
            frame.push_back((dataLen >> (i * 8)) & 0xFF);
        }
    }

    frame.insert(frame.end(), data.begin(), data.end());

    return frame;
}

void WebSocketServer::broadcastEvent(const WebSocketEvent& event) {
    std::string json = generateEventJson(event);
    std::vector<uint8_t> frame = createFrame(json);

    std::lock_guard<std::mutex> lock(connectionsMutex_);
    std::vector<int> toClose;

    for (const auto& pair : connections_) {
        int sockfd = pair.first;
        ssize_t sent = send(sockfd, frame.data(), frame.size(), 0);
        if (sent < 0) {
            toClose.push_back(sockfd);
        }
    }

    // 关闭失败的连接
    for (int sockfd : toClose) {
        connections_.erase(sockfd);
        close(sockfd);
        if (onDisconnect_) {
            onDisconnect_(sockfd);
        }
    }
}

void WebSocketServer::sendEvent(int connectionId, const WebSocketEvent& event) {
    std::string json = generateEventJson(event);
    std::vector<uint8_t> frame = createFrame(json);

    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (const auto& pair : connections_) {
        if (pair.first == connectionId) {
            send(pair.first, frame.data(), frame.size(), 0);
            break;
        }
    }
}

std::string WebSocketServer::generateEventJson(const WebSocketEvent& event) {
    json j;
    j["jsonrpc"] = "2.0";
    j["method"] = "aria2.onDownloadStart";
    j["params"][0]["gid"] = event.gid;
    j["params"][0]["type"] = eventTypeToString(event.type);

    for (const auto& pair : event.data) {
        j["params"][0][pair.first] = pair.second;
    }

    return j.dump();
}

std::string WebSocketServer::eventTypeToString(WebSocketEventType type) {
    switch (type) {
        case WebSocketEventType::DownloadStart: return "downloadStart";
        case WebSocketEventType::DownloadPause: return "downloadPause";
        case WebSocketEventType::DownloadComplete: return "downloadComplete";
        case WebSocketEventType::DownloadError: return "downloadError";
        case WebSocketEventType::DownloadProgress: return "downloadProgress";
        case WebSocketEventType::TaskAdded: return "taskAdded";
        case WebSocketEventType::TaskRemoved: return "taskRemoved";
        case WebSocketEventType::BTMetadataComplete: return "btMetadataComplete";
        default: return "unknown";
    }
}

void WebSocketServer::setMessageHandler(WebSocketMessageHandler handler) {
    messageHandler_ = handler;
}

void WebSocketServer::setConnectionHandler(WebSocketConnectionHandler onConnect,
                                           WebSocketConnectionHandler onDisconnect) {
    onConnect_ = onConnect;
    onDisconnect_ = onDisconnect;
}

int WebSocketServer::getConnectionCount() const {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    return connections_.size();
}

} // namespace rpc
} // namespace daemon
} // namespace falcon
