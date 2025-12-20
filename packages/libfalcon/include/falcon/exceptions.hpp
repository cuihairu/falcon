#pragma once

#include <stdexcept>
#include <string>

namespace falcon {

/// 基础异常类
class FalconException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// 无效的 URL 格式
class InvalidURLException : public FalconException {
public:
    InvalidURLException(const std::string& url)
        : FalconException("Invalid URL: " + url) {}
};

/// 不支持的协议
class UnsupportedProtocolException : public FalconException {
public:
    UnsupportedProtocolException(const std::string& protocol)
        : FalconException("Unsupported protocol: " + protocol) {}
};

/// 网络错误
class NetworkException : public FalconException {
public:
    using FalconException::FalconException;
};

/// 文件 IO 错误
class FileIOException : public FalconException {
public:
    using FalconException::FalconException;
};

/// 任务已取消
class TaskCancelledException : public FalconException {
public:
    TaskCancelledException()
        : FalconException("Task was cancelled") {}
};

/// 下载失败
class DownloadFailedException : public FalconException {
public:
    using FalconException::FalconException;
};

} // namespace falcon
