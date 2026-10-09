// ============================================================================
// daemon P2SP 阶段 1 增量 4 真二进制 e2e（SwarmDaemonE2E，POSIX-only）
//
// fork + execv 真二进制（FALCON_SWARMD_BIN / FALCON_DAEMON_BIN 编译定义）：
//   - 规则 (a) 完成→查询→删文件撤销：真 falcon-daemon 从真 HTTP 服务器下载
//     完成（share.enabled=true, hash_delay_s=0）→ 公告到达真 falcon-swarmd
//     → 进程内 SwarmClient query 命中且元数据一致（sha256/name/size + node
//     source node_id == daemon 报的 node_id）→ 删除成品文件 → 文件消失
//     retract → 落空（sources 空）+ 撤销后 announce 再次 query 仍空
//   - 规则 (b) Rendezvous 被杀无感：A/B 两真 daemon 共享一个 Rendezvous，
//     下载中（慢发窗口）SIGKILL rendezvous → 两端下载照常完成，成品逐字节
//     一致（数据面零 Rendezvous 依赖）
//   - 规则 (c) 默认关（share.enabled 缺省 false）：整条链路在默认配置下
//     零公告——本用例断言默认 daemon 的 falcon.swarm.status.enabled == false
//     且 query 落空
//
// 环境隔离：每用例 setenv("HOME", <mkdtemp>) —— daemon 的
// ~/.config/falcon/{swarm_key.pem,tasks.db} 落临时目录不再污染仓库工作树；
// 子进程经 fork/execv 继承环境（home 前缀）。
//
// gcda 铁律：子进程恒 SIGTERM 优雅收尾（SIGKILL 仅兜底），杀 Rendezvous
// 的规则 (b) 用例按「Rendezvous 无覆盖产出」豁免（swarmd 覆盖由自身
// e2e 承担）。
// ============================================================================

// daemon tests 子目录在根 CMakeLists 中先于 swarmd 子目录处理——本目标
// 在 daemon tests CMakeLists 注册（无条件、POSIX-only），宏与 swarm 头
// 路径由根 CMakeLists 在 swarmd 目标之后事后补注入（与 daemon/CLI 的
// FALCON_HAS_SWARM 补链同款形态）。宏缺席（无 CURL / swarmd 未构建）时
// 退化为 skip 占位，套件不假绿也不阻断构建。

#ifdef FALCON_SWARM_E2E_ENABLED

#include "client/swarm_client.hpp"
#include "common/swarm_crypto.hpp"

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#error "This test is POSIX-only (guarded by CMake NOT WIN32)"
#endif

using namespace falcon::swarm;

namespace {

// ---- 子进程管理（沿 swarmd e2e 骨架）-------------------------------------

struct Proc {
    pid_t pid = -1;
    int out_fd = -1;
    int err_fd = -1;
    bool exited = false;
    int exit_status = 0;

    std::string out;
    std::string err;
};

bool proc_start(Proc& p, const char* bin,
                const std::vector<std::string>& args) {
    int out_pipe[2];
    int err_pipe[2];
    if (::pipe(out_pipe) != 0) return false;
    if (::pipe(err_pipe) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]}) {
            ::close(fd);
        }
        return false;
    }

    if (pid == 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::dup2(out_pipe[1], STDOUT_FILENO);
        ::dup2(err_pipe[1], STDERR_FILENO);
        if (out_pipe[1] > STDERR_FILENO) ::close(out_pipe[1]);
        if (err_pipe[1] > STDERR_FILENO) ::close(err_pipe[1]);
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(bin));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(bin, argv.data());
        ::_exit(127);  // exec failed
    }

    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    p.pid = pid;
    p.out_fd = out_pipe[0];
    p.err_fd = err_pipe[0];
    ::fcntl(p.out_fd, F_SETFL, ::fcntl(p.out_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(p.err_fd, F_SETFL, ::fcntl(p.err_fd, F_GETFL) | O_NONBLOCK);
    return true;
}

void drain_fd(int fd, std::string& into, bool& eof) {
    char buf[1024];
    while (true) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            into.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) eof = true;
        break;
    }
}

void poll_drain(Proc& p, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    bool out_eof = false;
    bool err_eof = false;
    while (!out_eof || !err_eof) {
        int remain = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now())
                .count());
        if (remain <= 0) return;
        pollfd fds[2];
        int nfds = 0;
        int out_idx = -1;
        int err_idx = -1;
        if (!out_eof) {
            fds[nfds].fd = p.out_fd;
            fds[nfds].events = POLLIN;
            out_idx = nfds;
            ++nfds;
        }
        if (!err_eof) {
            fds[nfds].fd = p.err_fd;
            fds[nfds].events = POLLIN;
            err_idx = nfds;
            ++nfds;
        }
        const int r = ::poll(fds, static_cast<nfds_t>(nfds), remain);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return;
        }
        if (out_idx >= 0 && (fds[out_idx].revents & (POLLIN | POLLHUP)) != 0) {
            drain_fd(p.out_fd, p.out, out_eof);
        }
        if (err_idx >= 0 && (fds[err_idx].revents & (POLLIN | POLLHUP)) != 0) {
            drain_fd(p.err_fd, p.err, err_eof);
        }
    }
}

bool proc_wait_exit(Proc& p, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const pid_t r = ::waitpid(p.pid, &status, WNOHANG);
        if (r == p.pid) {
            p.exited = true;
            p.exit_status = status;
            poll_drain(p, 300);
            return true;
        }
        poll_drain(p, 50);
    }
    return false;
}

// SIGTERM 优雅收尾为主（gcda 落盘），SIGKILL 仅兜底防挂死
void proc_terminate(Proc& p) {
    if (!p.exited && p.pid > 0) {
        ::kill(p.pid, SIGTERM);
        if (!proc_wait_exit(p, 10000)) {
            ::kill(p.pid, SIGKILL);
            int status = 0;
            ::waitpid(p.pid, &status, 0);
            p.exited = true;
            p.exit_status = status;
        }
    }
    if (p.out_fd >= 0) ::close(p.out_fd);
    if (p.err_fd >= 0) ::close(p.err_fd);
}

// 硬杀（规则 b：Rendezvous 进程崩溃模拟，flush 豁免）
void proc_kill9(Proc& p) {
    if (!p.exited && p.pid > 0) {
        ::kill(p.pid, SIGKILL);
        int status = 0;
        ::waitpid(p.pid, &status, 0);
        p.exited = true;
        p.exit_status = status;
    }
    if (p.out_fd >= 0) {
        ::close(p.out_fd);
        p.out_fd = -1;
    }
    if (p.err_fd >= 0) {
        ::close(p.err_fd);
        p.err_fd = -1;
    }
}

