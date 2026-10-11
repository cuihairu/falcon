#pragma once

// 发布改名重试：下载成品与控制文件落盘的最后一步
//
// Windows 实机/CI 的杀毒实时扫描与索引服务会在文件写入结束后短暂持有
// 句柄，紧随其后的发布改名报 sharing violation
// （ERROR_SHARING_VIOLATION / ERROR_LOCK_VIOLATION，消息为 "The process
// cannot access the file because it is being used by another process."）。
// 此时数据已完整落盘，瞬时句柄占用不该毁掉一次已完成的下载——共享重试
// 吞掉这个窗口；句柄持续占用（真死锁/占用不释放）才按最后一次错误收尾。
//
// 重试只对"被占用"类错误生效（is_transient_rename_error）：目录占用、
// 权限、跨设备等永久性错误重试不可能改变结果，第一次失败立即返回——
// 失败的收尾路径因此不烧重试预算（测试与真机都不必等 ~6s）。
//
// 调用方：成品发布（V1/V2/FTP/SFTP/WebDAV/metalink）、分段合并发布、
// 续传控制文件原子保存。
//
// 测试接缝：rename_with_retry_fn 注入改名函数，让重试循环的尝试计数与
// 放弃判定不依赖操作系统语义（POSIX 的 rename 不检查打开句柄，无法在
// 本地构造 Windows 的 sharing violation）。

#include <falcon/logger.hpp>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace falcon {
namespace protocols {
namespace detail {

// 结果携带最后一次失败的 message()，调用方拼进自己的错误文案
struct RenameRetryResult {
    bool ok = false;
    std::string error;  // 失败时非空（error_code::message()）
};

// 缺省重试节奏（毫秒）：指数退避，6 段合计 6.3s、7 次尝试。
// 实测 Windows CI 的扫描占用会击穿 4×200ms=600ms 的固定预算，
// 退避上限 3.2s 覆盖绝大多数实时扫描窗口，总预算仍远小于正常下载耗时。
inline const std::vector<unsigned>& default_publish_retry_delays_ms() {
    static const std::vector<unsigned> delays{100, 200, 400, 800, 1600, 3200};
    return delays;
}

// 只有"目标/源被其他进程占用"值得重试
inline bool is_transient_rename_error(const std::error_code& ec) {
#ifdef _WIN32
    // 32 = ERROR_SHARING_VIOLATION（句柄未以 FILE_SHARE_DELETE 打开，
    // MoveFileEx 需要的删除访问被拒——杀毒扫描/索引持句柄的签名）
    // 33 = ERROR_LOCK_VIOLATION（共享锁冲突，同族）
    return ec.value() == 32 || ec.value() == 33;
#else
    // POSIX 的对应形态：EBUSY（目录被占用/挂点）、ETXTBSY（正在执行的文件）
    return ec.value() == EBUSY || ec.value() == ETXTBSY;
#endif
}

// 注入式重试循环：do_rename(from, to, ec) 执行改名并填 ec
template <typename RenameFn>
RenameRetryResult rename_with_retry_fn(RenameFn&& do_rename,
                                       const std::filesystem::path& from,
                                       const std::filesystem::path& to,
                                       const std::vector<unsigned>& delays_ms) {
    const std::size_t attempts = delays_ms.size() + 1;
    for (std::size_t attempt = 1;; ++attempt) {
        std::error_code ec;
        do_rename(from, to, ec);
        if (!ec) {
            return {true, {}};
        }
        if (!is_transient_rename_error(ec) || attempt >= attempts) {
            return {false, ec.message()};
        }
        // 每次失败都留痕：CI 再红时日志里能直接看到占用持续了多久
        FALCON_LOG_WARN_STREAM("[download] 发布改名被占用，重试 " << attempt << "/"
                               << (attempts - 1) << ": " << from.string()
                               << " -> " << to.string() << " (" << ec.message()
                               << ")");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(delays_ms[attempt - 1]));
    }
}

// 缺省：真实文件改名 + 缺省退避节奏
inline RenameRetryResult rename_with_retry(
    const std::filesystem::path& from, const std::filesystem::path& to,
    const std::vector<unsigned>& delays_ms =
        default_publish_retry_delays_ms()) {
    return rename_with_retry_fn(
        [](const std::filesystem::path& f, const std::filesystem::path& t,
           std::error_code& ec) { std::filesystem::rename(f, t, ec); },
        from, to, delays_ms);
}

}  // namespace detail
}  // namespace protocols
}  // namespace falcon
