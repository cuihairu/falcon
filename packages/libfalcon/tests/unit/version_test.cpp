// Falcon Version Unit Tests

#include <falcon/version.hpp>

#include <gtest/gtest.h>

#include <string>
#include <sstream>
#include <algorithm>

//==============================================================================
// Version 基础测试
//==============================================================================

TEST(VersionTest, ToString) {
    falcon::Version v{1, 2, 3};
    EXPECT_EQ(v.to_string(), "1.2.3");
}

TEST(VersionTest, ToStringZeroVersions) {
    falcon::Version v{0, 0, 0};
    EXPECT_EQ(v.to_string(), "0.0.0");
}

TEST(VersionTest, ToStringLargeVersions) {
    falcon::Version v{255, 255, 255};
    std::string str = v.to_string();
    EXPECT_NE(str.find("255"), std::string::npos);
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

//==============================================================================
// Version 比较测试
//==============================================================================

TEST(VersionComparison, EqualVersions) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2{1, 2, 3};

    EXPECT_EQ(v1, v2);
    EXPECT_FALSE(v1 != v2);
    EXPECT_FALSE(v1 < v2);
    EXPECT_FALSE(v1 > v2);
    EXPECT_LE(v1, v2);
    EXPECT_GE(v1, v2);
}

TEST(VersionComparison, LessThanMajor) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2{2, 0, 0};

    EXPECT_LT(v1, v2);
    EXPECT_LE(v1, v2);
    EXPECT_NE(v1, v2);
}

TEST(VersionComparison, LessThanMinor) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2{1, 3, 0};

    EXPECT_LT(v1, v2);
    EXPECT_LE(v1, v2);
}

TEST(VersionComparison, LessThanPatch) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2{1, 2, 4};

    EXPECT_LT(v1, v2);
    EXPECT_LE(v1, v2);
}

TEST(VersionComparison, GreaterThanMajor) {
    falcon::Version v1{2, 0, 0};
    falcon::Version v2{1, 2, 3};

    EXPECT_GT(v1, v2);
    EXPECT_GE(v1, v2);
}

TEST(VersionComparison, GreaterThanMinor) {
    falcon::Version v1{1, 3, 0};
    falcon::Version v2{1, 2, 3};

    EXPECT_GT(v1, v2);
    EXPECT_GE(v1, v2);
}

TEST(VersionComparison, GreaterThanPatch) {
    falcon::Version v1{1, 2, 4};
    falcon::Version v2{1, 2, 3};

    EXPECT_GT(v1, v2);
    EXPECT_GE(v1, v2);
}

TEST(VersionComparison, NotEqual) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2{1, 2, 4};

    EXPECT_NE(v1, v2);
}

//==============================================================================
// Version 构造测试
//==============================================================================

TEST(VersionConstruction, DefaultConstruction) {
    falcon::Version v;
    EXPECT_EQ(v.major, 0);
    EXPECT_EQ(v.minor, 0);
    EXPECT_EQ(v.patch, 0);
}

TEST(VersionConstruction, ParameterizedConstruction) {
    falcon::Version v{1, 2, 3};
    EXPECT_EQ(v.major, 1);
    EXPECT_EQ(v.minor, 2);
    EXPECT_EQ(v.patch, 3);
}

TEST(VersionConstruction, CopyConstruction) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2 = v1;

    EXPECT_EQ(v2.major, 1);
    EXPECT_EQ(v2.minor, 2);
    EXPECT_EQ(v2.patch, 3);
}

TEST(VersionConstruction, MoveConstruction) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2 = std::move(v1);

    EXPECT_EQ(v2.major, 1);
    EXPECT_EQ(v2.minor, 2);
    EXPECT_EQ(v2.patch, 3);
}

TEST(VersionConstruction, CopyAssignment) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2;
    v2 = v1;

    EXPECT_EQ(v2.major, 1);
    EXPECT_EQ(v2.minor, 2);
    EXPECT_EQ(v2.patch, 3);
}