// ---- 临时目录 / 环境 ------------------------------------------------------

std::string make_temp_dir(const char* tag) {
    std::string tmpl = std::string("/tmp/") + tag + "_XXXXXX";
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    EXPECT_NE(::mkdtemp(buf.data()), nullptr);
    return std::string(buf.data());
}

std::string write_temp_file_at(const std::string& dir, const char* name,
                               const std::string& content) {
    const std::string path = dir + "/" + name;
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    EXPECT_GE(fd, 0);
    if (fd < 0) return {};
    const ssize_t n = ::write(fd, content.data(), content.size());
    EXPECT_EQ(static_cast<std::size_t>(n), content.size());
    ::close(fd);
    return path;
}

std::string read_file_if_exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int exit_code_of(const Proc& p) {
    if (!p.exited) return -1;
    return WIFEXITED(p.exit_status) ? WEXITSTATUS(p.exit_status) : -1;
}

// ---- 端口 / 连接 ----------------------------------------------------------

std::uint16_t pick_free_port() {
    int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(listener, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    EXPECT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&addr),
                     sizeof(addr)), 0);
    EXPECT_EQ(::listen(listener, 1), 0);
    socklen_t len = sizeof(addr);
    EXPECT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&addr),
                            &len), 0);
    const std::uint16_t port = ntohs(addr.sin_port);
    ::close(listener);
    return port;
}

bool tcp_connect(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    const int rc = ::connect(fd, reinterpret_cast<sockaddr*>(&addr),
                             sizeof(addr));
    ::close(fd);
    return rc == 0;
}

template <typename Pred>
bool wait_until(Pred pred, int timeout_ms, int poll_ms = 20) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(poll_ms));
    }
    return false;
}

// ---- 极简 HTTP 下载服务器（固定 body 或多路径表；HEAD + GET + Range）------

// 请求记录（V2 分段是否携带 Range 的证据链）
struct RequestLog {
    std::string method;
    std::string path;
    std::string range;
};

class HttpMirror {
public:
    // 构造即监听 127.0.0.1 随机端口
    bool start(const std::string& body) {
        body_ = body;
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listener_ < 0) return false;
        int one = 1;
        ::setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0) {
            return false;
        }
        if (::listen(listener_, 8) != 0) return false;
        socklen_t len = sizeof(addr);
        ::getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        accept_ = std::thread([this] { accept_loop(); });
        return true;
    }

    ~HttpMirror() {
        stop();
    }

    void stop() {
        if (listener_ >= 0) {
            ::shutdown(listener_, SHUT_RDWR);
            ::close(listener_);
            listener_ = -1;
        }
        if (accept_.joinable()) accept_.join();
        // 连接线程必须 join（2026-10-01 detached 线程教训——栈上对象析构
        // 先于 detached 线程退出即踩已死栈帧）
        for (auto& t : conns_) {
            if (t.joinable()) t.join();
        }
        conns_.clear();
    }

    std::uint16_t port() const { return port_; }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/payload.bin";
    }

    // 多路径表（任一 add_path 后进入多路径模式：按 path 服务、缺 path 404、
    // Range 忠实 206 切片 / 越界 416；表为空保持固定 body 形态零变化）。
    // 须在产生请求之前调用（无内部请求排队）。
    void add_path(const std::string& path, const std::string& body) {
        std::lock_guard<std::mutex> lock(paths_mu_);
        paths_[path] = body;
    }

    // 请求留痕（method/path/Range）——断言在已收到的快照上，锁内拷贝
    std::vector<RequestLog> requests() const {
        std::lock_guard<std::mutex> lock(log_mu_);
        return requests_;
    }

    // 规则 (b)：慢发拉开下载窗口（每 chunk 字节间歇 ms），让"下载进行中"
    // 与"杀 Rendezvous"可排序
    void set_slow(std::size_t chunk, int ms) {
        slow_chunk_ = chunk;
        slow_ms_ = ms;
    }

    // 规则 (d)：该 path 的 Range GET 一律回 416（plain GET 照常 200 +
    // Accept-Ranges: bytes + 全量 body）。判别器形态——镜像对 Range 撒谎
    // （HEAD 宣称 bytes、GET 拒绝切片）：V2 分段连接必须换源重试才能收
    // 满；V1 串行回退（SegmentDownloader）发 Range 必败不可能假完成
    void refuse_range(const std::string& path) {
        std::lock_guard<std::mutex> lock(paths_mu_);
        range_refusers_.insert(path);
    }

