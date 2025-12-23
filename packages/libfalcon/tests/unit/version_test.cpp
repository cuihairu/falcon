// Falcon Version Unit Tests

#include <falcon/version.hpp>

#include <gtest/gtest.h>

#include <string>

TEST(VersionTest, ToString) {
    falcon::Version v{1, 2, 3};
    EXPECT_EQ(v.to_string(), "1.2.3");
}

TEST(VersionTest, FullStringContainsVersion) {
    std::string s = falcon::FALCON_VERSION.to_full_string();
    EXPECT_NE(s.find("Falcon"), std::string::npos);
    EXPECT_NE(s.find(falcon::FALCON_VERSION.to_string()), std::string::npos);
}

TEST(VersionTest, LegacyFunctionsReturnNonEmptyStrings) {
    const char* version = falcon::get_version_string();
    ASSERT_NE(version, nullptr);
    EXPECT_FALSE(std::string(version).empty());

    const char* ts = falcon::get_build_timestamp();
    ASSERT_NE(ts, nullptr);
    EXPECT_FALSE(std::string(ts).empty());
}

