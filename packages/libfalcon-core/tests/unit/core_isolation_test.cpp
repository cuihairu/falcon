// Verify that falcon_core can operate without falcon_protocols.
// This test must only be linked against falcon_core (no protocols).

#include <falcon/download_engine.hpp>
#include <falcon/exceptions.hpp>
#include <falcon/plugin_manager.hpp>

#include <gtest/gtest.h>

TEST(CoreIsolationTest, PluginManagerWorksWithoutProtocols) {
    falcon::PluginManager manager;

    // The weak stub makes loadAllPlugins() a no-op when falcon_protocols
    // is not linked.
    manager.loadAllPlugins();

    EXPECT_EQ(manager.getPluginCount(), 0u);
    EXPECT_EQ(manager.getPlugin("http"), nullptr);
}

TEST(CoreIsolationTest, DownloadEngineCreatesWithoutProtocols) {
    falcon::DownloadEngine engine;

    // Without any protocol handler registered, add_task should throw
    // for an unsupported URL.
    EXPECT_THROW(engine.add_task("http://example.com/file.zip"),
                 falcon::UnsupportedProtocolException);
}

TEST(CoreIsolationTest, PluginManagerSupportsManualRegistration) {
    falcon::PluginManager manager;

    class DummyHandler final : public falcon::IProtocolHandler {
    public:
        [[nodiscard]] std::string protocol_name() const override {
            return "dummy";
        }
        [[nodiscard]] std::vector<std::string> supported_schemes()
            const override {
            return {"dummy"};
        }
        [[nodiscard]] bool can_handle(const std::string& url) const override {
            return url.rfind("dummy://", 0) == 0;
        }
        [[nodiscard]] falcon::FileInfo get_file_info(
            const std::string& url,
            const falcon::DownloadOptions&) override {
            falcon::FileInfo info;
            info.url = url;
            return info;
        }
        void download(falcon::DownloadTask::Ptr,
                      falcon::IEventListener*) override {}
        void pause(falcon::DownloadTask::Ptr) override {}
        void resume(falcon::DownloadTask::Ptr,
                    falcon::IEventListener*) override {}
        void cancel(falcon::DownloadTask::Ptr) override {}
    };

    manager.registerPlugin(std::make_unique<DummyHandler>());
    EXPECT_EQ(manager.getPluginCount(), 1u);
    EXPECT_NE(manager.getPlugin("dummy"), nullptr);
    EXPECT_TRUE(manager.supportsUrl("dummy://test"));
}
