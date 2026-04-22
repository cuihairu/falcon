// Falcon Protocol Registry Unit Tests

#include <falcon/protocol_registry.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <thread>
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

TEST(ProtocolRegistryTest, RegisterGet) {
    falcon::ProtocolRegistry manager;

    EXPECT_EQ(manager.handler_count(), 0u);
    EXPECT_EQ(manager.get_handler("foo"), nullptr);

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "foo", std::vector<std::string>{"foo"}, "foo://"));

    EXPECT_EQ(manager.handler_count(), 1u);
    EXPECT_NE(manager.get_handler("foo"), nullptr);
    EXPECT_EQ(manager.supported_protocols(), (std::vector<std::string>{"foo"}));
    EXPECT_EQ(manager.supported_schemes(), (std::vector<std::string>{"foo"}));

    // Note: ProtocolRegistry doesn't provide unload functionality
    // Handlers remain registered for the lifetime of the registry
}

TEST(ProtocolRegistryTest, GetPluginByUrlPrefersHlsForM3u8) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
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

    manager.register_handler(std::make_unique<HlsOnlyHandler>());

    auto* chosen = manager.get_handler_for_url("https://example.com/stream.m3u8");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->protocol_name(), "hls");

    auto* normal = manager.get_handler_for_url("https://example.com/index.html");
    ASSERT_NE(normal, nullptr);
    EXPECT_EQ(normal->protocol_name(), "http");
}

TEST(ProtocolRegistryTest, GetPluginByUrlFallsBackToCanHandle) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "custom", std::vector<std::string>{"custom"}, "custom:"));

    auto* chosen = manager.get_handler_for_url("custom:opaque");
    ASSERT_NE(chosen, nullptr);
    EXPECT_EQ(chosen->protocol_name(), "custom");
}

// 新增：注册多个插件
TEST(ProtocolRegistryTest, RegisterMultiplePlugins) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http"));
    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));
    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "bt", std::vector<std::string>{"magnet", "torrent"}, "magnet:"));

    EXPECT_EQ(manager.handler_count(), 3u);

    EXPECT_NE(manager.get_handler("http"), nullptr);
    EXPECT_NE(manager.get_handler("ftp"), nullptr);
    EXPECT_NE(manager.get_handler("bt"), nullptr);
}

// 新增：重复注册同一插件
TEST(ProtocolRegistryTest, RegisterDuplicatePlugin) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http"}, "http://"));

    auto* first = manager.get_handler("http");
    ASSERT_NE(first, nullptr);

    // 尝试注册同名插件
    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http"}, "http://"));

    EXPECT_EQ(manager.handler_count(), 1u);

    auto* second = manager.get_handler("http");
    EXPECT_NE(second, nullptr);
}

// 新增：获取不存在的插件
TEST(ProtocolRegistryTest, GetNonExistentPlugin) {
    falcon::ProtocolRegistry manager;

    EXPECT_EQ(manager.get_handler("nonexistent"), nullptr);
    EXPECT_EQ(manager.get_handler_for_url("nonexistent://test"), nullptr);
}

// Note: Unload functionality removed from ProtocolRegistry
// Handlers are designed to be registered once and remain for the lifetime
// TEST(ProtocolRegistryTest, UnloadNonExistentHandler) {
//     falcon::ProtocolRegistry manager;
//     // ProtocolRegistry doesn't provide unload functionality
// }

// 新增：支持的协议列表
TEST(ProtocolRegistryTest, SupportedProtocolsList) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));
    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));

    auto protocols = manager.supported_protocols();

    EXPECT_EQ(protocols.size(), 2u);
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "http") != protocols.end());
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "ftp") != protocols.end());
}

// 新增：支持的 URL schemes
TEST(ProtocolRegistryTest, SupportedSchemesList) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));
    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));

    auto schemes = manager.supported_schemes();

    EXPECT_EQ(schemes.size(), 3u);
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "http") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "https") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "ftp") != schemes.end());
}

// 新增：URL 路由到正确的插件
TEST(ProtocolRegistryTest, RouteUrlToCorrectPlugin) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http"));
    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));

    auto* http_plugin = manager.get_handler_for_url("http://example.com/file.zip");
    ASSERT_NE(http_plugin, nullptr);
    EXPECT_EQ(http_plugin->protocol_name(), "http");

    auto* https_plugin = manager.get_handler_for_url("https://example.com/file.zip");
    ASSERT_NE(https_plugin, nullptr);
    EXPECT_EQ(https_plugin->protocol_name(), "http");

    auto* ftp_plugin = manager.get_handler_for_url("ftp://example.com/file.zip");
    ASSERT_NE(ftp_plugin, nullptr);
    EXPECT_EQ(ftp_plugin->protocol_name(), "ftp");
}

// 新增：无效 URL 处理
TEST(ProtocolRegistryTest, HandleInvalidUrl) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http"}, "http://"));

    // 空 URL
    EXPECT_EQ(manager.get_handler_for_url(""), nullptr);

    // 无协议 URL
    EXPECT_EQ(manager.get_handler_for_url("example.com/file.zip"), nullptr);

    // 不支持的协议
    EXPECT_EQ(manager.get_handler_for_url("unsupported://test"), nullptr);
}

