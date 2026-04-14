// Test that register_builtin_protocol_handlers() registers the correct
// protocol handlers when falcon_protocols is linked.

#include <falcon/plugin_manager.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

TEST(BuiltinProtocolHandlersTest, LoadAllPluginsRegistersHttpWhenEnabled) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::PluginManager manager;
    manager.loadAllPlugins();

    EXPECT_NE(manager.getPlugin("http"), nullptr);

    auto protocols = manager.getSupportedProtocols();
    EXPECT_NE(std::find(protocols.begin(), protocols.end(), "http"),
              protocols.end());
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, LoadAllPluginsRegistersFtpWhenEnabled) {
#ifdef FALCON_ENABLE_FTP_PLUGIN
    falcon::PluginManager manager;
    manager.loadAllPlugins();

    EXPECT_NE(manager.getPlugin("ftp"), nullptr);

    auto protocols = manager.getSupportedProtocols();
    EXPECT_NE(std::find(protocols.begin(), protocols.end(), "ftp"),
              protocols.end());
#else
    GTEST_SKIP() << "FTP plugin not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, SupportedSchemesIncludeHttpHttps) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::PluginManager manager;
    manager.loadAllPlugins();

    auto schemes = manager.listSupportedSchemes();
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "http"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "https"),
              schemes.end());
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, CanHandleHttpUrl) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::PluginManager manager;
    manager.loadAllPlugins();

    EXPECT_TRUE(manager.supportsUrl("http://example.com/file.zip"));
    EXPECT_TRUE(manager.supportsUrl("https://example.com/file.zip"));
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, GetPluginByUrlRoutesCorrectly) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::PluginManager manager;
    manager.loadAllPlugins();

    auto* handler = manager.getPluginByUrl("http://example.com/file.zip");
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->protocol_name(), "http");

    auto* https_handler =
        manager.getPluginByUrl("https://example.com/file.zip");
    ASSERT_NE(https_handler, nullptr);
    EXPECT_EQ(https_handler->protocol_name(), "http");
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}
