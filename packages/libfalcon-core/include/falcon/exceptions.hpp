#pragma once

#include <stdexcept>
#include <string>

namespace falcon {

/// Base exception class
class FalconException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Invalid URL format
class InvalidURLException : public FalconException {
public:
    InvalidURLException(const std::string& url)
        : FalconException("Invalid URL: " + url) {}
};

/// Unsupported protocol
class UnsupportedProtocolException : public FalconException {
public:
    UnsupportedProtocolException(const std::string& protocol)
        : FalconException("Unsupported protocol: " + protocol) {}
};

/// Network error
class NetworkException : public FalconException {
public:
    using FalconException::FalconException;
};

/// File IO error
class FileIOException : public FalconException {
public:
    using FalconException::FalconException;
};

/// Task cancelled
class TaskCancelledException : public FalconException {
public:
    TaskCancelledException()
        : FalconException("Task was cancelled") {}
};

/// Download failed
class DownloadFailedException : public FalconException {
public:
    using FalconException::FalconException;
};

} // namespace falcon
