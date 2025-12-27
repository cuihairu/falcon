// FTP Handler Unit Tests

#include <falcon/plugin_manager.hpp>

#include <gtest/gtest.h>

#include <algorithm>

TEST(FtpHandlerTest, PluginManagerLoadsFtpHandler) {
    falcon::PluginManager pm;
    pm.loadAllPlugins();

    auto schemes = pm.listSupportedSchemes();
    // If FTP plugin is enabled in this build, "ftp" should be listed.
    // (This test is compiled only when FALCON_ENABLE_FTP_PLUGIN is defined.)
    EXPECT_NE(std::find(schemes.begin(), schemes.end(), "ftp"), schemes.end());
}
