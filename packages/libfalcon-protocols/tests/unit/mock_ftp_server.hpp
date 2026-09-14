/**
 * @file mock_ftp_server.hpp
 * @brief FTP mock 服务器（单元测试用，下载语义扩展版）
 *
 * 编程式 FTP 服务器：文本控制协议 + EPSV 被动模式数据连接。
 * 自 libfalcon-storage 的 mock_ftp_server.hpp 复制并扩展下载路径：
 * RETR（按 set_file_content 内容发数据）、REST（断点续传偏移）、
 * 一次性命令失败（重试语义）、分块慢发（暂停窗口）。
 *
 * 每条控制命令原样记录供断言；文件内容、SIZE 结果与命令成败均可
 * 按路径/命令编程。每条控制连接一线程，连接入口重置工作目录与
 * REST 偏移（curl 的 easy handle 逐会话独立连接）。
 *
 * 已知简化：数据连接监听口为服务器级单实例（测试串行使用）；
 * 认证为构造时给定的期望凭据（空表示接受任意）；不支持 FTPS
 * （AUTH TLS 应答 502，测试仅用明文 ftp://）。
 */
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace falcon::test {

class MockFtpServer {
public:
    MockFtpServer() = default;
    MockFtpServer(const std::string& expect_user, const std::string& expect_pass)
        : expect_user_(expect_user), expect_pass_(expect_pass) {}

    ~MockFtpServer() { stop(); }

    MockFtpServer(const MockFtpServer&) = delete;
    MockFtpServer& operator=(const MockFtpServer&) = delete;

    void start() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        ASSERT_SYS(::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
        ASSERT_SYS(::listen(listen_fd_, 8) == 0);

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        ASSERT_SYS(::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) == 0);
        port_ = ntohs(bound.sin_port);

        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });

        // 等 accept 就绪，避免测试抢先连接
        std::unique_lock<std::mutex> lock(ready_mutex_);
        ready_cv_.wait(lock, [this] { return ready_; });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // Linux close() 不会唤醒阻塞在 accept() 的线程：先 shutdown 再 close 再 join
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        if (accept_thread_.joinable()) accept_thread_.join();

        std::lock_guard<std::mutex> lock(data_mutex_);
        if (data_listen_fd_ >= 0) {
            ::shutdown(data_listen_fd_, SHUT_RDWR);
            ::close(data_listen_fd_);
            data_listen_fd_ = -1;
        }
    }

    int port() const { return port_; }

    // ---- 应答编程 ----

    /// 目录 listing 文本（路径不含尾部斜杠；根为 "/"）
    void set_listing(const std::string& path, const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex_);
        listings_[normalize(path)] = text;
    }

    /// 让对路径的 LIST/NLST 失败（550）
    void fail_list(const std::string& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_list_.insert(normalize(path));
    }

    /// 让某命令词失败（如 "RNTO"、"DELE"）
    void fail_command(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_cmds_.insert(cmd);
    }

    /// 让某命令词**下一次**失败（重试语义：先败一次，再放行）
    void fail_command_once(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_once_cmds_.insert(cmd);
    }

    /// 已知文件（SIZE 应答 213 + 大小；未知文件 550）
    void set_file_size(const std::string& path, uint64_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        sizes_[normalize(path)] = size;
    }

    /// 已知文件内容（RETR 数据通道按此应答；REST 偏移后从该处截尾发送）
    void set_file_content(const std::string& path, const std::string& content) {
        std::lock_guard<std::mutex> lock(mutex_);
        contents_[normalize(path)] = content;
        sizes_[normalize(path)] = content.size();
    }

    /// RETR 数据通道分块慢发（暂停/取消窗口用；0 = 尽快发完）
    void set_chunk_delay_us(int delay_us, size_t chunk_size = 32) {
        std::lock_guard<std::mutex> lock(mutex_);
        chunk_delay_us_ = delay_us;
        chunk_size_ = chunk_size;
    }

    // ---- 记录 ----

    std::vector<std::string> commands() {
        std::lock_guard<std::mutex> lock(mutex_);
        return commands_;
    }

    size_t count_command(const std::string& cmd) {
        size_t n = 0;
        for (const auto& c : commands()) {
            if (c == cmd || c.rfind(cmd + " ", 0) == 0) ++n;
        }
        return n;
    }

    bool any_command(const std::string& cmd) { return count_command(cmd) > 0; }