private:
    // 请求行 path（首行第二个空格分段）
    static std::string request_path(const std::string& head) {
        const auto eol = head.find("\r\n");
        const std::string line =
            head.substr(0, eol == std::string::npos ? head.size() : eol);
        const auto sp1 = line.find(' ');
        if (sp1 == std::string::npos) return "";
        const auto sp2 = line.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return "";
        return line.substr(sp1 + 1, sp2 - sp1 - 1);
    }

    // 头值查找（RFC 9110 §5.1：头名大小写不敏感；两侧空白裁剪）
    static std::string header_value(const std::string& head,
                                    const std::string& name) {
        std::size_t pos = head.find("\r\n");
        if (pos == std::string::npos) return "";
        pos += 2;
        while (pos < head.size()) {
            const auto eol = head.find("\r\n", pos);
            if (eol == std::string::npos || eol == pos) break;
            const std::string line = head.substr(pos, eol - pos);
            pos = eol + 2;
            const auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            for (auto& c : key) {
                c = static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c)));
            }
            if (key != name) continue;
            const std::string val = line.substr(colon + 1);
            const auto first = val.find_first_not_of(" \t");
            if (first == std::string::npos) return "";
            const auto last = val.find_last_not_of(" \t");
            return val.substr(first, last - first + 1);
        }
        return "";
    }

    // 仅认 "bytes=S-" 与 "bytes=S-E"（后缀区间不支持——e 计算处 stoull 空
    // 串抛出即 false）；s>=total 或 e<s → false（调用方回 416）；e 截到
    // total-1
    static bool parse_range(const std::string& v, std::size_t total,
                            std::size_t& off, std::size_t& end) {
        const std::string pfx = "bytes=";
        if (v.size() < pfx.size() || v.compare(0, pfx.size(), pfx) != 0) {
            return false;
        }
        std::size_t i = pfx.size();
        std::size_t s = 0;
        bool got_s = false;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            s = s * 10 + static_cast<std::size_t>(v[i] - '0');
            ++i;
            got_s = true;
        }
        if (!got_s || i >= v.size() || v[i] != '-') return false;
        ++i;
        std::size_t e = 0;
        bool got_e = false;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
            e = e * 10 + static_cast<std::size_t>(v[i] - '0');
            ++i;
            got_e = true;
        }
        if (i != v.size()) return false;
        if (s >= total) return false;
        if (got_e) {
            if (e < s) return false;
            if (e >= total) e = total - 1;
        } else {
            e = total - 1;  // 开放区间 "bytes=S-" 到末尾
        }
        off = s;
        end = e;
        return true;
    }

    void accept_loop() {
        while (true) {
            int conn = ::accept(listener_, nullptr, nullptr);
            if (conn < 0) {
                if (errno == EINTR) continue;
                break;  // shutdown 唤醒收尾
            }
            conns_.emplace_back([this, conn] { serve(conn); });
        }
    }

    void serve(int conn) {
        char buf[4096];
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            const ssize_t n = ::recv(conn, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(conn);
                return;
            }
            head.append(buf, static_cast<std::size_t>(n));
        }
        const bool is_head = head.rfind("HEAD ", 0) == 0;
        const std::string path = request_path(head);
        const std::string range_hdr = header_value(head, "range");
        {
            std::lock_guard<std::mutex> lock(log_mu_);
            requests_.push_back({is_head ? "HEAD" : "GET", path, range_hdr});
        }

        // 路由：多路径表非空按 path 服务（缺 path 404）；表为空保持
        // 固定 body 形态（任意 path 回 body_，Accept-Ranges: none）
        std::string body;
        bool multi = false;
        {
            std::lock_guard<std::mutex> lock(paths_mu_);
            multi = !paths_.empty();
            if (multi) {
                const auto it = paths_.find(path);
                if (it == paths_.end()) {
                    static const char k404[] =
                        "HTTP/1.1 404 Not Found\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
                    const ssize_t n =
                        ::send(conn, k404, sizeof(k404) - 1, MSG_NOSIGNAL);
                    (void)n;
                    ::close(conn);
                    return;
                }
                body = it->second;
            } else {
                body = body_;
            }
        }

        // Range 解析（仅多路径模式——固定 body 形态不带 Accept-Ranges: bytes）
        std::size_t off = 0;
        std::size_t len = body.size();
        bool ranged = false;
        bool refuse = false;
        if (multi) {
            std::lock_guard<std::mutex> lock(paths_mu_);
            refuse = range_refusers_.count(path) > 0;
        }
        if (multi && !range_hdr.empty()) {
            std::size_t s = 0;
            std::size_t e = 0;
            if (!refuse && parse_range(range_hdr, body.size(), s, e)) {
                off = s;
                len = e - s + 1;
                ranged = true;
            } else {
                const std::string r416 =
                    "HTTP/1.1 416 Range Not Satisfiable\r\n"
                    "Content-Range: bytes */" +
                    std::to_string(body.size()) +
                    "\r\nContent-Length: 0\r\n"
                    "Connection: close\r\n\r\n";
                const ssize_t n =
                    ::send(conn, r416.data(), r416.size(), MSG_NOSIGNAL);
                (void)n;
                ::close(conn);
                return;
            }
        }

        std::string resp;
        if (ranged) {
            // 206 切片：Content-Range 回显真实区间
            resp = "HTTP/1.1 206 Partial Content\r\n";
            resp += "Content-Length: " + std::to_string(len) + "\r\n";
            resp += "Content-Range: bytes " + std::to_string(off) + "-" +
                    std::to_string(off + len - 1) + "/" +
                    std::to_string(body.size()) + "\r\n";
            resp += "Accept-Ranges: bytes\r\n";
            resp += "Connection: close\r\n\r\n";
        } else {
            resp = "HTTP/1.1 200 OK\r\n";
            resp += "Content-Length: " + std::to_string(len) + "\r\n";
            resp += std::string("Accept-Ranges: ") +
                    (multi ? "bytes" : "none") + "\r\n";
            resp += "Connection: close\r\n\r\n";
        }
        const ssize_t n0 =
            ::send(conn, resp.data(), resp.size(), MSG_NOSIGNAL);
        (void)n0;
        if (is_head) {
            ::close(conn);
            return;
        }
        // 头 + body 分开发送：body 按 slow 节奏逐块慢发
        std::size_t done = 0;
        while (done < len) {
            const std::size_t take = std::min(slow_chunk_, len - done);
            const ssize_t n = ::send(conn, body.data() + off + done, take,
                                     MSG_NOSIGNAL);
            if (n <= 0) break;
            done += static_cast<std::size_t>(n);
            if (slow_ms_ > 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(slow_ms_));
            }
        }
        ::close(conn);
    }

    int listener_ = -1;
    std::uint16_t port_ = 0;
    std::string body_;
    std::map<std::string, std::string> paths_;
    std::set<std::string> range_refusers_;
    mutable std::mutex paths_mu_;
    mutable std::mutex log_mu_;
    std::vector<RequestLog> requests_;
    std::thread accept_;
    std::vector<std::thread> conns_;
    // 默认整包直发（take = min(slow_chunk_, remaining)：初版默认 0 会让
    // send(fd, ptr, 0) 返回 0 被当断连 → body 永不发出，未设 set_slow 的
    // 用例（A/C）下载必 error）
    std::size_t slow_chunk_ = static_cast<std::size_t>(-1);
    int slow_ms_ = 0;
};

// ---- JSON-RPC 调用（裸 HTTP POST /jsonrpc）--------------------------------

nlohmann::json rpc_call(std::uint16_t port, const std::string& method,
                        const nlohmann::json& params) {
    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", 1},
        {"method", method},
        {"params", params},
    };
    const std::string body = req.dump();
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    EXPECT_GE(fd, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    EXPECT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)),
              0);

    std::string http = "POST /jsonrpc HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    http += "Content-Type: application/json\r\nConnection: close\r\n";
    http += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    http += body;
    std::size_t off = 0;
    while (off < http.size()) {
        const ssize_t n = ::send(fd, http.data() + off, http.size() - off, 0);
        if (n <= 0) break;
        off += static_cast<std::size_t>(n);
    }
    std::string resp;
    char tmp[4096];
    while (true) {
        const ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        resp.append(tmp, static_cast<std::size_t>(n));
    }
    ::close(fd);
    const auto sep = resp.find("\r\n\r\n");
    if (sep == std::string::npos) return nlohmann::json::object();
    try {
        return nlohmann::json::parse(resp.substr(sep + 4));
    } catch (const std::exception&) {
        return nlohmann::json::object();
    }
}

