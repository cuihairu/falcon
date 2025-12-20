#pragma once

// Falcon Downloader Library - Main Header
// Include this header to use all public APIs

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/protocol_handler.hpp>
#include <falcon/types.hpp>
#include <falcon/version.hpp>

namespace falcon {

/// Convenience function to create a download engine with default settings
inline std::unique_ptr<DownloadEngine> create_engine() {
    return std::make_unique<DownloadEngine>();
}

/// Convenience function to create a download engine with custom settings
inline std::unique_ptr<DownloadEngine> create_engine(const EngineConfig& config) {
    return std::make_unique<DownloadEngine>(config);
}

}  // namespace falcon
