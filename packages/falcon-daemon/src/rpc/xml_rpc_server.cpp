/**
 * @file xml_rpc_server.cpp
 * @brief XML-RPC 服务器实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#include "xml_rpc_server.hpp"
#include <falcon/logger.hpp>
#include <regex>
#include <sstream>
#include <iomanip>

namespace falcon {
namespace daemon {
namespace rpc {

// ============================================================================
// XMLRPCServer 实现
// ============================================================================

XMLRPCServer::XMLRPCServer()
    : port_(6800)
    , running_(false) {
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

    // TODO: 实现 HTTP 服务器
    // 这里需要集成 HTTP 服务器库（如 microhttpd、cpp-httplib）
    // 暂时只标记为运行状态

    running_ = true;
    FALCON_LOG_INFO("XML-RPC server started on port {}", port_);

    return true;
}

void XMLRPCServer::stop() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_) {
        return;
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

    // 简化的 XML 解析（生产环境应使用完整的 XML 解析库）
    std::regex methodCallRegex("<methodName>([^<]+)</methodName>");
    std::regex paramRegex("<param>(?:<value>(?:<[^>]+>)?([^<]*)(?:</[^>]+>)?</value>)?</param>");

    std::smatch match;
    if (std::regex_search(xml, match, methodCallRegex)) {
        request.methodName = match[1].str();
    }

    // TODO: 解析参数
    // 这里需要完整的 XML-RPC 参数解析

    return request;
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
    registerMethod("aria2.addUri", [](const std::vector<XMLRPCValue>& params) {
        // params[0]: uris (array of strings)
        // params[1]: options (struct, optional)
        // params[2]: id (string, optional)

        if (params.empty() || params[0].type != XMLRPCValue::Array) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        // TODO: 实际添加下载任务
        std::string gid = "123456";  // 生成的 GID

        XMLRPCValue result = XMLRPCValue::fromString(gid);
        return XMLRPCResponse::success(result);
    });

    // aria2.remove
    registerMethod("aria2.remove", [](const std::vector<XMLRPCValue>& params) {
        if (params.empty() || params[0].type != XMLRPCValue::String) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        std::string gid = params[0].stringValue;

        // TODO: 实际移除下载任务
        bool success = true;

        XMLRPCValue result = XMLRPCValue::fromBool(success);
        return XMLRPCResponse::success(result);
    });

    // aria2.pause
    registerMethod("aria2.pause", [](const std::vector<XMLRPCValue>& params) {
        if (params.empty()) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        // TODO: 实际暂停下载任务
        bool success = true;

        XMLRPCValue result = XMLRPCValue::fromBool(success);
        return XMLRPCResponse::success(result);
    });

    // aria2.unpause
    registerMethod("aria2.unpause", [](const std::vector<XMLRPCValue>& params) {
        if (params.empty()) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        // TODO: 实际恢复下载任务
        bool success = true;

        XMLRPCValue result = XMLRPCValue::fromBool(success);
        return XMLRPCResponse::success(result);
    });

    // aria2.tellStatus
    registerMethod("aria2.tellStatus", [](const std::vector<XMLRPCValue>& params) {
        if (params.empty() || params[0].type != XMLRPCValue::String) {
            return XMLRPCResponse::fault(1, "Invalid parameters");
        }

        std::string gid = params[0].stringValue;

        // TODO: 实际获取任务状态
        XMLRPCValue status = XMLRPCValue::Struct;
        status.structValue["gid"] = XMLRPCValue::fromString(gid);
        status.structValue["status"] = XMLRPCValue::fromString("active");
        status.structValue["totalLength"] = XMLRPCValue::fromString("1048576");
        status.structValue["completedLength"] = XMLRPCValue::fromString("524288");
        status.structValue["downloadSpeed"] = XMLRPCValue::fromString("102400");

        return XMLRPCResponse::success(status);
    });

    // aria2.getGlobalStat
    registerMethod("aria2.getGlobalStat", [](const std::vector<XMLRPCValue>& params) {
        // TODO: 实际获取全局统计
        XMLRPCValue stats = XMLRPCValue::Struct;
        stats.structValue["numActive"] = XMLRPCValue::fromString("3");
        stats.structValue["numWaiting"] = XMLRPCValue::fromString("5");
        stats.structValue["globalDownloadSpeed"] = XMLRPCValue::fromString("1024000");
        stats.structValue["globalUploadSpeed"] = XMLRPCValue::fromString("512000");

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

} // namespace rpc
} // namespace daemon
} // namespace falcon
