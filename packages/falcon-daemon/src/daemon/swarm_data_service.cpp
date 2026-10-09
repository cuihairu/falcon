/**
 * @file swarm_data_service.cpp
 * @brief P2SP 阶段 2 增量 1：入站只读 HTTP 数据服务实现（§10.3）。
 */

// winsock2 必须先于项目头：swarm_data_service.hpp → swarm_announcer.hpp →
// config.hpp → daemon.hpp 在 _WIN32 下拉入 windows.h（含 winsock.h），
// 后置 winsock2.h 会撞 'sockaddr'/'fd_set' 重定义（MSVC C2011）。
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "swarm_data_service.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <sstream>

#ifndef _WIN32
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <falcon/logger.hpp>

namespace falcon::daemon {

namespace {

#ifdef _WIN32
using socket_len_t = int;
constexpr int kInvalidSocket = INVALID_SOCKET;
void close_socket(int fd) { ::closesocket(fd); }
#else
using socket_len_t = socklen_t;
constexpr int kInvalidSocket = -1;
void close_socket(int fd) { ::close(fd); }
#endif

// send() 全量写出：返回 true 表示全部字节已交给内核。
// POSIX 走 MSG_NOSIGNAL（对端 RST 后写 socket 不杀进程——daemon 其余
// 网络基建同纪律）；macOS 无 MSG_NOSIGNAL 走 SO_NOSIGPIPE（套接字级，
// 与引擎 POSIX send 的 kSendFlags 同形态）。
bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
#ifdef _WIN32
        int n = ::send(fd, data + off, static_cast<int>(len - off), 0);
#else
        int n = static_cast<int>(::send(fd, data + off, len - off, MSG_NOSIGNAL));
#endif
        if (n <= 0) {
#ifdef _WIN32
            if (n < 0 && WSAGetLastError() == WSAEINTR) continue;
#else
            if (n < 0 && errno == EINTR) continue;
#endif
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 读满请求头区（到 \r\n\r\n）或对端关闭；返回 false 表示连接已不可用。
// 有界：头区上限 16KB，防无界膨胀。
bool recv_request_headers(int fd, std::string* out) {
    constexpr std::size_t kMaxHeaderBytes = 16 * 1024;
    char buf[4096];
    while (true) {
        if (out->size() >= kMaxHeaderBytes) return false;  // 头区超限，放弃
#ifdef _WIN32
        int n = ::recv(fd, buf, sizeof(buf), 0);
#else
        auto n = ::recv(fd, buf, sizeof(buf), 0);
#endif
        if (n == 0) return false;  // 对端关闭
        if (n < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEINTR) continue;
#endif
            return false;
        }
        out->append(buf, static_cast<std::size_t>(n));
        if (out->find("\r\n\r\n") != std::string::npos) return true;
    }
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool is_hex_string(const std::string& s) {
    if (s.empty()) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}

// 十进制无符号解析（无前导符号/空白/垃圾字符容忍）；空串或非法字符
// 返回 false。parse_range 与 make_data_service_config 共用。
bool to_u64(const std::string& t, std::uint64_t* out) {
    if (t.empty()) return false;
    std::uint64_t v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<std::uint64_t>(c - '0');
    }
    *out = v;
    return true;
}

// 解析请求头里的 Range: bytes=start-end / bytes=start- / bytes=-suffix。
// 返回 true 表示解析出合法区间（含 suffix 语义展开后）。end_exclusive 为
// [start, end) 半开区间，便于切文件流。total 用于 suffix 展开与钳制。
bool parse_range(const std::string& value, std::uint64_t total, std::uint64_t* start,
                 std::uint64_t* end_exclusive) {
    const std::string prefix = "bytes=";
    if (value.size() <= prefix.size() || value.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    std::string spec = value.substr(prefix.size());
    // 只支持单区间（P2SP 拉取方只用单段 Range）；逗号出现即多区间，拒绝。
    if (spec.find(',') != std::string::npos) return false;

    auto dash = spec.find('-');
    if (dash == std::string::npos) return false;
    std::string a = spec.substr(0, dash);
    std::string b = spec.substr(dash + 1);

    if (a.empty()) {
        // suffix-range：末 N 字节
        std::uint64_t suffix = 0;
        if (!to_u64(b, &suffix)) return false;
        if (suffix == 0 || total == 0) return false;
        if (suffix > total) suffix = total;
        *start = total - suffix;
        *end_exclusive = total;
        return true;
    }

    std::uint64_t s = 0;
    if (!to_u64(a, &s)) return false;
    if (total != 0 && s >= total) return false;  // 起点越界

    if (b.empty()) {
        *start = s;
        *end_exclusive = total;  // 开放末端（total==0 时调用方按不可服务挡下）
        return true;
    }
    std::uint64_t e = 0;
    if (!to_u64(b, &e)) return false;
    if (e < s) return false;  // 终点早于起点
    if (total != 0 && e >= total) e = total - 1;  // 钳到文件尾（RFC 7233）
    *start = s;
    *end_exclusive = e + 1;
    return true;
}

}  // namespace

SwarmDataServiceConfig make_data_service_config(
    const std::string& advertise_addr) {
    SwarmDataServiceConfig config;  // 默认 0.0.0.0:0（OS 分配，port() 回读）
    const auto colon = advertise_addr.rfind(':');
    if (colon == std::string::npos) return config;
    const std::string ip = advertise_addr.substr(0, colon);
    const std::string port_str = advertise_addr.substr(colon + 1);
    std::uint64_t port = 0;
    // 非法形态（空段/非数字/越界/IPv4 不可解析）一律回落默认绑定点——
    // advertise 解析失败不应阻断数据面（对端连拒绝 → 段级换源吸收）。
    if (ip.empty() || !to_u64(port_str, &port) || port == 0 || port > 65535) {
        return config;
    }
    in_addr probe{};
    if (::inet_pton(AF_INET, ip.c_str(), &probe) != 1) return config;
    config.bind_address = ip;
    config.listen_port = static_cast<std::uint16_t>(port);
    return config;
}

SwarmDataService::SwarmDataService(SwarmDataServiceConfig config) : config_(std::move(config)) {}

SwarmDataService::~SwarmDataService() { stop(); }

bool SwarmDataService::start(std::string* error) {
    if (running_.load()) return true;

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error) *error = "WSAStartup failed";
        return false;
    }
#endif

    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd == kInvalidSocket) {
        if (error) *error = "socket() failed";
        return false;
    }

    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
                 sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.listen_port);
    if (::inet_pton(AF_INET, config_.bind_address.c_str(), &addr.sin_addr) != 1) {
        close_socket(fd);
        if (error) *error = "invalid bind address: " + config_.bind_address;
        return false;
    }

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(fd);
        if (error) *error = "bind() failed";
        return false;
    }

    // 端口回读（沿 json_rpc_server.cpp:555 先例）：port=0 → OS 分配随机
    // 端口，getsockname 取回真实端口。
    sockaddr_in bound{};
    socket_len_t blen = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &blen) != 0) {
        close_socket(fd);
        if (error) *error = "getsockname() failed";
        return false;
    }
    port_ = ntohs(bound.sin_port);

    if (::listen(fd, 128) != 0) {
        close_socket(fd);
        if (error) *error = "listen() failed";
        return false;
    }

    listen_fd_ = fd;
    stop_requested_.store(false);
    running_.store(true);
    accept_thread_ = std::thread([this] { accept_loop(); });

    FALCON_LOG_INFO_STREAM("P2SP data service listening on " << config_.bind_address << ":"
                                                            << port_);
    return true;
}

