#ifndef FALCON_PLUGINS_BITTORRENT_SEED_POLICY_HPP
#define FALCON_PLUGINS_BITTORRENT_SEED_POLICY_HPP

#include <cstddef>
#include <cstdint>

namespace falcon {
namespace protocols {
namespace bt {

/// BitTorrent 做种退出策略（aria2 --seed-ratio / --seed-time 同语义）。
///
/// 语义（与 DownloadOptions::seed_ratio / seed_time_minutes 对齐）：
/// - seed_ratio > 0：做种直到 uploaded / 有效下载量 达到 ratio
///   （有效下载量 = max(downloaded, total_size)：下载完成的任务
///   downloaded == total_size 两者等价；纯做种任务 downloaded 为 0
///   时按 torrent 总长计，"传出去一个完整文件的比例"）
/// - seed_time_minutes > 0：做种不超过该时长（分钟）
/// - 任一条件满足即停止做种
/// - 两者均为 0：下载完成立即停止（aria2 --seed-ratio=0 不做种语义）
/// - seed_ratio == 0 且 seed_time_minutes > 0：按时长做种
struct SeedLimits {
    double ratio = 1.0;             ///< 份额比上限；0 = 不按比率
    double time_minutes = 0.0;      ///< 做种时长上限（分钟）；0 = 不按时长
};

struct SeedStats {
    std::uint64_t uploaded = 0;     ///< 累计上传字节
    std::uint64_t downloaded = 0;   ///< 本任务累计下载字节（纯做种为 0）
    std::uint64_t total_size = 0;   ///< torrent 总长（ratio 分母兜底）
    double seeded_seconds = 0.0;    ///< 已做种时长（秒）
};

/// 返回 true 表示停止做种（任务收口为终态）；false 表示继续做种
bool seeding_complete(const SeedLimits& limits, const SeedStats& stats);

}  // namespace bt
}  // namespace protocols
}  // namespace falcon

#endif  // FALCON_PLUGINS_BITTORRENT_SEED_POLICY_HPP
