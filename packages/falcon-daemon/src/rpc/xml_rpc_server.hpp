/**
 * @file xml_rpc_server.hpp
 * @brief XML-RPC 服务器实现
 * @author Falcon Team
 * @date 2025-12-27
 */

#pragma once

#include <string>
#include <map>
#include <functional>
#include <memory>
#include <mutex>

namespace falcon {

// 前向声明
class DownloadEngine;

namespace daemon {
namespace rpc {

// 前向声明
class HTTPServerThread;

// 前向声明
class HTTPServerThread;

/**
 * @struct XMLRPCValue
 * @brief XML-RPC 值类型
 */
struct XMLRPCValue {
    enum Type { String, Integer, Boolean, Double, Array, Struct, Nil };
    Type type;

    std::string stringValue;
    int intValue;
    bool boolValue;
    double doubleValue;
    std::vector<XMLRPCValue> arrayValue;
    std::map<std::string, XMLRPCValue> structValue;

    XMLRPCValue() : type(Nil), intValue(0), boolValue(false), doubleValue(0.0) {}

    static XMLRPCValue fromString(const std::string& v) {
        XMLRPCValue val;
        val.type = String;
        val.stringValue = v;
        return val;
    }

    static XMLRPCValue fromInt(int v) {
        XMLRPCValue val;
        val.type = Integer;
        val.intValue = v;
        return val;
    }

    static XMLRPCValue fromBool(bool v) {
        XMLRPCValue val;
        val.type = Boolean;
        val.boolValue = v;
        return val;
    }

    static XMLRPCValue fromDouble(double v) {
        XMLRPCValue val;
        val.type = Double;
        val.doubleValue = v;
        return val;
    }
};

/**
 * @struct XMLRPCRequest
 * @brief XML-RPC 请求
 */
struct XMLRPCRequest {
    std::string methodName;
    std::vector<XMLRPCValue> params;
    std::string id;  // 请求 ID
};

/**
 * @struct XMLRPCResponse
 * @brief XML-RPC 响应
 */
struct XMLRPCResponse {
    XMLRPCValue result;
    std::string faultString;
    int faultCode;
    bool isFault;

    XMLRPCResponse() : faultCode(0), isFault(false) {}

    static XMLRPCResponse success(const XMLRPCValue& result) {
        XMLRPCResponse response;
        response.result = result;
        response.isFault = false;
        return response;
    }

    static XMLRPCResponse fault(int code, const std::string& message) {
        XMLRPCResponse response;
        response.faultCode = code;
        response.faultString = message;
        response.isFault = true;
        return response;
    }
};

/**
 * @typedef RPCMethod
 * @brief RPC 方法处理器
 */
using RPCMethod = std::function<XMLRPCResponse(const std::vector<XMLRPCValue>&)>;

/**
 * @class XMLRPCServer
 * @brief XML-RPC 服务器
 *
 * 实现 XML-RPC 协议，与 aria2 RPC 接口兼容
 */
class XMLRPCServer {
public:
    /**
     * @brief 构造函数
     */
    XMLRPCServer();

    /**
     * @brief 析构函数
     */
    ~XMLRPCServer();

    /**
     * @brief 启动服务器
     */
    bool start(int port);

    /**
     * @brief 停止服务器
     */
    void stop();

    /**
     * @brief 注册 RPC 方法
     */
    void registerMethod(const std::string& name, RPCMethod handler);

    /**
     * @brief 处理 XML-RPC 请求
     */
    std::string handleRequest(const std::string& xmlRequest);

    /**
     * @brief 检查服务器是否运行
     */
    bool isRunning() const { return running_; }

    /**
     * @brief 设置下载引擎引用
     */
    void set_download_engine(class DownloadEngine* engine) { download_engine_ = engine; }

private:
    int port_;
    bool running_;
    std::map<std::string, RPCMethod> methods_;
    mutable std::mutex mutex_;
    std::unique_ptr<HTTPServerThread> http_server_;  // HTTP 服务器线程
    class DownloadEngine* download_engine_ = nullptr;  // 下载引擎引用

    // GID 管理
    std::unordered_map<TaskId, std::string> task_id_to_gid_;
    std::unordered_map<std::string, TaskId> gid_to_task_id_;
    TaskId next_gid_ = 1;

    /**
     * @brief 生成新的 GID
     */
    std::string generate_gid();

    /**
     * @brief TaskID 转 GID
     */
    std::string task_id_to_gid(TaskId id);

    /**
     * @brief GID 转 TaskID
     */
    TaskId gid_to_task_id(const std::string& gid);

    /**
     * @brief 解析 XML-RPC 请求
     */
    XMLRPCRequest parseRequest(const std::string& xml);

    /**
     * @brief 生成 XML-RPC 响应
     */
    std::string generateResponse(const XMLRPCResponse& response);

    /**
     * @brief 解析 XML 值
     */
    XMLRPCValue parseValue(const std::string& xml, size_t& pos);

    /**
     * @brief 生成 XML 值
     */
    std::string generateValue(const XMLRPCValue& value);

    /**
     * @brief 注册内置方法
     */
    void registerBuiltinMethods();
};

} // namespace rpc
} // namespace daemon
} // namespace falcon
