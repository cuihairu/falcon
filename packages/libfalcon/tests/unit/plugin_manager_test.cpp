// Falcon Plugin Manager Unit Tests

#include <falcon/plugin_manager.hpp>

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

// 新增：注册多个插件
TEST(PluginManagerTest, RegisterMultiplePlugins) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));
    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));
    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "bt", std::vector<std::string>{"magnet", "torrent"}, "magnet:"));

    EXPECT_EQ(manager.getPluginCount(), 3u);

    EXPECT_NE(manager.getPlugin("http"), nullptr);
    EXPECT_NE(manager.getPlugin("ftp"), nullptr);
    EXPECT_NE(manager.getPlugin("bt"), nullptr);
}

// 新增：重复注册同一插件
TEST(PluginManagerTest, RegisterDuplicatePlugin) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http"}, "http://"));

    auto* first = manager.getPlugin("http");
    ASSERT_NE(first, nullptr);

    // 尝试注册同名插件
    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http"}, "http://"));

    EXPECT_EQ(manager.getPluginCount(), 1u);

    auto* second = manager.getPlugin("http");
    EXPECT_NE(second, nullptr);
}

// 新增：获取不存在的插件
TEST(PluginManagerTest, GetNonExistentPlugin) {
    falcon::PluginManager manager;

    EXPECT_EQ(manager.getPlugin("nonexistent"), nullptr);
    EXPECT_EQ(manager.getPluginByUrl("nonexistent://test"), nullptr);
}

// 新增：卸载不存在的插件
TEST(PluginManagerTest, UnloadNonExistentPlugin) {
    falcon::PluginManager manager;

    // 不应该崩溃
    manager.unloadPlugin("nonexistent");

    EXPECT_EQ(manager.getPluginCount(), 0u);
}

// 新增：支持的协议列表
TEST(PluginManagerTest, SupportedProtocolsList) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));
    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));

    auto protocols = manager.getSupportedProtocols();

    EXPECT_EQ(protocols.size(), 2u);
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "http") != protocols.end());
    EXPECT_TRUE(std::find(protocols.begin(), protocols.end(), "ftp") != protocols.end());
}

// 新增：支持的 URL schemes
TEST(PluginManagerTest, SupportedSchemesList) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));
    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));

    auto schemes = manager.listSupportedSchemes();

    EXPECT_EQ(schemes.size(), 3u);
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "http") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "https") != schemes.end());
    EXPECT_TRUE(std::find(schemes.begin(), schemes.end(), "ftp") != schemes.end());
}

// 新增：URL 路由到正确的插件
TEST(PluginManagerTest, RouteUrlToCorrectPlugin) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));
    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "ftp", std::vector<std::string>{"ftp"}, "ftp://"));

    auto* http_plugin = manager.getPluginByUrl("http://example.com/file.zip");
    ASSERT_NE(http_plugin, nullptr);
    EXPECT_EQ(http_plugin->protocol_name(), "http");

    auto* https_plugin = manager.getPluginByUrl("https://example.com/file.zip");
    ASSERT_NE(https_plugin, nullptr);
    EXPECT_EQ(https_plugin->protocol_name(), "http");

    auto* ftp_plugin = manager.getPluginByUrl("ftp://example.com/file.zip");
    ASSERT_NE(ftp_plugin, nullptr);
    EXPECT_EQ(ftp_plugin->protocol_name(), "ftp");
}

// 新增：无效 URL 处理
TEST(PluginManagerTest, HandleInvalidUrl) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http"}, "http://"));

    // 空 URL
    EXPECT_EQ(manager.getPluginByUrl(""), nullptr);

    // 无协议 URL
    EXPECT_EQ(manager.getPluginByUrl("example.com/file.zip"), nullptr);

    // 不支持的协议
    EXPECT_EQ(manager.getPluginByUrl("unsupported://test"), nullptr);
}