// ---- 任务终态等待（gid 寻址）-----------------------------------------------
//
// 「tellActive 为空」在 addUri 返回与任务进活跃队列之间的窗口恒真——
// 轮询起点太早即把未开始的下载当已完成（首版 Tests A/C 即因此断言空
// 文件）。以 addUri 返回的 gid 轮询 tellStatus 到终态才是确定性完成判据。
// 返回终态串（"complete"/"error"/"removed"），超时返回 ""。
std::string wait_task_terminal(std::uint16_t port, const std::string& gid,
                               int timeout_ms) {
    std::string status;
    const bool reached = wait_until(
        [&] {
            auto r = rpc_call(port, "aria2.tellStatus",
                              nlohmann::json::array({gid}));
            if (!r.contains("result")) return false;
            status = r["result"].value("status", std::string());
            return status == "complete" || status == "error" ||
                   status == "removed";
        },
        timeout_ms);
    return reached ? status : std::string();
}

// ---- 内存身份（e2e 不落盘）------------------------------------------------

SwarmKeyMaterial memory_key() {
    SwarmKeyMaterial m;
    auto kp = SwarmCrypto::generate_keypair();
    m.private_seed = std::move(kp.private_seed);
    m.public_key_der = std::move(kp.public_key_der);
    m.node_id = SwarmCrypto::fingerprint(m.public_key_der);
    m.pubkey_hex = SwarmCrypto::bytes_to_hex(m.public_key_der.data(),
                                             m.public_key_der.size());
    return m;
}

// ---- 可复用夹具：Rendezvous + daemon 配置 ---------------------------------

// 写 daemon.json：p2sp.share.enabled + rendezvous + rpc。
// ttl_s 越小续租/消失检查越快（renewal = clamp(ttl/3, poll, cap)）——
// 规则 (a) 用 9s（≈3s retract 周期）避免 30s 等待依赖默认 1d TTL
std::string write_daemon_conf(const std::string& dir, const char* name,
                              bool share_enabled, std::uint16_t rdv_port,
                              const std::string& server_token,
                              const std::string& group_token,
                              long ttl_s = 86400,
                              const std::string& advertise_addr = "",
                              const std::string& http_engine = "") {
    nlohmann::json conf;
    conf["rpc"] = {{"enabled", true},
                   {"host", "127.0.0.1"},
                   {"port", 0}};
    conf["p2sp"]["share"] = {{"enabled", share_enabled},
                             {"mode", "standard"},
                             {"ttl_s", ttl_s},
                             {"hash_delay_s", 0},
                             {"announce_mirrors", false}};
    conf["p2sp"]["rendezvous"] = {{"host", "127.0.0.1"},
                                  {"port", static_cast<int>(rdv_port)},
                                  {"server_token", server_token},
                                  {"group_token", group_token}};
    // 缺省参数不写键——既有用例的配置 JSON 逐键不变
    if (!advertise_addr.empty()) {
        conf["p2sp"]["rendezvous"]["advertise_addr"] = advertise_addr;
    }
    if (!http_engine.empty()) {
        conf["download"]["http_engine"] = http_engine;
    }
    return write_temp_file_at(dir, name, conf.dump(2));
}

}  // namespace

