
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

// 请求记录（分段 Range 证据链）
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
    // Accept-Ranges: bytes + 全量 body）。用于验证分段下载遇到拒绝切片
    // 时的失败收口，不会假完成。
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
        // 服务器意外提前退出时对已关 fd 的写按 EPIPE 返回而非 SIGPIPE
        // 杀死整个测试进程（同文件服务器侧 send 的既有纪律）
        const ssize_t n =
            ::send(fd, http.data() + off, http.size() - off,
#ifdef __APPLE__
                   0
#else
                   MSG_NOSIGNAL
#endif
                   );
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


} // namespace