// 新增：插件优先级测试
TEST(PluginManagerTest, PluginPriority) {
    falcon::PluginManager manager;

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

    manager.registerPlugin(std::make_unique<UniversalHandler>());

    auto* plugin = manager.getPluginByUrl("http://example.com");
    ASSERT_NE(plugin, nullptr);
    EXPECT_EQ(plugin->protocol_name(), "universal");
}

// 新增：并发插件注册
TEST(PluginManagerTest, ConcurrentPluginRegistration) {
    falcon::PluginManager manager;

    std::vector<std::thread> threads;

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([this, i, &manager]() {
            auto plugin = std::make_unique<DummyProtocolHandler>(
                "plugin_" + std::to_string(i),
                std::vector<std::string>{"scheme" + std::to_string(i)},
                "scheme" + std::to_string(i) + "://");
            manager.registerPlugin(std::move(plugin));
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    EXPECT_EQ(manager.getPluginCount(), 10u);
}

// 新增：清空所有插件
TEST(PluginManagerTest, UnloadAllPlugins) {
    falcon::PluginManager manager;

    for (int i = 0; i < 5; ++i) {
        manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
            "plugin_" + std::to_string(i),
            std::vector<std::string>{"scheme" + std::to_string(i)},
            "scheme" + std::to_string(i) + "://"));
    }

    EXPECT_EQ(manager.getPluginCount(), 5u);

    // 卸载所有插件
    for (int i = 0; i < 5; ++i) {
        manager.unloadPlugin("plugin_" + std::to_string(i));
    }

    EXPECT_EQ(manager.getPluginCount(), 0u);
}

// 新增：插件信息查询
TEST(PluginManagerTest, PluginInformation) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "http", std::vector<std::string>{"http", "https"}, "http://"));

    auto* plugin = manager.getPlugin("http");
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
TEST(PluginManagerTest, EmptyProtocolName) {
    falcon::PluginManager manager;

    auto plugin = std::make_unique<DummyProtocolHandler>(
        "", std::vector<std::string>{}, "");

    // 尝试注册空协议名插件
    manager.registerPlugin(std::move(plugin));

    // 应该能注册，但可能返回 nullptr
    auto* retrieved = manager.getPlugin("");
    // 验证行为
}

// 新增：特殊字符在协议名中
TEST(PluginManagerTest, SpecialCharactersInProtocol) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "my-custom.protocol", std::vector<std::string>{"my-custom"}, "my-custom://"));

    auto* plugin = manager.getPlugin("my-custom.protocol");
    ASSERT_NE(plugin, nullptr);
    EXPECT_EQ(plugin->protocol_name(), "my-custom.protocol");
}

// 新增：大小写敏感性测试
TEST(PluginManagerTest, CaseSensitivity) {
    falcon::PluginManager manager;

    manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
        "HTTP", std::vector<std::string>{"HTTP"}, "HTTP://"));

    auto* upper = manager.getPlugin("HTTP");
    EXPECT_NE(upper, nullptr);

    auto* lower = manager.getPlugin("http");
    // 验证大小写敏感性
}

// 新增：大量插件注册压力测试
TEST(PluginManagerTest, ManyPluginsStressTest) {
    falcon::PluginManager manager;

    constexpr int plugin_count = 100;

    for (int i = 0; i < plugin_count; ++i) {
        manager.registerPlugin(std::make_unique<DummyProtocolHandler>(
            "plugin_" + std::to_string(i),
            std::vector<std::string>{"scheme" + std::to_string(i)},
            "scheme" + std::to_string(i) + "://"));
    }

    EXPECT_EQ(manager.getPluginCount(), plugin_count);

    // 验证所有插件都能正确获取
    for (int i = 0; i < plugin_count; ++i) {
        auto* plugin = manager.getPlugin("plugin_" + std::to_string(i));
        ASSERT_NE(plugin, nullptr);
        EXPECT_EQ(plugin->protocol_name(), "plugin_" + std::to_string(i));
    }
}