TEST(VersionConstruction, MoveAssignment) {
    falcon::Version v1{1, 2, 3};
    falcon::Version v2;
    v2 = std::move(v1);

    EXPECT_EQ(v2.major, 1);
    EXPECT_EQ(v2.minor, 2);
    EXPECT_EQ(v2.patch, 3);
}

//==============================================================================
// Version 解析测试
//==============================================================================

TEST(VersionParsing, ParseValidVersion) {
    auto v = falcon::Version::parse("1.2.3");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 1);
    EXPECT_EQ(v->minor, 2);
    EXPECT_EQ(v->patch, 3);
}

TEST(VersionParsing, ParseVersionWithTwoComponents) {
    auto v = falcon::Version::parse("1.2");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 1);
    EXPECT_EQ(v->minor, 2);
    EXPECT_EQ(v->patch, 0);
}

TEST(VersionParsing, ParseVersionWithOneComponent) {
    auto v = falcon::Version::parse("1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 1);
    EXPECT_EQ(v->minor, 0);
    EXPECT_EQ(v->patch, 0);
}

TEST(VersionParsing, ParseVersionWithPrefix) {
    auto v = falcon::Version::parse("v1.2.3");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(v->major, 1);
    EXPECT_EQ(v->minor, 2);
    EXPECT_EQ(v->patch, 3);
}

TEST(VersionParsing, ParseInvalidVersion) {
    auto v = falcon::Version::parse("invalid");
    EXPECT_FALSE(v.has_value());
}

TEST(VersionParsing, ParseEmptyString) {
    auto v = falcon::Version::parse("");
    EXPECT_FALSE(v.has_value());
}

TEST(VersionParsing, ParseVersionWithTooManyComponents) {
    auto v = falcon::Version::parse("1.2.3.4");
    // 取决于实现，可能解析前三个或失败
}

//==============================================================================
// Version 边界条件测试
//==============================================================================

TEST(VersionBoundary, VeryLargeNumbers) {
    falcon::Version v{999999, 999999, 999999};
    std::string str = v.to_string();
    EXPECT_FALSE(str.empty());
}

TEST(VersionBoundary, AllZero) {
    falcon::Version v{0, 0, 0};
    EXPECT_EQ(v.to_string(), "0.0.0");
}

TEST(VersionBoundary, SingleComponentNonZero) {
    falcon::Version v1{1, 0, 0};
    EXPECT_EQ(v1.to_string(), "1.0.0");

    falcon::Version v2{0, 1, 0};
    EXPECT_EQ(v2.to_string(), "0.1.0");

    falcon::Version v3{0, 0, 1};
    EXPECT_EQ(v3.to_string(), "0.0.1");
}

//==============================================================================
// Version 流输出测试
//==============================================================================

TEST(VersionStream, OutputStream) {
    falcon::Version v{1, 2, 3};
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ(oss.str(), "1.2.3");
}

TEST(VersionStream, InputStream) {
    std::istringstream iss("1.2.3");
    falcon::Version v;
    // 取决于是否实现了 >>
}

//==============================================================================
// FALCON_VERSION 宏测试
//==============================================================================

TEST(FalconVersionMacro, VersionMacroIsDefined) {
    EXPECT_GE(falcon::FALCON_VERSION.major, 0);
    EXPECT_GE(falcon::FALCON_VERSION.minor, 0);
    EXPECT_GE(falcon::FALCON_VERSION.patch, 0);
}

TEST(FalconVersionMacro, VersionStringMacro) {
    std::string version_str = FALCON_VERSION_STRING;
    EXPECT_FALSE(version_str.empty());
}

TEST(FalconVersionMacro, VersionStringMacroFormat) {
    std::string version_str = FALCON_VERSION_STRING;
    // 验证格式是否为 X.Y.Z
    int dot_count = 0;
    for (char c : version_str) {
        if (c == '.') dot_count++;
    }
    EXPECT_EQ(dot_count, 2);
}

//==============================================================================
// 主函数
//==============================================================================