void SwarmDataService::stop() {
    if (!running_.exchange(false)) {
        // 即便未运行也保证 accept 线程被回收（start 失败后可能残留）。
        if (accept_thread_.joinable()) accept_thread_.join();
        return;
    }
    stop_requested_.store(true);

    if (listen_fd_ != kInvalidSocket) {
        ::shutdown(listen_fd_, 2 /* SHUT_RDWR */);
        close_socket(listen_fd_);
        listen_fd_ = kInvalidSocket;
    }

    // 唤醒全部会话线程的阻塞 recv：shutdown 存量 fd（不 close，fd 由
    // 各连接线程自身收尾，避免编号复用误关不属于自己的 socket）。
    {
        std::lock_guard<std::mutex> lk(conns_mutex_);
        for (int cfd : conn_fds_) ::shutdown(cfd, 2);
    }

    if (accept_thread_.joinable()) accept_thread_.join();

    // 有界等待会话线程归零（5s 兜底：宁可放走迟到线程的微秒残窗，
    // 也不让析构后仍有无界触碰——沿 2026-10-01 detached 线程收口纪律）。
    std::unique_lock<std::mutex> lk(conns_mutex_);
    conns_cv_.wait_for(lk, std::chrono::seconds(5),
                       [this] { return active_conns_.load() == 0; });
}

