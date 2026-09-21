#include "seed_policy.hpp"

#include <algorithm>

namespace falcon::protocols::bt {

bool seeding_complete(const SeedLimits& limits, const SeedStats& stats)
{
    // 两者皆 0：下载完成即停（aria2 --seed-ratio=0 不做种语义）
    if (limits.ratio <= 0.0 && limits.time_minutes <= 0.0) {
        return true;
    }

    // 时间条件：显式设置且已做种满时限
    if (limits.time_minutes > 0.0 &&
        stats.seeded_seconds >= limits.time_minutes * 60.0) {
        return true;
    }

    // 比率条件：uploaded / max(downloaded, total_size) 达标
    // （下载完成 downloaded==total 两者等价；纯做种 downloaded=0 时
    //   按 torrent 总长计"传出去一个完整文件的比例"）
    const std::uint64_t denominator =
        std::max<std::uint64_t>(stats.downloaded, stats.total_size);
    if (limits.ratio > 0.0) {
        if (denominator == 0) {
            // 无总长信息的退化情形：比率不可判定，不因比率退出
            return false;
        }
        const double ratio =
            static_cast<double>(stats.uploaded) / static_cast<double>(denominator);
        if (ratio >= limits.ratio) {
            return true;
        }
    }

    return false;
}

}  // namespace falcon::protocols::bt
