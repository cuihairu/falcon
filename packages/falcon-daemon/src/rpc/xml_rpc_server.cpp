/**
 * @file xml_rpc_server.cpp
 * @brief XML-RPC 服务器实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "xml_rpc_server.hpp"
#include <falcon/logger.hpp>
#include <falcon/download_engine.hpp>
#include <falcon/download_task.hpp>

#include <regex>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <conditionvariable>
#include <unordered_map>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
    typedef int socklen_t;
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #define INVALID_SOCKET -1
    #define SOCKET_ERROR -1
    typedef int SOCKET;
#endif

namespace falcon {
namespace daemon {
namespace rpc {

// ============================================================================
// HTTP 服务器实现
// ============================================================================

namespace {
    SOCKET create_server_socket(int port) {
        SOCKET server_fd = INVALID_SOCKET;

#ifdef _WIN32
        WSADATA wsa_data;
        if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
            FALCON_LOG_ERROR("WSAStartup failed");
            return INVALID_SOCKET;
        }
#endif

        server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (server_fd == INVALID_SOCKET) {
            FALCON_LOG_ERROR("Failed to create socket");
            return INVALID_SOCKET;
        }

        // 设置 SO_REUSEADDR
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                   (const char*)&opt,
#else
                   &opt,
#endif
                   sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(static_cast<unsigned short>(port));

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            FALCON_LOG_ERROR("Bind failed on port {}", port);
#ifdef _WIN32
            closesocket(server_fd);
#else
            close(server_fd);
#endif
            return INVALID_SOCKET;
        }

        if (listen(server_fd, 10) < 0) {
            FALCON_LOG_ERROR("Listen failed");
#ifdef _WIN32
            closesocket(server_fd);
#else
            close(server_fd);
#endif
            return INVALID_SOCKET;
        }

        return server_fd;
    }

    void set_nonblocking(SOCKET fd) {
#ifdef _WIN32
        u_long mode = 1;
        ioctlsocket(fd, FIONBIO, &mode);
#else
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
    }

    std::string read_http_request(SOCKET client_fd) {
        char buffer[8192];
        std::string request;

#ifdef _WIN32
        int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
#else
        ssize_t bytes_read = read(client_fd, buffer, sizeof(buffer) - 1);
#endif

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';
            request = buffer;
        }

        return request;
    }

    void send_http_response(SOCKET client_fd, const std::string& body) {
        std::ostringstream response;
        response << "HTTP/1.1 200 OK\r\n";
        response << "Content-Type: text/xml\r\n";
        response << "Content-Length: " << body.length() << "\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        response << body;

        std::string response_str = response.str();
#ifdef _WIN32
        send(client_fd, response_str.c_str(), static_cast<int>(response_str.length()), 0);
#else
        send(client_fd, response_str.c_str(), response_str.length(), 0);
#endif
    }

    void send_http_error(SOCKET client_fd, int code, const std::string& message) {
        std::ostringstream response;
        response << "HTTP/1.1 " << code << " Error\r\n";
        response << "Content-Type: text/plain\r\n";
        response << "Connection: close\r\n";
        response << "\r\n";
        response << message;

        std::string response_str = response.str();
#ifdef _WIN32
        send(client_fd, response_str.c_str(), static_cast<int>(response_str.length()), 0);
#else
        send(client_fd, response_str.c_str(), response_str.length(), 0);
#endif
    }

    std::string extract_xml_body(const std::string& http_request) {
        // 查找空行分隔符（HTTP 头部和身体之间）
        size_t header_end = http_request.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            header_end = http_request.find("\n\n");
        }

        if (header_end != std::string::npos) {
            return http_request.substr(header_end + 4);
        }

        return "";
    }
}

// HTTP 服务器线程
class HTTPServerThread {
public:
    explicit HTTPServerThread(XMLRPCServer* server, int port)
        : server_(server), port_(port), stop_requested_(false) {}

    void start() {
        thread_ = std::thread([this]() { run(); });
    }

    void stop() {
        stop_requested_ = true;
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        SOCKET server_fd = create_server_socket(port_);
        if (server_fd == INVALID_SOCKET) {
            FALCON_LOG_ERROR("Failed to create server socket");
            return;
        }

        FALCON_LOG_INFO("HTTP server listening on port {}", port_);

        while (!stop_requested_) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            SOCKET client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            if (client_fd == INVALID_SOCKET) {
                if (stop_requested_) break;

#ifdef _WIN32
                int error = WSAGetLastError();
                if (error == WSAEWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
#else
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
#endif

                continue;
            }

            // 处理请求
            std::string http_request = read_http_request(client_fd);
            if (!http_request.empty()) {
                // 提取 XML 身体
                std::string xml_body = extract_xml_body(http_request);

                if (!xml_body.empty()) {
                    // 处理 XML-RPC 请求
                    std::string response = server_->handleRequest(xml_body);
                    send_http_response(client_fd, response);
                } else {
                    send_http_error(client_fd, 400, "Bad Request");
                }
            }

#ifdef _WIN32
            closesocket(client_fd);
#else
            close(client_fd);
#endif
        }

#ifdef _WIN32
        closesocket(server_fd);
        WSACleanup();
#else
        close(server_fd);
#endif

        FALCON_LOG_INFO("HTTP server stopped");
    }

    XMLRPCServer* server_;
    int port_;
    std::atomic<bool> stop_requested_;
    std::thread thread_;
};

// ============================================================================
// XMLRPCServer 实现
// ============================================================================

XMLRPCServer::XMLRPCServer()
    : port_(6800)
    , running_(false)
    , http_server_(nullptr) {
    registerBuiltinMethods();
}

XMLRPCServer::~XMLRPCServer() {
    stop();
}

bool XMLRPCServer::start(int port) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (running_) {
        FALCON_LOG_WARN("XML-RPC server already running");
        return true;
    }

    port_ = port;

    // 创建并启动 HTTP 服务器
    http_server_ = std::make_unique<HTTPServerThread>(this, port);
    http_server_->start();

    running_ = true;
    FALCON_LOG_INFO("XML-RPC server started on port {}", port_);

    return true;
}

void XMLRPCServer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        return;
    }

    // 停止 HTTP 服务器
    if (http_server_) {
        http_server_->stop();
        http_server_.reset();
    }

    running_ = false;
    FALCON_LOG_INFO("XML-RPC server stopped");
}

void XMLRPCServer::registerMethod(const std::string& name, RPCMethod handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    methods_[name] = handler;
    FALCON_LOG_DEBUG("Registered XML-RPC method: {}", name);
}

std::string XMLRPCServer::handleRequest(const std::string& xmlRequest) {
    try {
        XMLRPCRequest request = parseRequest(xmlRequest);

        FALCON_LOG_DEBUG("XML-RPC request: {}", request.methodName);

        // 查找方法
        RPCMethod handler;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = methods_.find(request.methodName);
            if (it == methods_.end()) {
                auto response = XMLRPCResponse::fault(1, "Method not found: " + request.methodName);
                return generateResponse(response);
            }
            handler = it->second;
        }

        // 调用方法
        XMLRPCResponse response = handler(request.params);
        return generateResponse(response);

    } catch (const std::exception& e) {
        FALCON_LOG_ERROR("XML-RPC error: {}", e.what());
        auto response = XMLRPCResponse::fault(2, std::string("Internal error: ") + e.what());
        return generateResponse(response);
    }
}

XMLRPCRequest XMLRPCServer::parseRequest(const std::string& xml) {
    XMLRPCRequest request;

    // 解析方法名
    std::regex methodCallRegex("<methodName>([^<]+)</methodName>");
    std::smatch match;
    if (std::regex_search(xml, match, methodCallRegex)) {
        request.methodName = match[1].str();
    }

    // 解析参数
    size_t pos = 0;
    std::regex paramStartRegex("<param>");
    std::regex valueStartRegex("<value>");

    while (true) {
        // 查找 <param>
        size_t param_pos = xml.find("<param>", pos);
        if (param_pos == std::string::npos) break;

        // 查找对应的 </param>
        size_t param_end = xml.find("</param>", param_pos);
        if (param_end == std::string::npos) break;

        // 提取参数内容
        size_t value_start = xml.find("<value>", param_pos);
        if (value_start != std::string::npos && value_start < param_end) {
            size_t value_end = xml.find("</value>", value_start);
            if (value_end != std::string::npos && value_end < param_end) {
                std::string value_content = xml.substr(value_start + 7, value_end - value_start - 7);
                request.params.push_back(parseValue(value_content, pos));
            }
        }

        pos = param_end + 8;
    }

    return request;
}

XMLRPCValue XMLRPCServer::parseValue(const std::string& xml, size_t& pos) {
    XMLRPCValue value;

    // 检测类型
    if (xml.find("<string>") == 0) {
        value.type = XMLRPCValue::String;
        size_t end = xml.find("</string>");
        if (end != std::string::npos) {
            value.stringValue = xml.substr(8, end - 8);
        }
    } else if (xml.find("<int>") == 0 || xml.find("<i4>") == 0) {
        value.type = XMLRPCValue::Integer;
        size_t start_tag = xml.find(">") + 1;
        size_t end = xml.find("</int>");
        if (end == std::string::npos) {
            end = xml.find("</i4>");
        }
        if (end != std::string::npos && start_tag != std::string::npos) {
            value.intValue = std::stoi(xml.substr(start_tag, end - start_tag));
        }
    } else if (xml.find("<boolean>") == 0) {
        value.type = XMLRPCValue::Boolean;
        size_t start_tag = xml.find(">") + 1;
        size_t end = xml.find("</boolean>");
        if (end != std::string::npos && start_tag != std::string::npos) {
            std::string bool_str = xml.substr(start_tag, end - start_tag);
            value.boolValue = (bool_str == "1");
        }
    } else if (xml.find("<double>") == 0) {
        value.type = XMLRPCValue::Double;
        size_t start_tag = xml.find(">") + 1;
        size_t end = xml.find("</double>");
        if (end != std::string::npos && start_tag != std::string::npos) {
            value.doubleValue = std::stod(xml.substr(start_tag, end - start_tag));
        }
    } else if (xml.find("<array>") == 0) {
        value.type = XMLRPCValue::Array;
        size_t data_start = xml.find("<data>");
        if (data_start != std::string::npos) {
            size_t pos = data_start + 6;
            while (true) {
                size_t value_start = xml.find("<value>", pos);
                if (value_start == std::string::npos) break;
                size_t value_end = xml.find("</value>", value_start);
                if (value_end == std::string::npos) break;

                std::string value_content = xml.substr(value_start + 7, value_end - value_start - 7);
                size_t dummy_pos = 0;
                value.arrayValue.push_back(parseValue(value_content, dummy_pos));

                pos = value_end + 8;
            }
        }
    } else if (xml.find("<struct>") == 0) {
        value.type = XMLRPCValue::Struct;
        size_t pos = 8; // skip <struct>
        while (true) {
            size_t member_start = xml.find("<member>", pos);
            if (member_start == std::string::npos) break;
            size_t member_end = xml.find("</member>", member_start);
            if (member_end == std::string::npos) break;

            // 解析 name
            size_t name_start = xml.find("<name>", member_start);
            size_t name_end = xml.find("</name>", member_start);
            if (name_start != std::string::npos && name_end != std::string::npos) {
                std::string member_name = xml.substr(name_start + 6, name_end - name_start - 6);

                // 解析 value
                size_t value_start = xml.find("<value>", name_end);
                size_t value_end = xml.find("</value>", value_start);
                if (value_start != std::string::npos && value_end != std::string::npos) {
                    std::string value_content = xml.substr(value_start + 7, value_end - value_start - 7);
                    size_t dummy_pos = 0;
                    XMLRPCValue member_value = parseValue(value_content, dummy_pos);
                    value.structValue[member_name] = member_value;
                }
            }

            pos = member_end + 9;
        }
    } else if (xml.find("<nil/>") == 0) {
        value.type = XMLRPCValue::Nil;
    } else {
        // 默认为字符串
        value.type = XMLRPCValue::String;
        value.stringValue = xml;
    }

    return value;
}

std::string XMLRPCServer::generateResponse(const XMLRPCResponse& response) {
    std::ostringstream xml;

    xml << "<?xml version=\"1.0\"?>\n";
    xml << "<methodResponse>\n";

    if (response.isFault) {
        xml << "  <fault>\n";
        xml << "    <value>\n";
        xml << "      <struct>\n";
        xml << "        <member>\n";
        xml << "          <name>faultCode</name>\n";
        xml << "          <value><int>" << response.faultCode << "</int></value>\n";
        xml << "        </member>\n";
        xml << "        <member>\n";
        xml << "          <name>faultString</name>\n";
        xml << "          <value><string>" << response.faultString << "</string></value>\n";
        xml << "        </member>\n";
        xml << "      </struct>\n";
        xml << "    </value>\n";
        xml << "  </fault>\n";
    } else {
        xml << "  <params>\n";
        xml << "    <param>\n";
        xml << "      <value>\n";
        xml << generateValue(response.result);
        xml << "      </value>\n";
        xml << "    </param>\n";
        xml << "  </params>\n";
    }

    xml << "</methodResponse>\n";

    return xml.str();
}

std::string XMLRPCServer::generateValue(const XMLRPCValue& value) {
    std::ostringstream xml;

    switch (value.type) {
        case XMLRPCValue::String:
            xml << "<string>" << value.stringValue << "</string>";
            break;
        case XMLRPCValue::Integer:
            xml << "<int>" << value.intValue << "</int>";
            break;
        case XMLRPCValue::Boolean:
            xml << "<boolean>" << (value.boolValue ? "1" : "0") << "</boolean>";
            break;
        case XMLRPCValue::Double:
            xml << "<double>" << std::fixed << std::setprecision(6) << value.doubleValue << "</double>";
            break;
        case XMLRPCValue::Array:
            xml << "<array>\n";
            xml << "  <data>\n";
            for (const auto& item : value.arrayValue) {
                xml << "    <value>" << generateValue(item) << "</value>\n";
            }
            xml << "  </data>\n";
            xml << "</array>";
            break;
        case XMLRPCValue::Struct:
            xml << "<struct>\n";
            for (const auto& member : value.structValue) {
                xml << "  <member>\n";
                xml << "    <name>" << member.first << "</name>\n";
                xml << "    <value>" << generateValue(member.second) << "</value>\n";
                xml << "  </member>\n";
            }
            xml << "</struct>";
            break;
        case XMLRPCValue::Nil:
            xml << "<nil/>";
            break;
    }

    return xml.str();
}

void XMLRPCServer::registerBuiltinMethods() {
    // aria2 兼容方法

    // aria2.addUri
    registerMethod("aria2.addUri", [this](const std::vector<XMLRPCValue>& params) {
        // params[0]: uris (array of strings)
        // params[1]: options (struct, optional)
        // params[2]: id (string, optional)

        if (params.empty() || params[0].type != XMLRPCValue::Array) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        if (!download_engine_) {
            return XMLRPCResponse::fault(2, "Download engine not available");
        }

        // 提取 URL 列表
        std::vector<std::string> urls;
        for (const auto& url_val : params[0].arrayValue) {
            if (url_val.type == XMLRPCValue::String) {
                urls.push_back(url_val.stringValue);
            }
        }

        if (urls.empty()) {
            return XMLRPCResponse::fault(1, "No valid URLs provided");
        }

        // 解析下载选项
        DownloadOptions options;
        if (params.size() > 1 && params[1].type == XMLRPCValue::Struct) {
            for (const auto& [key, val] : params[1].structValue) {
                if (val.type == XMLRPCValue::String) {
                    if (key == "dir") {
                        options.output_directory = val.stringValue;
                    } else if (key == "out") {
                        options.output_filename = val.stringValue;
                    } else if (key == "user-agent") {
                        options.user_agent = val.stringValue;
                    } else {
                        options.headers[key] = val.stringValue;
                    }
                } else if (val.type == XMLRPCValue::Integer && key == "split") {
                    options.max_connections = static_cast<std::size_t>(val.intValue);
                }
            }
        }

        // 添加下载任务
        std::vector<std::string> gids;
        if (urls.size() == 1) {
            auto task = download_engine_->add_task(urls[0], options);
            if (task) {
                std::string gid = task_id_to_gid(task->id());
                gids.push_back(gid);
                FALCON_LOG_INFO_STREAM("Added download task: " << urls[0] << " -> GID: " << gid);
            }
        } else {
            auto tasks = download_engine_->add_tasks(urls, options);
            for (const auto& task : tasks) {
                std::string gid = task_id_to_gid(task->id());
                gids.push_back(gid);
            }
        }

        if (gids.empty()) {
            return XMLRPCResponse::fault(3, "Failed to add download task");
        }

        // 返回 GID 列表
        XMLRPCValue result = XMLRPCValue::Array;
        result.arrayValue.clear();
        for (const auto& gid : gids) {
            result.arrayValue.push_back(XMLRPCValue::fromString(gid));
        }
        return XMLRPCResponse::success(result);
    });

    // aria2.remove
    registerMethod("aria2.remove", [this](const std::vector<XMLRPCValue>& params) {
        if (params.empty()) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        if (!download_engine_) {
            return XMLRPCResponse::fault(2, "Download engine not available");
        }

        // 支持单个 GID 或 GID 数组
        std::vector<TaskId> task_ids;
        if (params[0].type == XMLRPCValue::String) {
            TaskId id = gid_to_task_id(params[0].stringValue);
            if (id != 0) {
                task_ids.push_back(id);
            }
        } else if (params[0].type == XMLRPCValue::Array) {
            for (const auto& gid_val : params[0].arrayValue) {
                if (gid_val.type == XMLRPCValue::String) {
                    TaskId id = gid_to_task_id(gid_val.stringValue);
                    if (id != 0) {
                        task_ids.push_back(id);
                    }
                }
            }
        }

        if (task_ids.empty()) {
            return XMLRPCResponse::fault(1, "Invalid GID");
        }

        // 移除任务
        std::uint64_t removed_count = 0;
        for (TaskId id : task_ids) {
            if (download_engine_->remove_task(id)) {
                ++removed_count;
            }
        }

        // 返回移除的任务 GID 列表
        XMLRPCValue result = XMLRPCValue::Array;
        result.arrayValue.clear();
        for (TaskId id : task_ids) {
            std::string gid = task_id_to_gid(id);
            result.arrayValue.push_back(XMLRPCValue::fromString(gid));
        }
        return XMLRPCResponse::success(result);
    });

    // aria2.pause
    registerMethod("aria2.pause", [this](const std::vector<XMLRPCValue>& params) {
        if (params.empty()) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        if (!download_engine_) {
            return XMLRPCResponse::fault(2, "Download engine not available");
        }

        // 支持单个 GID 或 GID 数组
        std::vector<TaskId> task_ids;
        if (params[0].type == XMLRPCValue::String) {
            TaskId id = gid_to_task_id(params[0].stringValue);
            if (id != 0) {
                task_ids.push_back(id);
            }
        } else if (params[0].type == XMLRPCValue::Array) {
            for (const auto& gid_val : params[0].arrayValue) {
                if (gid_val.type == XMLRPCValue::String) {
                    TaskId id = gid_to_task_id(gid_val.stringValue);
                    if (id != 0) {
                        task_ids.push_back(id);
                    }
                }
            }
        }

        // 暂停任务
        std::uint64_t paused_count = 0;
        for (TaskId id : task_ids) {
            if (download_engine_->pause_task(id)) {
                ++paused_count;
            }
        }

        // 返回暂停的任务 GID 列表
        XMLRPCValue result = XMLRPCValue::Array;
        result.arrayValue.clear();
        for (TaskId id : task_ids) {
            std::string gid = task_id_to_gid(id);
            result.arrayValue.push_back(XMLRPCValue::fromString(gid));
        }
        return XMLRPCResponse::success(result);
    });

    // aria2.unpause (resume)
    registerMethod("aria2.unpause", [this](const std::vector<XMLRPCValue>& params) {
        if (params.empty()) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        if (!download_engine_) {
            return XMLRPCResponse::fault(2, "Download engine not available");
        }

        // 支持单个 GID 或 GID 数组
        std::vector<TaskId> task_ids;
        if (params[0].type == XMLRPCValue::String) {
            TaskId id = gid_to_task_id(params[0].stringValue);
            if (id != 0) {
                task_ids.push_back(id);
            }
        } else if (params[0].type == XMLRPCValue::Array) {
            for (const auto& gid_val : params[0].arrayValue) {
                if (gid_val.type == XMLRPCValue::String) {
                    TaskId id = gid_to_task_id(gid_val.stringValue);
                    if (id != 0) {
                        task_ids.push_back(id);
                    }
                }
            }
        }

        // 恢复任务
        std::uint64_t resumed_count = 0;
        for (TaskId id : task_ids) {
            if (download_engine_->resume_task(id)) {
                ++resumed_count;
            }
        }

        // 返回恢复的任务 GID 列表
        XMLRPCValue result = XMLRPCValue::Array;
        result.arrayValue.clear();
        for (TaskId id : task_ids) {
            std::string gid = task_id_to_gid(id);
            result.arrayValue.push_back(XMLRPCValue::fromString(gid));
        }
        return XMLRPCResponse::success(result);
    });

    // aria2.tellStatus
    registerMethod("aria2.tellStatus", [this](const std::vector<XMLRPCValue>& params) {
        if (params.empty() || params[0].type != XMLRPCValue::String) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        if (!download_engine_) {
            return XMLRPCResponse::fault(2, "Download engine not available");
        }

        std::string gid = params[0].stringValue;
        TaskId id = gid_to_task_id(gid);

        auto task = download_engine_->get_task(id);
        if (!task) {
            return XMLRPCResponse::fault(3, "Task not found");
        }

        // 构建状态响应
        XMLRPCValue status = XMLRPCValue::Struct;
        status.structValue["gid"] = XMLRPCValue::fromString(gid);

        // 状态字符串
        std::string status_str = "unknown";
        switch (task->status()) {
            case TaskStatus::Pending: status_str = "waiting"; break;
            case TaskStatus::Preparing: status_str = "waiting"; break;
            case TaskStatus::Downloading: status_str = "active"; break;
            case TaskStatus::Paused: status_str = "paused"; break;
            case TaskStatus::Completed: status_str = "complete"; break;
            case TaskStatus::Failed: status_str = "error"; break;
            case TaskStatus::Cancelled: status_str = "removed"; break;
        }
        status.structValue["status"] = XMLRPCValue::fromString(status_str);

        // 文件信息
        status.structValue["totalLength"] = XMLRPCValue::fromString(
            std::to_string(task->total_bytes()));
        status.structValue["completedLength"] = XMLRPCValue::fromString(
            std::to_string(task->downloaded_bytes()));
        status.structValue["downloadSpeed"] = XMLRPCValue::fromString(
            std::to_string(task->speed()));
        status.structValue["fileName"] = XMLRPCValue::fromString(
            task->options().output_filename);
        status.structValue["dir"] = XMLRPCValue::fromString(
            task->options().output_directory);

        // 进度百分比
        int progress = static_cast<int>(task->progress() * 100.0f);
        status.structValue["progress"] = XMLRPCValue::fromInt(progress);

        return XMLRPCResponse::success(status);
    });

    // aria2.getGlobalStat
    registerMethod("aria2.getGlobalStat", [this](const std::vector<XMLRPCValue>& params) {
        if (!download_engine_) {
            return XMLRPCResponse::fault(2, "Download engine not available");
        }

        XMLRPCValue stats = XMLRPCValue::Struct;
        stats.structValue["numActive"] = XMLRPCValue::fromString(
            std::to_string(download_engine_->get_active_task_count()));
        stats.structValue["numWaiting"] = XMLRPCValue::fromString(
            std::to_string(download_engine_->get_total_task_count() -
                          download_engine_->get_active_task_count()));
        stats.structValue["numStopped"] = XMLRPCValue::fromString("0");
        stats.structValue["globalDownloadSpeed"] = XMLRPCValue::fromString(
            std::to_string(download_engine_->get_total_speed()));
        stats.structValue["globalUploadSpeed"] = XMLRPCValue::fromString("0");

        return XMLRPCResponse::success(stats);
    });

    // aria2.getVersion
    registerMethod("aria2.getVersion", [](const std::vector<XMLRPCValue>& params) {
        XMLRPCValue version = XMLRPCValue::Struct;
        version.structValue["version"] = XMLRPCValue::fromString("1.0.0");
        version.structValue["enabledFeatures"] = XMLRPCValue::fromString("http https ftp bittorrent sftp metalink");

        return XMLRPCResponse::success(version);
    });

    // aria2.listMethods
    registerMethod("system.listMethods", [this](const std::vector<XMLRPCValue>& params) {
        std::vector<XMLRPCValue> methods;
        for (const auto& pair : methods_) {
            methods.push_back(XMLRPCValue::fromString(pair.first));
        }

        XMLRPCValue result = XMLRPCValue::Array;
        result.arrayValue = methods;

        return XMLRPCResponse::success(result);
    });
}

//==============================================================================
// GID 管理辅助方法
//==============================================================================

std::string XMLRPCServer::generate_gid() {
    std::ostringstream gid;
    gid << std::hex << std::setfill('0') << std::setw(16) << next_gid_++;
    return gid.str();
}

std::string XMLRPCServer::task_id_to_gid(TaskId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = task_id_to_gid_.find(id);
    if (it != task_id_to_gid_.end()) {
        return it->second;
    }
    // 如果不存在，生成新的 GID
    std::string gid = generate_gid();
    task_id_to_gid_[id] = gid;
    gid_to_task_id_[gid] = id;
    return gid;
}

TaskId XMLRPCServer::gid_to_task_id(const std::string& gid) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = gid_to_task_id_.find(gid);
    if (it != gid_to_task_id_.end()) {
        return it->second;
    }
    return 0;
}

} // namespace rpc
} // namespace daemon
} // namespace falcon