void SwarmDataService::register_resource(const std::string& sha256_hex, const std::string& path) {
    std::lock_guard<std::mutex> lk(resources_mutex_);
    resources_[sha256_hex] = path;
}

void SwarmDataService::unregister_resource(const std::string& sha256_hex) {
    std::lock_guard<std::mutex> lk(resources_mutex_);
    resources_.erase(sha256_hex);
}

void SwarmDataService::clear() {
    std::lock_guard<std::mutex> lk(resources_mutex_);
    resources_.clear();
}

std::size_t SwarmDataService::resource_count() const {
    std::lock_guard<std::mutex> lk(resources_mutex_);
    return resources_.size();
}

bool SwarmDataService::lookup(const std::string& sha256_hex, std::string* path) const {
    std::lock_guard<std::mutex> lk(resources_mutex_);
    auto it = resources_.find(sha256_hex);
    if (it == resources_.end()) return false;
    *path = it->second;
    return true;
}

void SwarmDataService::accept_loop() {
    while (!stop_requested_.load()) {
        sockaddr_in peer{};
        socket_len_t plen = sizeof(peer);
        int cfd = static_cast<int>(
            ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen));
        if (cfd == kInvalidSocket) {
            if (stop_requested_.load()) break;
            continue;  // 瞬态 accept 失败（ECONNABORTED 等）继续
        }

        // spawn 前登记台账（spawn 与登记间无窗口——沿 2026-10-01 教训）。
        {
            std::lock_guard<std::mutex> lk(conns_mutex_);
            conn_fds_.insert(cfd);
        }
        active_conns_.fetch_add(1);

        std::thread([this, cfd] {
            handle_connection(cfd);
            // 线程体收尾：最后一次 this 访问是台账注销 + 计数递减。
            {
                std::lock_guard<std::mutex> lk(conns_mutex_);
                conn_fds_.erase(cfd);
                active_conns_.fetch_sub(1);
            }
            conns_cv_.notify_all();
        }).detach();
    }
}