// ============================================================================
// 规则 (a)：完成 → 查询命中(元数据一致) → 删文件 → 落空
// ============================================================================
TEST(SwarmDaemonE2E, AnnounceQueryThenDeleteRetracts) {
    const std::string home = make_temp_dir("falcon_daemon_e2e_a");
    ASSERT_FALSE(home.empty());
    ::setenv("HOME", home.c_str(), 1);

    const std::string body = "Falcon P2SP daemon e2e payload A\n";
    const std::string expect_sha = SwarmCrypto::sha256_hex(body);

    HttpMirror mirror;
    ASSERT_TRUE(mirror.start(body));

    const std::uint16_t rdv_port = pick_free_port();
    const std::string server_token = "tok-a";
    const std::string group_token = "gtok-a";

    // 真 falcon-swarmd（Rendezvous）——端口可连即就绪（stdout 管道全缓冲，
    // banner 探测不可靠）
    Proc rdv;
    ASSERT_TRUE(proc_start(rdv, FALCON_SWARMD_BIN,
                           {"--no-conf", "--swarm-host", "127.0.0.1",
                            "--swarm-port", std::to_string(rdv_port),
                            "--server-token", server_token,
                            "--group-token", group_token}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(rdv_port); }, 10000));

    // 真 falcon-daemon（share.enabled + rendezvous 指向上面）
    const std::string dir = home + "/dl";
    ::mkdir(dir.c_str(), 0755);
    const std::string conf = write_daemon_conf(
        home, "daemon_a.json", true, rdv_port, server_token, group_token,
        /*ttl_s=*/9);
    ASSERT_FALSE(conf.empty());

    const std::uint16_t daemon_port = pick_free_port();
    Proc daemon;
    // 注意：--no-conf 会连显式 --conf-path 一并忽略——必须不带 --no-conf
    ASSERT_TRUE(proc_start(
        daemon, FALCON_DAEMON_BIN,
        {"--enable-rpc", "--rpc-listen-host", "127.0.0.1",
         "--rpc-listen-port", std::to_string(daemon_port), "--task-db",
         home + "/tasks_a.db", "--conf-path", conf}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(daemon_port); }, 15000));

    // 等 daemon 侧 SwarmAnnouncer 注册进 Rendezvous
    auto status_registered = [&] {
        auto r = rpc_call(daemon_port, "falcon.swarm.status",
                          nlohmann::json::array());
        return r.value("result", nlohmann::json::object())
                   .value("registered", false);
    };
    ASSERT_TRUE(wait_until(status_registered, 20000))
        << "daemon never registered; stderr=" << daemon.err;

    auto status0 = rpc_call(daemon_port, "falcon.swarm.status",
                            nlohmann::json::array())["result"];
    const std::string daemon_node_id = status0.value("node_id", std::string());
    ASSERT_FALSE(daemon_node_id.empty());
    EXPECT_TRUE(status0.value("enabled", false));

    // 触发下载（HTTP GET → 完成后公告）
    const std::string out_path = dir + "/payload.bin";
    auto add = rpc_call(daemon_port, "aria2.addUri",
                        nlohmann::json::array(
                            {nlohmann::json::array({mirror.url()}),
                             nlohmann::json{{"dir", dir},
                                            {"allow-overwrite", "true"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();
    const std::string gid = add["result"].get<std::string>();
    ASSERT_EQ(wait_task_terminal(daemon_port, gid, 20000), "complete")
        << "daemon.err=" << daemon.err;
    ASSERT_EQ(read_file_if_exists(out_path), body);

    // Rendezvous 侧：等公告到达（用独立节点 SwarmClient query 命中）
    auto cfg = SwarmClientConfig{};
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(rdv_port);
    cfg.server_token = server_token;
    cfg.group_token = group_token;
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    cfg.reconnect_delay = std::chrono::milliseconds(100);
    SwarmClient qnode(cfg, memory_key());
    std::string qerr;
    ASSERT_TRUE(qnode.start(&qerr)) << qerr;

    // rdv 默认 query 限频 120/min（§9.1）——20ms 轮询 2.4s 即烧穿预算，
    // 此后本 key 恒 -32002，查询永不稳定 ok。600ms 间隔 ≤100/min 保持在限内
    std::string qnode_last_err;
    auto query_hit = [&] {
        nlohmann::json result;
        auto err = qnode.query(expect_sha, &result);
        if (!err.ok()) {
            qnode_last_err =
                std::to_string(err.code) + ": " + err.message;
            return false;
        }
        const auto srcs = result.value("sources", nlohmann::json::array());
        return !srcs.empty();
    };
    ASSERT_TRUE(wait_until(query_hit, 20000, 600))
        << "announcement never reached rendezvous; last query err: "
        << qnode_last_err;

    // 元数据一致性：sha256 / name / size / node source node_id
    nlohmann::json hit;
    ASSERT_TRUE(qnode.query(expect_sha, &hit).ok());
    EXPECT_EQ(hit.value("sha256", std::string()), expect_sha);
    EXPECT_EQ(hit.value("name", std::string()), "payload.bin");
    EXPECT_EQ(hit.value("size", std::uint64_t(0)),
              static_cast<std::uint64_t>(body.size()));
    const auto& srcs = hit["sources"];
    ASSERT_FALSE(srcs.empty());
    EXPECT_EQ(srcs[0].value("type", std::string()), "node");
    EXPECT_EQ(srcs[0].value("node_id", std::string()), daemon_node_id);

    // 删文件 → SwarmAnnouncer retract → 落空
    ASSERT_EQ(::unlink(out_path.c_str()), 0);
    auto query_empty = [&] {
        nlohmann::json result;
        auto err = qnode.query(expect_sha, &result);
        if (!err.ok()) {
            qnode_last_err =
                std::to_string(err.code) + ": " + err.message;
            return false;
        }
        return result.value("sources", nlohmann::json::array()).empty();
    };
    EXPECT_TRUE(wait_until(query_empty, 30000, 600))
        << "retract after delete never propagated; last query err: "
        << qnode_last_err;

    qnode.stop();
    proc_terminate(daemon);
    proc_terminate(rdv);
    EXPECT_EQ(exit_code_of(daemon), 0) << daemon.err;
    EXPECT_EQ(exit_code_of(rdv), 0) << rdv.err;
}

// ============================================================================
// 规则 (d)：metalink V2 桥接把 swarm 节点源拉入镜像池（P2SP 阶段 2 验收 #1）
//
// A（share on + advertise_addr）从单镜像完成 8MB 下载 → 公告节点源（入站
// 数据服务绑 advertise 端口）；B（share off + http_engine=v2）经「单镜像
// metalink 文档」下载同一内容——V2 门禁放行后池子只有 1 条文档镜像，
// collect_swarm_sources 查询补入 A 的节点源凑满 2 条走多段轮转。
// 文档镜像 plain GET 忠实应答、Range GET 一律 416（416-refuser 判别器）：
// 多段轮转下必有段落在文档镜像上吃到 416 后换源到 swarm 数据服务——
// 「doc 日志同时出现 plain GET 与 Range GET」+ 成品逐字节一致 +
// 整文件哈希通过 = swarm 源真实承载了数据段（V1 串行回落不会对单镜像
// 发 Range GET；announce_mirrors=false 时池中除 swarm 外别无其他源）。
// ============================================================================
TEST(SwarmDaemonE2E, MetalinkV2BridgePullsSwarmSourceIntoMirrorPool) {
    std::string payload(8 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('A' + (i % 26));
    }
    const std::string expect_sha = SwarmCrypto::sha256_hex(payload);

    // A 的源镜像：legacy 形态（任意 path → 200 全量 + Accept-Ranges: none）
    HttpMirror mirror_a;
    ASSERT_TRUE(mirror_a.start(payload));

    // B 的文档镜像：多路径形态；payload 忠实应答但拒绝 Range（416）
    HttpMirror mirror_doc;
    ASSERT_TRUE(mirror_doc.start(payload));
    const std::uint16_t doc_port = mirror_doc.port();
    const std::string doc_payload_url =
        "http://127.0.0.1:" + std::to_string(doc_port) + "/payload.bin";
    const std::string meta4_url =
        "http://127.0.0.1:" + std::to_string(doc_port) + "/meta4/test.meta4";
    const std::string xml =
        std::string("<?xml version=\"1.0\"?>"
                    "<metalink xmlns=\"urn:ietf:params:xml:ns:metalink\">"
                    "<file name=\"payload.bin\"><size>8388608</size>"
                    "<hash type=\"sha256\">") +
        expect_sha + "</hash><url>" + doc_payload_url +
        "</url></file></metalink>";
    mirror_doc.add_path("/payload.bin", payload);
    mirror_doc.add_path("/meta4/test.meta4", xml);
    mirror_doc.refuse_range("/payload.bin");

    const std::uint16_t adv_port = pick_free_port();
    const std::uint16_t rdv_port = pick_free_port();
    const std::string server_token = "tok-d";
    const std::string group_token = "gtok-d";
    const std::string adv_addr = "127.0.0.1:" + std::to_string(adv_port);

    // 注册限频 per-IP 按 RPC 调用计（两步挑战 step1+step2 各一次）；本测试
    // 单 IP 上有 3 个注册客户端（A 公告面 + 测试侧探针 + B 查询面）= 6 次
    // register 调用 > 默认 5/min，B 的 step2 必吃 -32002。注册限频语义
    // 由 swarmd 单测覆盖，e2e 只取够用的值（30）。
    Proc rdv;
    ASSERT_TRUE(proc_start(rdv, FALCON_SWARMD_BIN,
                           {"--no-conf", "--swarm-host", "127.0.0.1",
                            "--swarm-port", std::to_string(rdv_port),
                            "--server-token", server_token,
                            "--group-token", group_token,
                            "--rate-register-per-min", "30"}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(rdv_port); }, 10000));

    // ---- A：share on + advertise_addr → 完成后公告节点源 ----
    const std::string home_a = make_temp_dir("falcon_daemon_e2e_d");
    ASSERT_FALSE(home_a.empty());
    ::setenv("HOME", home_a.c_str(), 1);
    const std::string dir_a = home_a + "/dl_a";
    ::mkdir(dir_a.c_str(), 0755);
    const std::string conf_a = write_daemon_conf(
        home_a, "daemon_da.json", /*share_enabled=*/true, rdv_port,
        server_token, group_token, /*ttl_s=*/86400, adv_addr);
    ASSERT_FALSE(conf_a.empty());

    const std::uint16_t port_a = pick_free_port();
    Proc daemon_a;
    ASSERT_TRUE(proc_start(daemon_a, FALCON_DAEMON_BIN,
                           {"--enable-rpc", "--rpc-listen-host", "127.0.0.1",
                            "--rpc-listen-port", std::to_string(port_a),
                            "--task-db", home_a + "/tasks_d_a.db",
                            "--conf-path", conf_a}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port_a); }, 15000));

    const auto status_registered_a = [&] {
        const auto r = rpc_call(port_a, "falcon.swarm.status",
                                nlohmann::json::array());
        return r.value("result", nlohmann::json::object())
            .value("registered", false);
    };
    ASSERT_TRUE(wait_until(status_registered_a, 20000))
        << "daemon_a never registered; stderr=" << daemon_a.err;
    const auto status_a =
        rpc_call(port_a, "falcon.swarm.status", nlohmann::json::array());
    ASSERT_TRUE(status_a.contains("result")) << status_a.dump();
    const std::string daemon_node_id_a =
        status_a["result"].value("node_id", std::string());
    ASSERT_FALSE(daemon_node_id_a.empty());

    const auto add_a = rpc_call(
        port_a, "aria2.addUri",
        nlohmann::json::array(
            {nlohmann::json::array({mirror_a.url()}),
             nlohmann::json{{"dir", dir_a}, {"allow-overwrite", "true"}}}));
    ASSERT_TRUE(add_a.contains("result")) << add_a.dump();
    const std::string gid_a = add_a["result"].get<std::string>();
    ASSERT_EQ(wait_task_terminal(port_a, gid_a, 60000), "complete")
        << "daemon_a.err=" << daemon_a.err;
    ASSERT_EQ(read_file_if_exists(dir_a + "/payload.bin"), payload);

    // 公告可见之后才让 B 开工——否则 B 首查落空池子=1 走串行回落，判别器失效
    auto cfg = SwarmClientConfig{};
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(rdv_port);
    cfg.server_token = server_token;
    cfg.group_token = group_token;
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    cfg.reconnect_delay = std::chrono::milliseconds(100);
    SwarmClient qnode(cfg, memory_key());
    std::string qerr;
    ASSERT_TRUE(qnode.start(&qerr)) << qerr;
    std::string qnode_last_err;
    nlohmann::json announced;
    const auto query_hit = [&] {
        nlohmann::json hit;
        const auto err = qnode.query(expect_sha, &hit);
        if (!err.ok()) {
            qnode_last_err = std::to_string(err.code) + ": " + err.message;
            return false;
        }
        if (!hit.contains("sources") || hit["sources"].empty()) {
            return false;
        }
        announced = hit;
        return true;
    };
    // rdv 默认 query 限频 120/min（§9.1）——20ms 轮询 2.4s 即烧穿预算，
    // 此后本 key 恒 -32002，查询永不稳定 ok。600ms 间隔 ≤100/min 保持在限内
    ASSERT_TRUE(wait_until(query_hit, 30000, 600))
        << "announcement never reached rendezvous; last query err: "
        << qnode_last_err;
    const auto& srcs = announced["sources"];
    ASSERT_FALSE(srcs.empty());
    EXPECT_EQ(srcs[0].value("type", std::string()), "node");
    EXPECT_EQ(srcs[0].value("node_id", std::string()), daemon_node_id_a);
    ASSERT_TRUE(srcs[0].contains("advertise")) << srcs[0].dump();
    EXPECT_EQ(srcs[0]["advertise"].value("addr", std::string()), adv_addr);
    EXPECT_EQ(srcs[0]["advertise"].value("direct", false), true);

    // A 的入站数据服务必须真的在 advertise 端口监听（公告内容 ≠ 数据面
    // 在位；这里直接探活，缺席即结构性失败，不等 B 侧下载超时归因）
    ASSERT_TRUE(wait_until([&] { return tcp_connect(adv_port); }, 10000))
        << "daemon_a data service never listened on " << adv_addr
        << "; daemon_a.out=" << daemon_a.out;

    // ---- B：share off + http_engine=v2 → 单镜像 metalink + swarm 补池 ----
    const std::string home_b = make_temp_dir("falcon_daemon_e2e_d_b");
    ASSERT_FALSE(home_b.empty());
    ::setenv("HOME", home_b.c_str(), 1);
    const std::string dir_b = home_b + "/dl_b";
    ::mkdir(dir_b.c_str(), 0755);
    const std::string conf_b = write_daemon_conf(
        home_b, "daemon_db.json", /*share_enabled=*/false, rdv_port,
        server_token, group_token, /*ttl_s=*/86400, /*advertise_addr=*/"",
        /*http_engine=*/"v2");
    ASSERT_FALSE(conf_b.empty());

    const std::uint16_t port_b = pick_free_port();
    Proc daemon_b;
    ASSERT_TRUE(proc_start(daemon_b, FALCON_DAEMON_BIN,
                           {"--enable-rpc", "--rpc-listen-host", "127.0.0.1",
                            "--rpc-listen-port", std::to_string(port_b),
                            "--task-db", home_b + "/tasks_d_b.db",
                            "--conf-path", conf_b}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(port_b); }, 15000));

    const auto add_b = rpc_call(
        port_b, "aria2.addUri",
        nlohmann::json::array(
            {nlohmann::json::array({meta4_url}),
             nlohmann::json{{"dir", dir_b}, {"allow-overwrite", "true"}}}));
    ASSERT_TRUE(add_b.contains("result")) << add_b.dump();
    const std::string gid_b = add_b["result"].get<std::string>();
    if (wait_task_terminal(port_b, gid_b, 120000) != "complete") {
        // 诊断面：daemon 日志分流（logger console sink：WARN 及以上 →
        // stderr，INFO 及以下 → stdout），失败时两侧日志都倾倒——
        // P2SP 查询面的退避/失败 WARN 全在 err 侧，只 dump out 会漏。
        auto tail = [](const std::string& s, std::size_t n) {
            return s.size() <= n ? s : s.substr(s.size() - n);
        };
        std::string reqs;
        for (const auto& r : mirror_doc.requests()) {
            reqs += "  " + r.method + " " + r.path +
                    (r.range.empty() ? "" : " Range=" + r.range) + "\n";
        }
        // out 只在 poll_drain（proc_wait_exit 路径）时从管道填充——
        // 打印前先排空，否则 64KB 管道里的日志被误读为空
        poll_drain(daemon_b, 300);
        poll_drain(daemon_a, 300);
        ADD_FAILURE() << "task_b not complete; adv_port_listening="
                      << tcp_connect(adv_port)
                      << "\n--- daemon_b.err tail ---\n"
                      << tail(daemon_b.err, 8000)
                      << "\n--- daemon_b.out tail ---\n"
                      << tail(daemon_b.out, 20000)
                      << "\n--- daemon_a.err tail ---\n"
                      << tail(daemon_a.err, 4000)
                      << "\n--- daemon_a.out tail ---\n"
                      << tail(daemon_a.out, 3000)
                      << "\n--- mirror_doc requests ---\n" << reqs;
        return;
    }

    ASSERT_EQ(read_file_if_exists(dir_b + "/payload.bin"), payload);

    // 416-refuser 判别器：doc 日志必须同时出现 plain GET（初始连接=段 0）
    // 与 Range GET（轮转段被 416 后换源到 swarm）——Range GET 只在
    // V2 多段轮转下发生，串行回落形态不可能出现
    std::size_t plain_gets = 0;
    std::size_t range_gets = 0;
    for (const auto& r : mirror_doc.requests()) {
        if (r.method == "GET" && r.path == "/payload.bin") {
            if (r.range.empty()) {
                ++plain_gets;
            } else {
                ++range_gets;
            }
        }
    }
    EXPECT_GE(plain_gets, 1u);
    EXPECT_GE(range_gets, 1u);

    // 无临时残留（.falcon.tmp / .falcon.ctrl / metalink part 均已收口）
    if (DIR* d = ::opendir(dir_b.c_str())) {
        while (const dirent* e = ::readdir(d)) {
            const std::string name = e->d_name;
            EXPECT_EQ(name.find(".falcon."), std::string::npos) << name;
        }
        ::closedir(d);
    }

    qnode.stop();
    proc_terminate(daemon_a);
    proc_terminate(daemon_b);
    proc_terminate(rdv);
    EXPECT_EQ(exit_code_of(daemon_a), 0) << daemon_a.err;
    EXPECT_EQ(exit_code_of(daemon_b), 0) << daemon_b.err;
    EXPECT_EQ(exit_code_of(rdv), 0) << rdv.err;
}

