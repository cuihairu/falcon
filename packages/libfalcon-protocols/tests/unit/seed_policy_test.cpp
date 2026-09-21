// BT 做种退出策略纯逻辑单测（aria2 --seed-ratio/--seed-time 同语义）
#include "seed_policy.hpp"

#include <gtest/gtest.h>

namespace {

using falcon::protocols::bt::SeedLimits;
using falcon::protocols::bt::SeedStats;
using falcon::protocols::bt::seeding_complete;

SeedStats stats(std::uint64_t uploaded, std::uint64_t downloaded,
                std::uint64_t total, double seconds)
{
    SeedStats s;
    s.uploaded = uploaded;
    s.downloaded = downloaded;
    s.total_size = total;
    s.seeded_seconds = seconds;
    return s;
}

// 默认 1.0/0：比率未达标继续做种
TEST(SeedPolicyTest, DefaultRatioNotReachedKeepsSeeding)
{
    SeedLimits limits;  // ratio 1.0, time 0
    EXPECT_FALSE(seeding_complete(limits, stats(50, 100, 100, 0)));
    EXPECT_FALSE(seeding_complete(limits, stats(99, 100, 100, 3600)));
}

// 恰好达标与超过
TEST(SeedPolicyTest, RatioReachedStops)
{
    SeedLimits limits;  // 1.0
    EXPECT_TRUE(seeding_complete(limits, stats(100, 100, 100, 0)));
    EXPECT_TRUE(seeding_complete(limits, stats(250, 100, 100, 0)));
}

// 纯做种任务（downloaded=0）分母回落 torrent 总长
TEST(SeedPolicyTest, SeedOnlyUsesTotalAsDenominator)
{
    SeedLimits limits;  // 1.0
    EXPECT_FALSE(seeding_complete(limits, stats(50, 0, 100, 0)));
    EXPECT_TRUE(seeding_complete(limits, stats(100, 0, 100, 0)));
}

// ratio=2.0：上传两倍于体积才停
TEST(SeedPolicyTest, HigherRatioRequiresMoreUpload)
{
    SeedLimits limits;
    limits.ratio = 2.0;
    EXPECT_FALSE(seeding_complete(limits, stats(199, 100, 100, 0)));
    EXPECT_TRUE(seeding_complete(limits, stats(200, 100, 100, 0)));
}

// 两者皆 0：下载完成立即停止（不做种）
TEST(SeedPolicyTest, ZeroLimitsStopImmediately)
{
    SeedLimits limits;
    limits.ratio = 0.0;
    limits.time_minutes = 0.0;
    EXPECT_TRUE(seeding_complete(limits, stats(0, 100, 100, 0)));
}

// ratio=0 但 time>0：按时长做种
TEST(SeedPolicyTest, TimeOnlySeedingHonorsDeadline)
{
    SeedLimits limits;
    limits.ratio = 0.0;
    limits.time_minutes = 1.0;
    EXPECT_FALSE(seeding_complete(limits, stats(0, 100, 100, 59)));
    EXPECT_TRUE(seeding_complete(limits, stats(0, 100, 100, 60)));
}

// 任一条件满足即退出（比率先到）
TEST(SeedPolicyTest, EitherConditionStops)
{
    SeedLimits limits;
    limits.ratio = 1.0;
    limits.time_minutes = 10.0;
    EXPECT_TRUE(seeding_complete(limits, stats(100, 100, 100, 30)));
    // 时间先到、比率未到同样退出
    EXPECT_TRUE(seeding_complete(limits, stats(10, 100, 100, 600)));
}

// 无总长信息的退化情形：比率不可判定，仅时间条件生效
TEST(SeedPolicyTest, ZeroTotalIgnoresRatioCondition)
{
    SeedLimits limits;
    limits.ratio = 1.0;
    EXPECT_FALSE(seeding_complete(limits, stats(100, 0, 0, 0)));
    limits.time_minutes = 1.0;
    EXPECT_TRUE(seeding_complete(limits, stats(0, 0, 0, 60)));
}

}  // namespace
