#pragma once

#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace falcon {

/// Protocol handler interface for implementing download protocols
class IProtocolHandler {
public:
    virtual ~IProtocolHandler() = default;

    /// Get protocol name (e.g., "http", "ftp", "magnet")
    [[nodiscard]] virtual std::string protocol_name() const = 0;

    /// Get supported URL schemes (e.g., {"http", "https"})
    [[nodiscard]] virtual std::vector<std::string> supported_schemes() const = 0;

    /// Check if this handler can handle the given URL
    /// @param url The URL to check
    /// @return true if this handler supports the URL
    [[nodiscard]] virtual bool can_handle(const std::string& url) const = 0;

    /// Get file information without downloading
    /// @param url The URL to query
    /// @param options Download options
    /// @return File information
    /// @throws NetworkException on network error
    [[nodiscard]] virtual FileInfo get_file_info(const std::string& url,
                                                  const DownloadOptions& options) = 0;

    /// Start downloading a file
    /// @param task The download task
    /// @param listener Event listener for callbacks
    /// @throws NetworkException on network error
    /// @throws FileIOException on file error
    virtual void download(DownloadTask::Ptr task, IEventListener* listener) = 0;

    /// Pause an active download
    /// @param task The download task to pause
    virtual void pause(DownloadTask::Ptr task) = 0;

    /// Resume a paused download
    /// @param task The download task to resume
    /// @param listener Event listener for callbacks
    virtual void resume(DownloadTask::Ptr task, IEventListener* listener) = 0;

    /// Cancel a download
    /// @param task The download task to cancel
    virtual void cancel(DownloadTask::Ptr task) = 0;

    /// Check if protocol supports resume
    [[nodiscard]] virtual bool supports_resume() const { return false; }

    /// Check if protocol supports segmented download
    [[nodiscard]] virtual bool supports_segments() const { return false; }

    /// Get handler priority (higher = preferred when multiple handlers match)
    [[nodiscard]] virtual int priority() const { return 0; }
};

/// Factory function type for creating protocol handlers
using ProtocolHandlerFactory = std::unique_ptr<IProtocolHandler> (*)();

}  // namespace falcon
