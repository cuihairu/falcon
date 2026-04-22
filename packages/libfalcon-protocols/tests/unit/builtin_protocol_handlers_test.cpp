// Test that register_builtin_protocol_handlers() registers the correct
// protocol handlers when falcon_protocols is linked.

#include <falcon/protocol_registry.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

TEST(BuiltinProtocolHandlersTest, LoadBuiltinHandlersRegistersHttpWhenEnabled) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::ProtocolRegistry registry;
    registry.load_builtin_handlers();

    EXPECT_NE(registry.get_handler("http"), nullptr);

    auto protocols = registry.supported_protocols();
    EXPECT_NE(std::find(protocols.begin(), protocols.end(), "http"),
              protocols.end());
#else
    GTEST_SKIP() << "HTTP handler not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, LoadBuiltinHandlersRegistersFtpWhenEnabled) {
#ifdef FALCON_ENABLE_FTP_PLUGIN
    falcon::ProtocolRegistry registry;
    registry.load_builtin_handlers();

    EXPECT_NE(registry.get_handler("ftp"), nullptr);

    auto protocols = registry.supported_protocols();
    EXPECT_NE(std::find(protocols.begin(), protocols.end(), "ftp"),
              protocols.end());
#else
    GTEST_SKIP() << "FTP handler not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, SupportedSchemesIncludeHttpHttps) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::ProtocolRegistry registry;
    registry.load_builtin_handlers();

    auto schemes = registry.supported_schemes();
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "http"), schemes.end());
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "https"),
              schemes.end());
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, CanHandleHttpUrl) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::ProtocolRegistry manager;
    manager.load_builtin_handlers();

    EXPECT_TRUE(manager.supportsUrl("http://example.com/file.zip"));
    EXPECT_TRUE(manager.supportsUrl("https://example.com/file.zip"));
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}

TEST(BuiltinProtocolHandlersTest, GetPluginByUrlRoutesCorrectly) {
#if defined(FALCON_ENABLE_HTTP) || defined(FALCON_ENABLE_HTTP_PLUGIN)
    falcon::ProtocolRegistry manager;
    manager.load_builtin_handlers();

    auto* handler = manager.get_handlerByUrl("http://example.com/file.zip");
    ASSERT_NE(handler, nullptr);
    EXPECT_EQ(handler->protocol_name(), "http");

    auto* https_handler =
        manager.get_handlerByUrl("https://example.com/file.zip");
    ASSERT_NE(https_handler, nullptr);
    EXPECT_EQ(https_handler->protocol_name(), "http");
#else
    GTEST_SKIP() << "HTTP plugin not enabled";
#endif
}