// ============================================================================
// 规则 (b)：Rendezvous 进程被杀，A/B 下载任务（进行中）无感
// ============================================================================
TEST(SwarmDaemonE2E, RendezvousKilledMidDownloadIsTransparent) {
    const std::string home_a = make_temp_dir("falcon_daemon_e2e_b_a");
    ASSERT_FALSE(home_a.empty());

    const std::uint16_t rdv_port_num = pick_free_port();
    const std::string rdv_port = std::to_string(rdv_port_num);
    const std::string server_token = "tok-b";
    const std::string group_token = "gtok-b";

    Proc rdv;
    ASSERT_TRUE(proc_start(
        rdv, FALCON_SWARMD_BIN,
        {"--no-conf", "--swarm-host", "127.0.0.1", "--swarm-port", rdv_port,
         "--server-token", server_token, "--group-token", group_token}));
    ASSERT_TRUE(
        wait_until([&] { return tcp_connect(rdv_port_num); }, 10000));

    const std::string body(256 * 1024, 'B');  // 256 KiB
    HttpMirror mirror;
    ASSERT_TRUE(mirror.start(body));
    // 慢发拉开下载窗（256KiB / 8KB = 32 块 × 40ms ≈ 1.3s），确保
    // "进行中" 与 "杀 Rendezvous" 时序可排序
    mirror.set_slow(8 * 1024, 40);

    // A 与 B 各自独立 HOME + task db（模拟两台节点）
    struct NodeProc {
        std::string home;
        Proc proc;
        std::string dir;
        std::string out;
    };
    NodeProc a, b;
    a.home = home_a;
    b.home = make_temp_dir("falcon_daemon_e2e_b_b");

    auto spawn_node = [&](NodeProc& n, const char* name,
                          std::uint16_t port) {
        ::mkdir((n.home + "/dl").c_str(), 0755);
        n.dir = n.home + "/dl";
        n.out = n.dir + "/payload.bin";
        const std::string conf = write_daemon_conf(
            n.home, name, true, static_cast<std::uint16_t>(
                                   std::stoi(rdv_port)),
            server_token, group_token);
        EXPECT_FALSE(conf.empty());
        // setenv HOME 影响本进程；fork/execv 继承 → 但 A/B 用各自 conf 已
        // 固定路径，HOME 仅决定 swarm_key.pem/tasks.db 落点。
        // 不带 --no-conf（否则显式 --conf-path 也被忽略，conf 不生效）
        return proc_start(n.proc, FALCON_DAEMON_BIN,
                          {"--enable-rpc", "--rpc-listen-host",
                           "127.0.0.1", "--rpc-listen-port",
                           std::to_string(port), "--task-db",
                           n.home + "/tasks.db", "--conf-path", conf});
    };

    // HOME 需在 spawn 前设到该节点目录（子进程继承）
    ::setenv("HOME", a.home.c_str(), 1);
    const std::uint16_t a_port = pick_free_port();
    ASSERT_TRUE(spawn_node(a, "daemon_ba.json", a_port));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(a_port); }, 15000));

    ::setenv("HOME", b.home.c_str(), 1);
    const std::uint16_t b_port = pick_free_port();
    ASSERT_TRUE(spawn_node(b, "daemon_bb.json", b_port));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(b_port); }, 15000));

    auto registered = [&](std::uint16_t port) {
        auto r = rpc_call(port, "falcon.swarm.status", nlohmann::json::array());
        return r.value("result", nlohmann::json::object())
            .value("registered", false);
    };
    ASSERT_TRUE(wait_until([&] { return registered(a_port); }, 20000));
    ASSERT_TRUE(wait_until([&] { return registered(b_port); }, 20000));

    // 两端各起下载
    auto start_dl = [&](NodeProc& n, std::uint16_t port) {
        return rpc_call(port, "aria2.addUri",
                        nlohmann::json::array(
                            {nlohmann::json::array({mirror.url()}),
                             nlohmann::json{{"dir", n.dir},
                                            {"allow-overwrite", "true"}}}));
    };
    const auto add_a = start_dl(a, a_port);
    const auto add_b = start_dl(b, b_port);
    ASSERT_TRUE(add_a.contains("result")) << add_a.dump();
    ASSERT_TRUE(add_b.contains("result")) << add_b.dump();
    const std::string gid_a = add_a["result"].get<std::string>();
    const std::string gid_b = add_b["result"].get<std::string>();

    // 下载进行中杀掉 Rendezvous（硬杀，模拟崩溃）
    ASSERT_TRUE(wait_until(
        [&] {
            auto r = rpc_call(a_port, "aria2.tellActive", nlohmann::json::array());
            return !r.value("result", nlohmann::json::array()).empty();
        },
        15000));
    proc_kill9(rdv);

    // 数据面无 Rendezvous 依赖：两端照常完成，成品逐字节一致
    ASSERT_EQ(wait_task_terminal(a_port, gid_a, 30000), "complete")
        << "daemon A err=" << a.proc.err;
    ASSERT_EQ(wait_task_terminal(b_port, gid_b, 30000), "complete")
        << "daemon B err=" << b.proc.err;

    EXPECT_EQ(read_file_if_exists(a.out), body);
    EXPECT_EQ(read_file_if_exists(b.out), body);

    proc_terminate(a.proc);
    proc_terminate(b.proc);
    EXPECT_EQ(exit_code_of(a.proc), 0) << a.proc.err;
    EXPECT_EQ(exit_code_of(b.proc), 0) << b.proc.err;
    // rendezvous 已硬杀（无 flush 产出预期）
}

