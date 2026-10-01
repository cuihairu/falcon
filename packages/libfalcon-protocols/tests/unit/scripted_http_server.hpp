// 自包含可编程 HTTP 测试服务器(从 http_handler_edges_test.cpp 抽取,
// 供 HTTP handler 与 metalink 委托等多套回环测试共用)
//
// 能力:记录请求 + 响应剧本(按次序,末位无限重复)+ 自动 Range 206
// 切片 + 分块慢发(进度/暂停窗口)+ 一次性中断(短传 → 重试续传)+
// 无 Content-Length 的 EOF 定界响应。

#pragma once

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// Windows 缺少 POSIX socket 语义的符号，测试服务器代码统一走这些别名
#include <cstddef>  // std::ptrdiff_t（MSVC 不经其他头传递提供）
using ssize_t = std::ptrdiff_t;
#define SHUT_RDWR SD_BOTH
#define MSG_NOSIGNAL 0  // Windows 无 SIGPIPE，标志位无意义
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>
#define CLOSE_SOCKET(fd) close(fd)
#endif

namespace falcon::testscripts {
// 最近一次 socket 系统调用的错误码（Windows WSAGetLastError / POSIX
// errno）——连接事件取证用（红面诊断轮：无条件记录判别事实）
inline int sock_last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
}  // namespace falcon::testscripts

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace falcon::testscripts {

namespace fs = std::filesystem;

struct RecordedRequest {
    std::string method;
    std::string path;
    std::string range;  // Range 头原值（无则空）
    std::unordered_map<std::string, std::string> headers;  // 键小写
};

struct FakeResponse {
    int status = 200;
    std::string status_text = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    bool support_range = false;  // 请求带 Range 时自动 206 + 切片
    // 非空：Range 请求必须携带匹配的 If-Range 才切片（206），否则 200
    // 全量——模拟「续传间隙资源已变更」的服务器（If-Range 门禁测试）
    std::string if_range_guard;
    // true：Range 请求携带任意 If-Range 即接受（切片）——HTTP-date 形
    // 态验证器的值依赖被续传文件的 mtime，测试无法预知精确值，用本
    // 模式钉住「date 形态 If-Range 被服务器接受 → 206 正常续传」
    bool require_if_range = false;
    bool fail_nonzero_range = false;  // 起始 >0 的 Range 请求一律 500（段失败收口）
    bool no_length = false;  // 不发 Content-Length，body 以连接关闭为界（未知总长）
    bool single_write = false;  // 头与 body 拼成一次 send（构造"首包即含
                                // body"的初始批次数据，如 /dev/full 首写失败）
    int defer_partial_ms = 0;   // >0：延迟后只发 partial_prefix（半截响应头）
                                // ——客户端收到前缀后继续 recv 得 EAGAIN 走
                                // 重注册路径（响应头 recv 注册失败注入的
                                // 确定性命中窗口）
    std::string partial_prefix = "HTTP/1.1 200 OK\r\n";
};

// 连接级事件（红面取证用）：accept / 请求读取 recv 终止 / defer 路径
// recv 返回值与错误码 / 关闭。Windows 确定性 ~140ms abort 诊断轮的
// 判别事实源——服务器侧唯一主动关闭点在 defer 路径，该路径 recv 的
// 返回值（0=对端 FIN / >0=对端来数据 / <0=错误码）直接指认关闭链
struct ConnEvent {
    std::string what;  // accept / reqrecv_close / defer_recv / defer_close / conn_close
    long long ms = 0;  // 距 start() 的毫秒
    int conn = 0;      // 连接序号（accept 顺序）
    long long n = 0;   // recv 返回值
    int err = 0;       // sock_last_error() 快照
};

class ScriptedHttpServer {
public:
    ScriptedHttpServer() = default;
    ~ScriptedHttpServer() { stop(); }
    ScriptedHttpServer(const ScriptedHttpServer&) = delete;
    ScriptedHttpServer& operator=(const ScriptedHttpServer&) = delete;

    void start() {
#ifdef _WIN32
        static std::once_flag wsa_once;
        std::call_once(wsa_once, [] {
            WSADATA data{};
            WSAStartup(MAKEWORD(2, 2), &data);
        });
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            std::fprintf(stderr, "ScriptedHttpServer bind/listen failed\n");
            std::abort();
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ntohs(bound.sin_port);
        running_ = true;
        started_ = std::chrono::steady_clock::now();
        accept_thread_ = std::thread([this] { accept_loop(); });
    }

    /// 连接事件快照（红面诊断读取；拷贝出避免锁内遍历）
    std::vector<ConnEvent> conn_events() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return conn_events_;
    }

    /// dual-stack 监听（AF_INET6 + IPV6_V6ONLY=0）：同一端口同时接受
    /// 127.0.0.1 与 [::1]。socket/bind 失败（环境无 IPv6）返回 false，
    /// 调用方 GTEST_SKIP——不 abort（与 start() 的确定性环境假设不同）
    bool start_v6() {
#ifdef _WIN32
        static std::once_flag wsa_once;
        std::call_once(wsa_once, [] {
            WSADATA data{};
            WSAStartup(MAKEWORD(2, 2), &data);
        });
#endif
        listen_fd_ = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return false;
        }
        int v6only = 0;
        ::setsockopt(listen_fd_, IPPROTO_IPV6, IPV6_V6ONLY,
                     reinterpret_cast<const char*>(&v6only), sizeof(v6only));
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_addr = in6addr_any;
        addr.sin6_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return false;
        }
        sockaddr_in6 bound{};
        socklen_t len = sizeof(bound);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len);
        port_ = ntohs(bound.sin6_port);
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        ::shutdown(listen_fd_, SHUT_RDWR);  // Linux close() 不唤醒 accept
        CLOSE_SOCKET(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();

        // detached 连接线程收尾等待（RawWsServer 先例）：accept 线程
        // join 后不再产生新连接线程；对存量连接 fd shutdown 唤醒阻塞
        // 在 recv 的线程，再轮询活跃计数归零——线程体最后一次 this
        // 访问是计数递减，归零即再无线程触碰本对象。此前连接线程可
        // 在测试函数返回、栈帧被后续测试复用后仍锁本对象的 mutex_
        //（栈复用后 kind 字段是垃圾）→ glibc tpp/ESRCH 断言或
        // std::system_error 进程级崩溃（全量 falcon_protocols_tests
        // #622 崩溃根因）。超时兜底有界：宁可放走一个迟到线程（各
        // 连接自带 SO_RCVTIMEO）也不让析构后的触碰成为无界 UB
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(5);
        while (active_conns_.load(std::memory_order_relaxed) != 0) {
            std::vector<int> pending;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                pending.assign(conn_fds_.begin(), conn_fds_.end());
            }
            for (int fd : pending) ::shutdown(fd, SHUT_RDWR);
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    int port() const { return port_; }
    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + path;
    }
    /// IPv6 字面量形态 URL（start_v6 的 dual-stack 监听下与 v4 同端口）
    std::string url_v6(const std::string& path) const {
        return "http://[::1]:" + std::to_string(port_) + path;
    }

    // ---- 应答编程 ----

    void set_response(const std::string& path, const FakeResponse& resp) {
        std::lock_guard<std::mutex> lock(mutex_);
        responses_[path] = resp;
    }

    /// 按请求次序应答（末尾的应答无限重复）
    void set_script(const std::string& path, std::vector<FakeResponse> script) {
        std::lock_guard<std::mutex> lock(mutex_);
        scripts_[path] = std::move(script);
    }

    /// body 分块慢发（进度/暂停窗口）；range_start >= 0 时仅对
    /// 该起始偏移的 Range 请求慢发（多段差异化速度）
    void set_slow_body(const std::string& path, int chunk_delay_us,
                       size_t chunk_size = 32, long range_start = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        slow_[path] = SlowSpec{chunk_delay_us, chunk_size, range_start};
    }

    void clear_slow_body(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        slow_.erase(path);
    }

    /// 一次性中断：对匹配 Range（range_start<0 则任意请求）发出
    /// n_bytes 字节后直接断连（段短传 → 尺寸校验失败 → 重试续传）
    void set_abort_after(const std::string& path, size_t n_bytes,
                         long range_start = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        abort_[path] = SlowSpec{0, n_bytes, range_start};
    }

    /// 黑洞：对匹配 Range（range_start<0 则任意请求）收下请求后
    /// 不回任何字节，连接保持到客户端断开/服务器收超时（响应头
    /// 阶段挂起 → 客户端超时清理路径）
    void set_black_hole(const std::string& path, long range_start = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        black_holes_[path] = SlowSpec{0, 0, range_start};
    }

    /// 立即断连：对匹配 Range（range_start<0 则任意请求）收下请求
    /// 后不回任何字节直接关连接（响应头阶段断连，客户端按连接失败
    /// 收口 → 连接级重试链；与黑洞的区别是不等客户端先断）
    void set_fail_immediate(const std::string& path, long range_start = -1) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_immediates_[path] = SlowSpec{0, 0, range_start};
    }

    /// HEAD 专用应答（GET 保持 set_response/script 剧本不变）。引擎
    /// 数据面（V2）纯 GET 不会产生 HEAD,而 HEAD 探测只出现在下载
    /// 入口——可借此按阶段切换剧本。flip_get=true 时首个 HEAD 之后
    /// 把该 path 的 GET 应答一并替换为 resp
    void set_head_response(const std::string& path, const FakeResponse& resp,
                           bool flip_get = false) {
        std::lock_guard<std::mutex> lock(mutex_);
        head_responses_[path] = resp;
        head_flip_get_[path] = flip_get;
    }

    // ---- 记录 ----

    std::vector<RecordedRequest> requests() {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    size_t count_requests(const std::string& path, const std::string& method = {}) {
        size_t n = 0;
        for (const auto& r : requests()) {
            if (r.path == path && (method.empty() || r.method == method)) ++n;
        }
        return n;
    }

    // 已 accept 的连接数（与请求计数解耦：观测"连接已建立但请求
    // 未到达"——请求发送被 WANT_WRITE 注入拦下的服务器侧证据）
    int connections() { return connections_.load(std::memory_order_relaxed); }

private:
    static bool send_all(int fd, const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    // Winsock 的 SO_RCVTIMEO 取 DWORD 毫秒而非 timeval（传 timeval
    // 会被按前 4 字节解读为 N 毫秒：timeval{15,0} 的原始字节 = 15ms
    // 超时——Windows 红面根因：defer 分支的阻塞 recv 在 ~15ms 被
    // WSAETIMEDOUT 抢醒、服务器抢先 close，客户端 LOW_SPEED 看门狗
    // 收口前就吃到 CURLE_GOT_NOTHING）。AdapterTestServer 的
    // set_socket_timeout 同款范式
    static void set_recv_timeout(int fd, int seconds) {
#ifdef _WIN32
        const DWORD ms = static_cast<DWORD>(seconds) * 1000u;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                         reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
        timeval tv{};
        tv.tv_sec = seconds;
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    void accept_loop() {
        while (running_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) return;
                continue;
            }
            connections_.fetch_add(1, std::memory_order_relaxed);
            const int conn = conn_seq_.fetch_add(1);
            trace("accept", conn, 0, 0);
            set_recv_timeout(fd, 15);
            // detached 连接线程预登记（RawWsServer 先例）：spawn 与登
            // 记间无窗口；线程体最后一次 this 访问是计数递减，stop()
            // 等计数归零后才允许对象析构
            {
                std::lock_guard<std::mutex> lock(mutex_);
                conn_fds_.insert(fd);
            }
            active_conns_.fetch_add(1, std::memory_order_relaxed);
            std::thread([this, fd, conn] {
                handle_connection(fd, conn);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    conn_fds_.erase(fd);
                }
                active_conns_.fetch_sub(1, std::memory_order_relaxed);
            }).detach();
        }
    }

    void trace(const char* what, int conn, long long n, int err) {
        std::lock_guard<std::mutex> lock(mutex_);
        conn_events_.push_back(
            {what,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started_)
                 .count(),
             conn, n, err});
    }

    void handle_connection(int fd, int conn) {
        std::string buffer;
        for (;;) {
            // 读一个请求（到空行；GET 无 body）
            size_t head_end;
            while ((head_end = buffer.find("\r\n\r\n")) == std::string::npos) {
                char chunk[4096];
                ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
                if (n <= 0) {
                    trace("reqrecv_close", conn, static_cast<long long>(n),
                          sock_last_error());
                    CLOSE_SOCKET(fd);
                    return;
                }
                buffer.append(chunk, static_cast<size_t>(n));
            }
            const std::string head = buffer.substr(0, head_end);
            buffer.erase(0, head_end + 4);

            RecordedRequest rec;
            std::istringstream stream(head);
            std::string line;
            std::getline(stream, line);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const size_t sp1 = line.find(' ');
            const size_t sp2 = line.find(' ', sp1 + 1);
            if (sp1 != std::string::npos && sp2 != std::string::npos) {
                rec.method = line.substr(0, sp1);
                rec.path = line.substr(sp1 + 1, sp2 - sp1 - 1);
            }
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                for (auto& ch : key) ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
                size_t vstart = colon + 1;
                while (vstart < line.size() && line[vstart] == ' ') ++vstart;
                std::string value = line.substr(vstart);
                if (key == "range") rec.range = value;
                rec.headers[key] = value;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                requests_.push_back(rec);
            }

            FakeResponse resp;
            bool slow = false;
            int slow_delay = 0;
            size_t slow_chunk = 32;
            size_t abort_after = 0;
            bool abort_match = false;
            // 查表键剥除 query（记录保留原样供断言）
            std::string route = rec.path;
            const size_t qpos = route.find('?');
            if (qpos != std::string::npos) route = route.substr(0, qpos);
            long request_range_start = -1;
            if (rec.range.rfind("bytes=", 0) == 0) {
                const std::string spec = rec.range.substr(6);
                const size_t dash = spec.find('-');
                if (dash != std::string::npos) {
                    request_range_start =
                        static_cast<long>(std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10));
                }
            }
            {
                bool is_black_hole = false;
                bool is_fail_immediate = false;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto bh = black_holes_.find(route);
                    is_black_hole =
                        bh != black_holes_.end() &&
                        (bh->second.range_start < 0 ||
                         bh->second.range_start == request_range_start);
                    auto fi = fail_immediates_.find(route);
                    is_fail_immediate =
                        fi != fail_immediates_.end() &&
                        (fi->second.range_start < 0 ||
                         fi->second.range_start == request_range_start);
                }
                if (is_black_hole) {
                    // 黑洞：不回任何字节，recv 阻塞到客户端断开或
                    // SO_RCVTIMEO 到期（必须锁外等待，否则 mutex 被
                    // 占住会拖死其他连接的应答）
                    char drain[4096];
                    while (::recv(fd, drain, sizeof(drain), 0) > 0) {
                    }
                    CLOSE_SOCKET(fd);
                    return;
                }
                if (is_fail_immediate) {
                    CLOSE_SOCKET(fd);
                    return;
                }
                std::lock_guard<std::mutex> lock(mutex_);
                auto script = scripts_.find(route);
                auto hresp = rec.method == "HEAD"
                                 ? head_responses_.find(route)
                                 : head_responses_.end();
                if (hresp != head_responses_.end()) {
                    resp = hresp->second;
                    if (head_flip_get_[route]) responses_[route] = hresp->second;
                } else if (script != scripts_.end()) {
                    auto& seq = script->second;
                    if (rec.method == "HEAD") {
                        // HEAD 探测（get_file_info）恒用末位应答：
                        // 剧本只驱动 GET 的错误/重试次序，否则 500 剧本
                        // 会在 download() 顶部的 HEAD 处直接炸掉
                        resp = seq.back();
                    } else if (seq.size() > 1) {
                        resp = seq.front();
                        seq.erase(seq.begin());
                    } else {
                        resp = seq.front();  // 末位应答无限重复
                    }
                } else {
                    auto it = responses_.find(route);
                    resp = it != responses_.end() ? it->second
                                                  : FakeResponse{404, "Not Found", {}, "missing", false, {}, false, false, false, false, 0, {}};
                }
                auto d = slow_.find(route);
                if (d != slow_.end() &&
                    (d->second.range_start < 0 ||
                     d->second.range_start == request_range_start)) {
                    slow = true;
                    slow_delay = d->second.delay_us;
                    slow_chunk = d->second.chunk;
                }
                abort_after = 0;
                abort_match = false;
                auto a = abort_.find(route);
                if (a != abort_.end() && rec.method != "HEAD" &&
                    (a->second.range_start < 0 ||
                     a->second.range_start == request_range_start)) {
                    // HEAD 不消费：abort 是「发部分 body 后断连」语义，
                    // HEAD 无 body（一次性条目留给首个 GET）
                    abort_after = a->second.chunk;  // 复用 chunk 存字节数
                    abort_match = true;
                    abort_.erase(a);  // 一次性
                }
            }

            // Range 自动处理：206 + 切片
            std::string body = resp.body;
            int status = resp.status;
            std::string status_text = resp.status_text;
            if (resp.fail_nonzero_range && !rec.range.empty() &&
                rec.range.rfind("bytes=", 0) == 0) {
                const std::string spec = rec.range.substr(6);
                const size_t dash = spec.find('-');
                if (dash != std::string::npos &&
                    std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10) > 0) {
                    status = 500;
                    status_text = "Internal Server Error";
                    body.clear();
                }
            }
            // If-Range 门禁：守卫非空且请求的 If-Range 不匹配 → 视为
            // 资源已变更，跳过切片回 200 全量（不追加 Content-Range）；
            // require_if_range 模式（守卫为空时）：携带任意 If-Range 即
            // 接受（date 形态的值依赖文件 mtime，测试无法预知）
            const bool if_range_present = rec.headers.count("if-range") != 0;
            const bool if_range_ok =
                !resp.if_range_guard.empty()
                    ? (if_range_present &&
                       rec.headers.at("if-range") == resp.if_range_guard)
                    : (resp.require_if_range ? if_range_present : true);
            if (resp.support_range && if_range_ok && !rec.range.empty() &&
                rec.range.rfind("bytes=", 0) == 0 && status == 200) {
                const std::string spec = rec.range.substr(6);
                const size_t dash = spec.find('-');
                if (dash != std::string::npos) {
                    const uint64_t start = std::strtoull(spec.substr(0, dash).c_str(), nullptr, 10);
                    const uint64_t end = spec.substr(dash + 1).empty()
                                             ? body.size() - 1
                                             : std::min<uint64_t>(
                                                   std::strtoull(spec.substr(dash + 1).c_str(), nullptr, 10),
                                                   body.size() - 1);
                    if (start < body.size() && end >= start) {
                        body = body.substr(static_cast<size_t>(start),
                                           static_cast<size_t>(end - start + 1));
                        status = 206;
                        status_text = "Partial Content";
                        resp.headers.emplace_back(
                            "Content-Range",
                            "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                                std::to_string(resp.body.size()));
                    }
                }
            }

            if (resp.defer_partial_ms > 0) {
                // 延迟后只发响应头前缀，然后挂住等客户端断开（注册失败
                // 收口后命令关闭 fd）：不主动关是给客户端留出稳定的
                // 「前缀已到、剩余永不来」的 EAGAIN 状态
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(resp.defer_partial_ms));
                (void)send_all(fd, resp.partial_prefix);
                char drain[256];
                const ssize_t dn = ::recv(fd, drain, sizeof(drain), 0);
                trace("defer_recv", conn, static_cast<long long>(dn),
                      sock_last_error());
                trace("defer_close", conn, 0, 0);
                break;  // 尾部统一 conn_close + CLOSE_SOCKET（此前此处
                        // 先关一次、尾部再关——双重 close，编号被复用
                        // 时可能误关不属于自己的 socket）
            }

            std::string out = "HTTP/1.1 " + std::to_string(status) + " " + status_text + "\r\n";
            bool has_len = false;
            for (const auto& [k, v] : resp.headers) {
                out += k + ": " + v + "\r\n";
                if (k == "Content-Length") has_len = true;
            }
            if (!has_len) {
                if (resp.no_length) {
                    out += "Connection: close\r\n";  // body 以 EOF 为界
                } else {
                    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
                }
            }
            out += "\r\n";
            if (resp.single_write && rec.method != "HEAD" && !body.empty()) {
                // 头 + body 单次 send：客户端首个 recv 必然同时拿到
                // 响应头与首批 body 字节
                std::string combined = out;
                combined += body;
                send_all(fd, combined);
                break;
            }
            if (!send_all(fd, out)) break;
            if (rec.method != "HEAD") {
                if (abort_match && abort_after > 0 && abort_after < body.size()) {
                    // 发出部分数据后硬断连：客户端按 Content-Length 判短传
                    send_all(fd, body.substr(0, abort_after));
                    break;
                }
                if (slow) {
                    for (size_t pos = 0; pos < body.size(); pos += slow_chunk) {
                        std::this_thread::sleep_for(std::chrono::microseconds(slow_delay));
                        if (!send_all(fd, body.substr(pos, slow_chunk))) break;
                    }
                    if (slow && resp.no_length) break;  // EOF 定界：发完即关
                } else if (!body.empty()) {
                    send_all(fd, body);
                    if (resp.no_length) break;  // EOF 定界：发完即关
                }
            }
        }
        trace("conn_close", conn, 0, 0);
        CLOSE_SOCKET(fd);
    }

    struct SlowSpec {
        int delay_us;
        size_t chunk;
        long range_start;  // -1 = 所有请求
    };

    std::vector<RecordedRequest> requests_;
    std::unordered_map<std::string, FakeResponse> responses_;
    std::unordered_map<std::string, std::vector<FakeResponse>> scripts_;
    std::unordered_map<std::string, SlowSpec> slow_;
    std::unordered_map<std::string, SlowSpec> abort_;
    std::unordered_map<std::string, SlowSpec> black_holes_;
    std::unordered_map<std::string, SlowSpec> fail_immediates_;
    std::unordered_map<std::string, FakeResponse> head_responses_;
    std::unordered_map<std::string, bool> head_flip_get_;

    std::atomic<bool> running_{false};
    std::atomic<int> connections_{0};
    std::atomic<int> conn_seq_{0};
    // 存活连接线程数与连接 fd 台账（mutex_ 守护台账）——stop() 收尾
    // 等待的依据，见 stop() 注释
    std::atomic<int> active_conns_{0};
    std::set<int> conn_fds_;
    std::chrono::steady_clock::time_point started_{
        std::chrono::steady_clock::now()};
    mutable std::mutex mutex_;
    std::vector<ConnEvent> conn_events_;
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread accept_thread_;
};

//==============================================================================
// 夹具与辅助
//==============================================================================

/// 每次调用产生唯一后缀(时间戳 + 进程内计数),临时目录防撞
inline int unique_suffix() {
    static int counter = 0;
    return static_cast<int>(
        std::chrono::steady_clock::now().time_since_epoch().count() % 1000000) +
        (counter++);
}

/// RAII 临时目录
class TempDir {
public:
    TempDir() {
        path_ = fs::temp_directory_path() /
                ("falcon_test_" + std::to_string(unique_suffix()));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const fs::path& path() const { return path_; }
    std::string string() const { return path_.string(); }

private:
    fs::path path_;
};

} // namespace falcon::testscripts
