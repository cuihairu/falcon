#pragma once

#include <falcon/protocol_handler.hpp>

#include <memory>

namespace falcon {
namespace plugins {

/// FTP/FTPS protocol handler using libcurl
class FtpHandler : public IProtocolHandler {
public:
    FtpHandler();
    ~FtpHandler() override;

    FtpHandler(const FtpHandler&) = delete;
    FtpHandler& operator=(const FtpHandler&) = delete;

    [[nodiscard]] std::string protocol_name() const override { return "ftp"; }

    [[nodiscard]] std::vector<std::string> supported_schemes() const override {
        return {"ftp", "ftps"};
    }

    [[nodiscard]] bool can_handle(const std::string& url) const override;

    [[nodiscard]] FileInfo get_file_info(const std::string& url,
                                         const DownloadOptions& options) override;

    void download(DownloadTask::Ptr task, IEventListener* listener) override;

    void pause(DownloadTask::Ptr task) override;

    void resume(DownloadTask::Ptr task, IEventListener* listener) override;

    void cancel(DownloadTask::Ptr task) override;

    [[nodiscard]] bool supports_resume() const override { return true; }

    [[nodiscard]] int priority() const override { return 50; }
};

/// Factory function to create FTP handler
std::unique_ptr<IProtocolHandler> create_ftp_handler();

}  // namespace plugins
}  // namespace falcon

