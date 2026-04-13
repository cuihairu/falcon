#pragma once

#include <falcon/protocol_handler.hpp>

#include <memory>

namespace falcon {
namespace plugins {

/// HTTP/HTTPS protocol handler using libcurl
class HttpHandler : public IProtocolHandler {
public:
    HttpHandler();
    ~HttpHandler() override;

    // Non-copyable
    HttpHandler(const HttpHandler&) = delete;
    HttpHandler& operator=(const HttpHandler&) = delete;

    [[nodiscard]] std::string protocol_name() const override { return "http"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {"http", "https"};
    }

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                          const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] bool supports_segments() const override { return true; }

    [[nodiscard]] int priority() const override { return 100; }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Factory function to create HTTP handler
std::unique_ptr<IProtocolHandler> create_http_handler();

}  // namespace plugins
}  // namespace falcon