private:
    static void ASSERT_SYS(bool ok) {
        if (!ok) {
            std::fprintf(stderr, "mock_ftp_server syscall failed: %s\n", std::strerror(errno));
            std::abort();
        }
    }

    static std::string normalize(std::string s) {
        if (!s.empty() && s[0] != '/') s = "/" + s;
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        if (s.empty()) s = "/";
        return s;
    }

    /// 相对参数（libcurl multicwd 模式 CWD 后发 basename）按当前目录解析
    std::string resolve_arg(const std::string& arg) {
        if (arg.empty()) return cwd_;
        if (arg[0] == '/') return normalize(arg);
        return normalize(cwd_ == "/" ? "/" + arg : cwd_ + "/" + arg);
    }

    static bool send_all(int fd, const std::string& text) {
        size_t sent = 0;
        while (sent < text.size()) {
            ssize_t n = ::send(fd, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    /// 从 fd 读一行 CRLF 结尾的命令（缓冲跨行保留）
    static bool read_line(int fd, std::string& buffer, std::string& line) {
        for (;;) {
            size_t pos = buffer.find("\r\n");
            if (pos != std::string::npos) {
                line = buffer.substr(0, pos);
                buffer.erase(0, pos + 2);
                return true;
            }
            char chunk[2048];
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buffer.append(chunk, static_cast<size_t>(n));
        }
    }

    void record(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex_);
        commands_.push_back(line);
    }

    void accept_loop() {
        {
            std::lock_guard<std::mutex> lock(ready_mutex_);
            ready_ = true;
        }
        ready_cv_.notify_all();

        while (running_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) return;
                continue;
            }
            // 控制连接读超时：对端异常消失时线程可自行退出
            timeval tv{10, 0};
            ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            std::thread([this, fd] { handle_control(fd); }).detach();
        }
    }

    void handle_control(int fd) {
        // 每条控制连接是独立会话：重置会话态（服务器级字段被并发
        // 会话共享，但测试串行使用，重置保证会话间不互踩）
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cwd_ = "/";
            rest_offset_ = 0;
        }
        send_all(fd, "220 Mock FTP ready\r\n");
        std::string buffer, line;
        while (read_line(fd, buffer, line)) {
            std::string cmd = line, arg;
            size_t sp = line.find(' ');
            if (sp != std::string::npos) {
                cmd = line.substr(0, sp);
                arg = line.substr(sp + 1);
            }
            std::string upper = cmd;
            for (auto& ch : upper) ch = static_cast<char>(::toupper(static_cast<unsigned char>(ch)));
            record(line);

            if (upper == "USER") {
                send_all(fd, "331 Password required\r\n");
            } else if (upper == "PASS") {
                bool ok = expect_pass_.empty() || (arg == expect_pass_);
                send_all(fd, ok ? "230 Login successful\r\n" : "530 Login incorrect\r\n");
                if (!ok) break;
            } else if (upper == "QUIT") {
                send_all(fd, "221 Bye\r\n");
                break;
            } else if (upper == "EPSV" || upper == "PASV") {
                int data_port = open_data_listener();
                if (data_port < 0) {
                    send_all(fd, "425 Cannot open data connection\r\n");
                    continue;
                }
                send_all(fd, "229 Entering Extended Passive Mode (|||" +
                                 std::to_string(data_port) + "|)\r\n");
            } else if (upper == "LIST" || upper == "NLST") {
                serve_listing(fd, arg);
            } else if (upper == "RETR") {
                serve_file(fd, arg);
            } else if (upper == "REST") {
                uint64_t offset = 0;
                try {
                    offset = std::stoull(arg);
                } catch (...) {
                    send_all(fd, "501 Bad offset\r\n");
                    continue;
                }
                rest_offset_ = offset;
                send_all(fd, "350 Restarting at " + arg + ". Send STORE or RETRIEVE\r\n");
            } else if (upper == "TYPE" || upper == "NOOP") {
                send_all(fd, "200 OK\r\n");
            } else if (upper == "PWD") {
                send_all(fd, "257 \"/\" is the current directory\r\n");
            } else if (upper == "CWD") {
                std::lock_guard<std::mutex> lock(mutex_);
                // curl multicwd 逐段下发相对路径（CWD / → CWD docs →
                // CWD sub），与真实服务器一致按当前目录解析
                cwd_ = resolve_arg(arg);
                send_all(fd, "250 OK\r\n");
            } else if (upper == "FEAT") {
                send_all(fd, "211 END\r\n");
            } else if (upper == "SIZE") {
                uint64_t size = 0;
                bool known;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    auto it = sizes_.find(resolve_arg(arg));
                    known = it != sizes_.end();
                    if (known) size = it->second;
                }
                if (known) {
                    send_all(fd, "213 " + std::to_string(size) + "\r\n");
                } else {
                    send_all(fd, "550 Could not get file size\r\n");
                }
            } else if (upper == "RNFR") {
                send_all(fd, rejected(upper) ? "550 RNFR failed\r\n" : "350 Ready for RNTO\r\n");
            } else if (upper == "RNTO") {
                send_all(fd, rejected(upper) ? "550 RNTO failed\r\n" : "250 Rename successful\r\n");
            } else if (upper == "DELE") {
                send_all(fd, rejected(upper) ? "550 Delete failed\r\n" : "250 Deleted\r\n");
            } else if (upper == "RMD") {
                send_all(fd, rejected(upper) ? "550 Remove failed\r\n" : "250 Removed\r\n");
            } else if (upper == "MKD") {
                send_all(fd, rejected(upper) ? "550 MKD failed\r\n"
                                             : "257 \"" + arg + "\" created\r\n");
            } else {
                send_all(fd, "502 Command not implemented\r\n");
            }
        }
        ::close(fd);
    }

    void serve_listing(int ctrl_fd, const std::string& arg) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path = resolve_arg(arg);
        }
        if (fail_list_.count(path)) {
            send_all(ctrl_fd, "550 No such directory\r\n");
            close_data_listener();
            return;
        }
        send_all(ctrl_fd, "150 Opening BINARY mode data connection\r\n");

        int data_fd = accept_data();
        if (data_fd >= 0) {
            std::string text;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = listings_.find(path);
                text = it == listings_.end() ? "" : it->second;
            }
            send_all(data_fd, text);
            ::close(data_fd);
        }
        send_all(ctrl_fd, data_fd >= 0 ? "226 Transfer complete\r\n"
                                       : "425 Cannot open data connection\r\n");
    }

    void serve_file(int ctrl_fd, const std::string& arg) {
        std::string path;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            path = resolve_arg(arg);
        }
        if (rejected("RETR")) { // 按命令词编程（fail_command/fail_command_once）
            send_all(ctrl_fd, "550 Could not get file\r\n");
            close_data_listener();
            return;
        }
        send_all(ctrl_fd, "150 Opening BINARY mode data connection\r\n");

        int data_fd = accept_data();
        if (data_fd >= 0) {
            std::string content;
            uint64_t offset = 0;
            int delay_us = 0;
            size_t chunk_size = 32;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                offset = rest_offset_;
                rest_offset_ = 0; // 一次性：REST 只约束紧随的 RETR
                auto it = contents_.find(path);
                if (it != contents_.end()) {
                    content = offset < it->second.size()
                                  ? it->second.substr(static_cast<size_t>(offset))
                                  : std::string{};
                }
                delay_us = chunk_delay_us_;
                chunk_size = chunk_size_;
            }
            // 分块慢发：对端（暂停中止的 curl）提前关闭时 send 失败即停
            bool ok = true;
            for (size_t pos = 0; ok && pos < content.size(); pos += chunk_size) {
                if (delay_us > 0) std::this_thread::sleep_for(std::chrono::microseconds(delay_us));
                ok = send_all(data_fd, content.substr(pos, chunk_size));
            }
            ::close(data_fd);
            send_all(ctrl_fd, ok ? "226 Transfer complete\r\n"
                                 : "426 Connection closed; transfer aborted\r\n");
        } else {
            send_all(ctrl_fd, "425 Cannot open data connection\r\n");
        }
    }

    int open_data_listener() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        close_data_listener();
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0;
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(fd, 2) < 0) {
            ::close(fd);
            return -1;
        }
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) < 0) {
            ::close(fd);
            return -1;
        }
        data_listen_fd_ = fd;
        return ntohs(bound.sin_port);
    }

    int accept_data() {
        std::lock_guard<std::mutex> lock(data_mutex_);
        if (data_listen_fd_ < 0) return -1;
        timeval tv{10, 0};
        ::setsockopt(data_listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int fd = ::accept(data_listen_fd_, nullptr, nullptr);
        ::close(data_listen_fd_);
        data_listen_fd_ = -1;
        return fd;
    }

    void close_data_listener() {
        if (data_listen_fd_ >= 0) {
            ::shutdown(data_listen_fd_, SHUT_RDWR);
            ::close(data_listen_fd_);
            data_listen_fd_ = -1;
        }
    }

    bool rejected(const std::string& cmd) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_once_cmds_.count(cmd) > 0) {
            fail_once_cmds_.erase(cmd); // 一次性：只挡下一发
            return true;
        }
        return fail_cmds_.count(cmd) > 0;
    }

    std::mutex mutex_;
    std::vector<std::string> commands_;
    std::map<std::string, std::string> listings_;
    std::map<std::string, std::string> contents_;
    std::map<std::string, uint64_t> sizes_;
    std::set<std::string> fail_list_;
    std::set<std::string> fail_cmds_;
    std::set<std::string> fail_once_cmds_;
    std::string cwd_ = "/";
    uint64_t rest_offset_ = 0;
    int chunk_delay_us_ = 0;
    size_t chunk_size_ = 32;
    std::string expect_user_;
    std::string expect_pass_;

    std::mutex data_mutex_;
    int data_listen_fd_ = -1;

    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    int port_ = 0;
    std::thread accept_thread_;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;
    bool ready_ = false;
};

} // namespace falcon::test
