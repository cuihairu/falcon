#pragma once

// 故障注入框架（测试专用）。
//
// FALCON_FAILURE_INJECTION 未定义（生产构建）时 inject_failure 恒为
// constexpr false，`if (inject_failure(...) || 真实调用)` 被编译器完全
// 折叠为 `if (真实调用)`，零运行时开销、零代码膨胀。
//
// 测试构建（FALCON_BUILD_TESTS=ON 自动定义）下用 set_injection /
// ScopedInjection 置位后，下一次对应注入点检查返回 true，命中产品代码
// 既有的失败防御分支。掩码全部原子操作，产品代码可在任意线程查询。

#include <atomic>
#include <cstdint>

namespace falcon::detail {

// 注入点注册表：新增注入点在此追加，掩码上限 64 个
enum class InjectPoint : uint32_t {
    // file_hash：calculate（内存一次性哈希）EVP 防御
    FileHashCalcCtxNew,
    FileHashCalcDigestInit,
    FileHashCalcDigestUpdate,
    FileHashCalcDigestFinal,
    // file_hash：calculate_streaming（分块流式哈希）EVP 防御
    FileHashStreamCtxNew,
    FileHashStreamDigestInit,
    FileHashStreamDigestUpdate,
    FileHashStreamDigestFinal,
    // libcurl 句柄创建（storage 浏览器/资源搜索 WebCrawler 等共用一点：
    // 各测试互不并发、作用域互斥，误伤不存在）
    CurlEasyInit,
    // config_manager：AES-256-GCM 加密 EVP 防御
    ConfigEncryptCtxNew,
    ConfigEncryptInit,
    ConfigEncryptUpdate,
    ConfigEncryptFinal,
    // config_manager：AES-256-GCM 解密 EVP 防御
    ConfigDecryptCtxNew,
    ConfigDecryptInit,
    ConfigDecryptSetTag,
    ConfigDecryptUpdate,
    // upyun：请求签名（MD5 + HMAC-MD5）防御链
    UpyunSigCtxNew,
    UpyunSigDigestInit,
    UpyunSigDigestUpdate,
    UpyunSigDigestFinal,
    UpyunSigHmacNull,
    // cos：请求签名（SHA-256）防御链
    CosShaCtxNew,
    CosShaDigestInit,
    CosShaDigestUpdate,
    CosShaDigestFinal,
    // event_poll (epoll)：实例创建失败 / epoll_wait 失败
    EpollCreate1,
    EpollWaitFail,
    // DHT：UDP socket 创建失败
    DhtSocketCreate,
    // JSON-RPC 服务器：监听 socket 创建 / listen 失败
    RpcServerSocket,
    RpcServerListen,
    // WebSocket RPC 客户端：send_all 失败（握手发送与请求帧发送共用）
    WsClientSendFail,
    // V2 引擎 run() 事件循环体异常（std / 非 std 两形态，覆盖顶层
    // 兜底 catch——单命令与例程命令异常已有内层收口）
    EngineLoopThrowStd,
    EngineLoopThrowNonStd,
    // incremental_download：calculateHash EVP 防御
    IncrementalHashInit,
    IncrementalHashUpdate,
    IncrementalHashFinal,
    // HTTP 代理隧道：CONNECT 请求发送硬错误（回环上 RST 先于客户端
    // 首个 send 到达是亚毫秒竞态，注入点确定性命中收口分支）
    ProxyConnectSendFail,
    // V2 HTTP 命令：socket 创建 / connect 硬失败（回环上非阻塞 connect
    // 恒报 in-progress，立即硬失败在本地网络环境不可构造）
    HttpSocketCreate,
    HttpConnectHardFail,
    // V2 HTTP 命令 TLS 链：method/ctx/ssl 创建、fd 绑定、主机名绑定、
    // 首轮握手 WANT_WRITE、请求发送硬失败（创建类注入短路真实创建，
    // 防泄漏遵循批次 Z 形态）
    TlsMethodFail,
    TlsCtxNewFail,
    TlsSslNewFail,
    TlsSetFdFail,
    TlsSetHostFail,
    TlsHandshakeWantWrite,
    TlsRequestWriteFail,
    TlsRequestWriteWantWrite,
    // V2 HTTP 命令：请求发送阶段的 WANT_WRITE 重入（回环上请求恒
    // 小于内核发送缓冲，明文 send / SSL_write / 代理 CONNECT 三处
    // 的非阻塞挂起不可自然构造）
    HttpSendWantWrite,
    // daemon 守护化：fork / setsid 失败（回环上 fork 恒成功、setsid
    // 在有控制终端的会话首进程才失败，均不可自然构造）
    DaemonizeForkFail,
    DaemonizeSetsidFail,
    DaemonizeFork2Fail,
    // V2 引擎 socket 事件回调异常（std / 非 std 两形态，覆盖回调
    // lambda 顶层兜底 catch——handle_socket_ready 内部异常已有边界，
    // 兜底只在异常逃出该函数时可达）
    SocketReadyThrowStd,
    SocketReadyThrowNonStd,
    // V2 引擎 socket 事件注册失败（event_poll add_event 返回 false
    // 在回环上不可自然构造——fd 新鲜且有效）
    EventPollAddFail,
    // poll 后端系统调用失败（EINTR / 非 EINTR 硬错误两形态；回环
    // 测试下 poll 恒成功或超时）
    PollSyscallEintr,
    PollSyscallFail,
};

#if FALCON_FAILURE_INJECTION

inline std::atomic<uint64_t>& injection_mask() {
    static std::atomic<uint64_t> mask{0};
    return mask;
}

inline bool inject_failure(InjectPoint point) {
    return (injection_mask().load(std::memory_order_relaxed) &
            (uint64_t{1} << static_cast<uint32_t>(point))) != 0;
}

inline void set_injection(InjectPoint point, bool enabled) {
    const uint64_t bit = uint64_t{1} << static_cast<uint32_t>(point);
    if (enabled) {
        injection_mask().fetch_or(bit, std::memory_order_relaxed);
    } else {
        injection_mask().fetch_and(~bit, std::memory_order_relaxed);
    }
}

// RAII：构造置位、析构清除，异常安全
class ScopedInjection {
public:
    explicit ScopedInjection(InjectPoint point) : point_(point) {
        set_injection(point_, true);
    }
    ~ScopedInjection() { set_injection(point_, false); }
    ScopedInjection(const ScopedInjection&) = delete;
    ScopedInjection& operator=(const ScopedInjection&) = delete;

private:
    InjectPoint point_;
};

#else  // 生产构建：常量 false，编译器消除

inline constexpr bool inject_failure(InjectPoint) { return false; }
inline void set_injection(InjectPoint, bool) {}

class ScopedInjection {
public:
    explicit ScopedInjection(InjectPoint) {}
};

#endif

}  // namespace falcon::detail
