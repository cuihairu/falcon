// Unit tests for falcon::parse_size_with_suffix (download_options.hpp):
// strict byte-size parsing shared by the CLI size flags and the daemon
// addUri "min-split-size" option mapping.

#include <falcon/download_options.hpp>

#include <gtest/gtest.h>

namespace {

using falcon::parse_size_with_suffix;

TEST(ParseSizeWithSuffix, PlainDigits) {
    ASSERT_TRUE(parse_size_with_suffix("0").has_value());
    EXPECT_EQ(*parse_size_with_suffix("0"), 0ULL);
    EXPECT_EQ(*parse_size_with_suffix("100"), 100ULL);
    EXPECT_EQ(*parse_size_with_suffix("524288"), 524288ULL);
}

TEST(ParseSizeWithSuffix, BinarySuffixesCaseInsensitive) {
    EXPECT_EQ(*parse_size_with_suffix("1K"), 1024ULL);
    EXPECT_EQ(*parse_size_with_suffix("1k"), 1024ULL);
    EXPECT_EQ(*parse_size_with_suffix("512K"), 512ULL * 1024ULL);
    EXPECT_EQ(*parse_size_with_suffix("1M"), 1024ULL * 1024ULL);
    EXPECT_EQ(*parse_size_with_suffix("2g"), 2ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(*parse_size_with_suffix("1T"), 1024ULL * 1024ULL * 1024ULL * 1024ULL);
}

TEST(ParseSizeWithSuffix, RejectsGarbage) {
    EXPECT_FALSE(parse_size_with_suffix("").has_value());
    EXPECT_FALSE(parse_size_with_suffix("M").has_value());      // bare suffix
    EXPECT_FALSE(parse_size_with_suffix("abc").has_value());
    EXPECT_FALSE(parse_size_with_suffix("12abc").has_value());  // no prefix parse
    EXPECT_FALSE(parse_size_with_suffix(" 5").has_value());     // whitespace
    EXPECT_FALSE(parse_size_with_suffix("+5").has_value());     // sign
    EXPECT_FALSE(parse_size_with_suffix("1.5M").has_value());   // fraction
    EXPECT_FALSE(parse_size_with_suffix("1X").has_value());     // unknown suffix
}

TEST(ParseSizeWithSuffix, RejectsOverflow) {
    EXPECT_FALSE(parse_size_with_suffix("999999999999999999999").has_value());
    EXPECT_FALSE(parse_size_with_suffix("999999999999999999999G").has_value());
    // Largest representable "digits*T" is (2^24 - 1) * 2^40; one more unit
    // (2^64 bytes exactly) and 2^64 plain digits both overflow.
    const auto max_fit = parse_size_with_suffix("16777215T");
    ASSERT_TRUE(max_fit.has_value());
    EXPECT_EQ(*max_fit, 18446742974197923840ULL);  // 2^64 - 2^40
    EXPECT_FALSE(parse_size_with_suffix("16777216T").has_value());
    EXPECT_FALSE(parse_size_with_suffix("18446744073709551616T").has_value());
}

}  // namespace