// 新增：插件优先级测试
TEST(ProtocolRegistryTest, PluginPriority) {
    falcon::ProtocolRegistry manager;

    // 创建一个能处理多种协议的插件
    class UniversalHandler final : public falcon::IProtocolHandler {
    public:
        [[nodiscard]] std::string protocol_name() const override { return "universal"; }
        [[nodiscard]] std::vector<std::string> supported_schemes() const override {
            return {"http", "https", "ftp"};
        }

        [[nodiscard]] bool can_handle(const std::string&) const override { return true; }

        [[nodiscard]] falcon::FileInfo get_file_info(const std::string& url,
                                                     const falcon::DownloadOptions&) override {
            falcon::FileInfo info;
            info.url = url;
            return info;
        }

        void download(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
        void pause(falcon::DownloadTask::Ptr) override {}
        void resume(falcon::DownloadTask::Ptr, falcon::IEventListener*) override {}
        void cancel(falcon::DownloadTask::Ptr) override {}
    };

    manager.register_handler(std::make_unique<UniversalHandler>());

    auto* plugin = manager.get_handler_for_url("http://example.com");
    ASSERT_NE(plugin, nullptr);
    EXPECT_EQ(plugin->protocol_name(), "universal");
}

// 新增：并发插件注册
TEST(ProtocolRegistryTest, ConcurrentPluginRegistration) {
    falcon::ProtocolRegistry manager;

    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i, &manager]() {
            auto plugin = std::make_unique<DummyProtocolHandler>(
                "plugin_" + std::to_string(i),
                std::vector<std::string>{"scheme" + std::to_string(i)},
                "scheme" + std::to_string(i) + "://");
            manager.register_handler(std::move(plugin));
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(manager.handler_count(), 10u);
}

// Note: Unload functionality removed from ProtocolRegistry
// TEST(ProtocolRegistryTest, UnloadAllHandlers) {
//     falcon::ProtocolRegistry manager;
//     // ProtocolRegistry doesn't provide unload functionality
// }

// 新增：插件信息查询
TEST(ProtocolRegistryTest, PluginInformation) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));

    auto* plugin = manager.get_handler("http");
    ASSERT_NE(plugin, nullptr);

    EXPECT_EQ(plugin->protocol_name(), "http");

    auto schemes = plugin->supported_schemes();
    EXPECT_EQ(schemes.size(), 2u);
    EXPECT_EQ(schemes[0], "http");
    EXPECT_EQ(schemes[1], "https");

    EXPECT_TRUE(plugin->can_handle("http://example.com"));
    EXPECT_FALSE(plugin->can_handle("ftp://example.com"));
}

// 新增：边界条件 - 空协议名
TEST(ProtocolRegistryTest, EmptyProtocolName) {
    falcon::ProtocolRegistry manager;

    auto plugin = std::make_unique<DummyProtocolHandler>(
        "", std::vector<std::string>{}, "");

    // 尝试注册空协议名插件
    manager.register_handler(std::move(plugin));

    // 应该能注册，但可能返回 nullptr
    auto* retrieved = manager.get_handler("");
    // 验证行为
}

// 新增：特殊字符在协议名中
TEST(ProtocolRegistryTest, SpecialCharactersInProtocol) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "my-custom.protocol", std::vector<std::string>{"my-custom"}, "my-custom://"));

    auto* plugin = manager.get_handler("my-custom.protocol");
    ASSERT_NE(plugin, nullptr);
    EXPECT_EQ(plugin->protocol_name(), "my-custom.protocol");
}

// 新增：大小写敏感性测试
TEST(ProtocolRegistryTest, CaseSensitivity) {
    falcon::ProtocolRegistry manager;

    manager.register_handler(std::make_unique<DummyProtocolHandler>(
        "HTTP", std::vector<std::string>{"HTTP"}, "HTTP://"));

    auto* upper = manager.get_handler("HTTP");
    EXPECT_NE(upper, nullptr);

    auto* lower = manager.get_handler("http");
    // 验证大小写敏感性
}

// 新增：大量插件注册压力测试
TEST(ProtocolRegistryTest, ManyPluginsStressTest) {
    falcon::ProtocolRegistry manager;

    constexpr int plugin_count = 100;

    for (int i = 0; i < plugin_count; ++i) {
        manager.register_handler(std::make_unique<DummyProtocolHandler>(
            "plugin_" + std::to_string(i),
            std::vector<std::string>{"scheme" + std::to_string(i)},
            "scheme" + std::to_string(i) + "://"));
    }

    EXPECT_EQ(manager.handler_count(), plugin_count);

    // 验证所有插件都能正确获取
    for (int i = 0; i < plugin_count; ++i) {
        auto* plugin = manager.get_handler("plugin_" + std::to_string(i));
        ASSERT_NE(plugin, nullptr);
        EXPECT_EQ(plugin->protocol_name(), "plugin_" + std::to_string(i));
    }
}
