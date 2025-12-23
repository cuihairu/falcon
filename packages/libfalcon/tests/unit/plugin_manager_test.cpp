// Falcon Plugin Manager Unit Tests

#include <falcon/plugin_manager.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace {

class DummyProtocolHandler final : public falcon::IProtocolHandler {
public:
    DummyProtocolHandler(std::string protocol,
                         std::vector<std::string> schemes,
                         std::string url_prefix)
        : protocol_(std::move(protocol))
        , schemes_(std::move(schemes))
        , url_prefix_(std::move(url_prefix)) {}

    [[nodiscard]] std::string protocol_name() const override { return protocol_; }
    [[nodiscard]] std::vector<std::string> supported_schemes() const override { return schemes_; }

    [[nodiscard]] bool can_handle(const std::string& url) const override {
        return !url_prefix_.empty() && url.rfind(url_prefix_, 0) == 0;
    }

    [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                 const falcon::DownloadOptions&) override {
        falcon::FileInfo info;
        info.url = url;
        info.filename = "dummy";
        info.total_size = 0;
        info.supports_resume = false;
        info.content_type = "application/octet-stream";
        return info;
    }

    void download(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void pause(falcon::DownloadTask::Ptr) override {}
    void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
    void cancel(falcon::DownloadTask::Ptr) override {}

private:
    std::string protocol_;
    std::vector<std::string> schemes_;
    std::string url_prefix_;
};

} // namespace

TEST(PluginManagerTest, RegisterGetUnload) {
    falcon::PluginManager manager;

    EXPECT_EQ(manager.getPluginCount(), 0u);
    EXPECT_EQ(manager.getPlugin("foo"), nullptr);

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "foo", std::vector<std::string>{"foo"}, "foo://"));

    EXPECT_EQ(manager.getPluginCount(), 1u);
    EXPECT_NE(manager.getPlugin("foo"), nullptr);
    EXPECT_EQ(manager.getSupportedProtocols(), (std::vector<std::string>{"foo"}));
    EXPECT_EQ(manager.listSupportedSchemes(), (std::vector<std::string>{"foo"}));

    manager.unloadPlugin("foo");
    EXPECT_EQ(manager.getPluginCount(), 0u);
    EXPECT_EQ(manager.getPlugin("foo"), nullptr);
}

TEST(PluginManagerTest, GetPluginByUrlPrefersHlsForM3u8) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http"));

    class HlsOnlyHandler final : public falcon::IProtocolHandler {
    public:
        [[nodiscard]] std::string protocol_name() const override { return "hls"; }
        [[nodiscard]] std::vector<std::string> supported_schemes() const override { return {"hls"}; }

        [[nodiscard]] bool can_handle(const std::string& url) const override {
            if (url.rfind("http", 0) != 0) return false;
            return url.find(".m3u8") != std::string::npos || url.find(".mpd") != std::string::npos;
        }

        [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                     const falcon::DownloadOptions&) override {
            falcon::FileInfo info;
            info.url = url;
            info.filename = "playlist";
            info.total_size = 0;
            info.supports_resume = false;
            info.content_type = "application/vnd.apple.mpegurl";
            return info;
        }

        void download(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
        void pause(falcon::DownloadTask::Ptr) override {}
        void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
        void cancel(falcon::DownloadTask::Ptr) override {}
    };

    manager.registerPlugin(std::make_unique<HlsOnlyHandler>());

    auto* chosen = manager.getPluginByUrl("https://example.com/stream.m3u8");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->protocol_name(), "hls");

    auto* normal = manager.getPluginByUrl("https://example.com/index.html");
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->protocol_name(), "http");
}

TEST(PluginManagerTest, GetPluginByUrlFallsBackToCanHandle) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "custom", std::vector<std::string>{"custom"}, "custom:"));

    auto* chosen = manager.getPluginByUrl("custom:opaque");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->protocol_name(), "custom");
}