void SwarmDataService::handle_connection(int fd) {
    std::string headers;
    if (!recv_request_headers(fd, &headers)) {
        close_socket(fd);
        return;
    }

    // 解析请求行：METHOD SP TARGET SP HTTP/x.y
    std::istringstream hs(headers);
    std::string method, target, version;
    hs >> method >> target >> version;

    auto respond_status = [&](int status, const char* reason) {
        std::ostringstream resp;
        resp << "HTTP/1.1 " << status << ' ' << reason << "\r\n";
        resp << "Content-Length: 0\r\n";
        resp << "Connection: close\r\n\r\n";
        send_all(fd, resp.str().data(), resp.str().size());
    };

    // 方法白名单：只读 GET/HEAD，其余 405。
    if (method != "GET" && method != "HEAD") {
        respond_status(405, "Method Not Allowed");
        close_socket(fd);
        return;
    }

    // 路径：/by-sha256/<hex>（剥去 query）。
    const std::string prefix = "/by-sha256/";
    std::string path_only = target;
    auto qpos = path_only.find('?');
    if (qpos != std::string::npos) path_only = path_only.substr(0, qpos);

    if (path_only.compare(0, prefix.size(), prefix) != 0) {
        respond_status(404, "Not Found");
        close_socket(fd);
        return;
    }
    std::string hex = path_only.substr(prefix.size());
    if (!is_hex_string(hex)) {
        respond_status(400, "Bad Request");
        close_socket(fd);
        return;
    }

    std::string local_path;
    if (!lookup(hex, &local_path)) {
        respond_status(404, "Not Found");
        close_socket(fd);
        return;
    }

    // 打开成品文件（只读二进制）。成品在注册时已完整落盘（「只服务完整
    // 持有」不变式）；打开失败按 404（文件被移走/删除）。
    std::ifstream in(local_path, std::ios::binary);
    if (!in) {
        respond_status(404, "Not Found");
        close_socket(fd);
        return;
    }
    in.seekg(0, std::ios::end);
    std::uint64_t total = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0, std::ios::beg);

    // 解析 Range（若有）。
    std::uint64_t start = 0;
    std::uint64_t end_excl = total;
    bool has_range = false;
    bool range_unsatisfiable = false;
    {
        const std::string lower = to_lower(headers);
        const std::string key = "\nrange:";
        auto rk = lower.find(key);
        if (rk != std::string::npos) {
            const std::size_t value_start = rk + key.size();
            auto line_end = lower.find("\r\n", value_start);
            if (line_end == std::string::npos) line_end = headers.size();
            std::string value = headers.substr(value_start, line_end - value_start);
            // 去前后空白
            auto b = value.find_first_not_of(" \t");
            auto e = value.find_last_not_of(" \t");
            value = (b == std::string::npos) ? std::string{} : value.substr(b, e - b + 1);
            if (parse_range(value, total, &start, &end_excl)) {
                has_range = true;
            } else {
                range_unsatisfiable = true;
            }
        }
    }

    if (range_unsatisfiable) {
        std::ostringstream resp;
        resp << "HTTP/1.1 416 Range Not Satisfiable\r\n";
        resp << "Content-Range: bytes */" << total << "\r\n";
        resp << "Content-Length: 0\r\n";
        resp << "Connection: close\r\n\r\n";
        send_all(fd, resp.str().data(), resp.str().size());
        close_socket(fd);
        return;
    }

    // 计算本次响应体长度。
    std::uint64_t body_len = (end_excl >= start) ? (end_excl - start) : 0;
    std::uint64_t send_len = (total == 0) ? 0 : body_len;

    std::ostringstream head;
    if (has_range) {
        head << "HTTP/1.1 206 Partial Content\r\n";
        head << "Content-Range: bytes " << start << "-"
             << (end_excl > 0 ? end_excl - 1 : 0) << "/" << total << "\r\n";
    } else {
        head << "HTTP/1.1 200 OK\r\n";
        start = 0;
        send_len = total;
    }
    head << "Accept-Ranges: bytes\r\n";
    head << "Content-Type: application/octet-stream\r\n";
    head << "Content-Length: " << send_len << "\r\n";
    head << "Connection: close\r\n\r\n";
    std::string head_str = head.str();
    if (!send_all(fd, head_str.data(), head_str.size())) {
        close_socket(fd);
        return;
    }
    FALCON_LOG_INFO_STREAM("P2SP data service serving " << hex << " bytes "
                           << start << "-" << (end_excl > 0 ? end_excl - 1 : 0)
                           << "/" << total << " ("
                           << (method == "HEAD" ? "HEAD" : "GET") << ")");

    // HEAD：头之后不发体。
    if (method == "HEAD") {
        served_requests_.fetch_add(1);
        close_socket(fd);
        return;
    }

    // 体：直接文件流（sendfile 语义：只走内核→用户→内核的整块搬运）。
    if (send_len > 0) {
        in.seekg(static_cast<std::streamoff>(start), std::ios::beg);
        constexpr std::size_t kChunk = 64 * 1024;
        std::vector<char> buf(kChunk);
        std::uint64_t remaining = send_len;
        while (remaining > 0) {
            std::size_t want = static_cast<std::size_t>(
                std::min<std::uint64_t>(kChunk, remaining));
            in.read(buf.data(), static_cast<std::streamsize>(want));
            std::streamsize got = in.gcount();
            if (got <= 0) break;  // 文件被截短等异常，提前收口
            if (!send_all(fd, buf.data(), static_cast<std::size_t>(got))) break;
            remaining -= static_cast<std::uint64_t>(got);
        }
    }

    served_requests_.fetch_add(1);
    close_socket(fd);
}

}  // namespace falcon::daemon