// ============================================================================
// 规则 (c)：默认关（share.enabled=false）→ 零公告
// ============================================================================
TEST(SwarmDaemonE2E, DefaultOffAnnouncesNothing) {
    const std::string home = make_temp_dir("falcon_daemon_e2e_c");
    ASSERT_FALSE(home.empty());
    ::setenv("HOME", home.c_str(), 1);

    const std::string body = "default off payload C\n";
    const std::string expect_sha = SwarmCrypto::sha256_hex(body);

    HttpMirror mirror;
    ASSERT_TRUE(mirror.start(body));

    const std::uint16_t rdv_port = pick_free_port();
    Proc rdv;
    ASSERT_TRUE(proc_start(rdv, FALCON_SWARMD_BIN,
                           {"--no-conf", "--swarm-host", "127.0.0.1",
                            "--swarm-port", std::to_string(rdv_port)}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(rdv_port); }, 10000));

    // share.enabled=false（默认关）
    const std::string dir = home + "/dl";
    ::mkdir(dir.c_str(), 0755);
    const std::string conf =
        write_daemon_conf(home, "daemon_c.json", false, rdv_port, "", "");
    ASSERT_FALSE(conf.empty());

    const std::uint16_t daemon_port = pick_free_port();
    Proc daemon;
    ASSERT_TRUE(proc_start(
        daemon, FALCON_DAEMON_BIN,
        {"--enable-rpc", "--rpc-listen-host", "127.0.0.1",
         "--rpc-listen-port", std::to_string(daemon_port), "--task-db",
         home + "/tasks_c.db", "--conf-path", conf}));
    ASSERT_TRUE(wait_until([&] { return tcp_connect(daemon_port); }, 15000));

    // 默认关：status.enabled == false
    auto st = rpc_call(daemon_port, "falcon.swarm.status",
                       nlohmann::json::array());
    ASSERT_TRUE(st.contains("result"));
    EXPECT_FALSE(st["result"].value("enabled", true));

    // 下载完成后仍无公告（query 命中空）
    const std::string out_path = dir + "/payload.bin";
    const auto add = rpc_call(daemon_port, "aria2.addUri",
                              nlohmann::json::array(
                                  {nlohmann::json::array({mirror.url()}),
                                   nlohmann::json{{"dir", dir},
                                                  {"allow-overwrite", "true"}}}));
    ASSERT_TRUE(add.contains("result")) << add.dump();
    ASSERT_EQ(wait_task_terminal(daemon_port, add["result"].get<std::string>(),
                                 20000),
              "complete")
        << "daemon.err=" << daemon.err;
    ASSERT_EQ(read_file_if_exists(out_path), body);

    // 独立节点 query 落空
    auto cfg = SwarmClientConfig{};
    cfg.host = "127.0.0.1";
    cfg.port = static_cast<int>(rdv_port);
    cfg.rpc_timeout_ms = std::chrono::milliseconds(2000);
    SwarmClient qnode(cfg, memory_key());
    std::string qerr;
    ASSERT_TRUE(qnode.start(&qerr)) << qerr;

    // 等查询通道本身可用（连上应答），再断言零公告——固定 sleep 对
    // qnode 首连时序是竞速
    nlohmann::json result;
    ASSERT_TRUE(wait_until(
        [&] { return qnode.query(expect_sha, &result).ok(); }, 10000,
        600)) << "query channel never became usable";
    EXPECT_TRUE(result.value("sources", nlohmann::json::array()).empty());

    qnode.stop();
    proc_terminate(daemon);
    proc_terminate(rdv);
    EXPECT_EQ(exit_code_of(daemon), 0) << daemon.err;
    EXPECT_EQ(exit_code_of(rdv), 0) << rdv.err;
}

#else  // !FALCON_SWARM_E2E_ENABLED

#include <gtest/gtest.h>

TEST(SwarmDaemonE2E, DisabledWithoutSwarmBuild) {
    GTEST_SKIP() << "swarm daemon e2e requires falcon_swarm_client + "
                    "falcon-swarmd (FALCON_SWARM_E2E_ENABLED)";
}

#endif  // FALCON_SWARM_E2E_ENABLED
