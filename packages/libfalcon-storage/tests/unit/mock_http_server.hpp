/**
 * @file mock_http_server.hpp
 * @brief 存储浏览器 mock HTTP 服务器（测试共享）
 * @author Falcon Team
 * @date 2026-09-14
 *
 * 一连接一请求：应答由编程式 handler 决定，请求 (method, path) 记录供断言。
 * 监听 INADDR_ANY + 随机端口，离线可运行。三平台可编译。
 *
 * 停机先 shutdown(listen_fd, SHUT_RDWR) 再 close 再 join——Linux close()
 * 不唤醒阻塞在 accept() 的线程（strace 实证 join 死等）。
 *
 * 应答后半关闭写端并排空读端——客户端 POST 带请求体时，带未读数据直接
 * close 会触发 RST，可能吞掉刚写出的响应（curl 报 empty reply）。
 */

#ifndef FALCON_STORAGE_TESTS_MOCK_HTTP_SERVER_HPP
#define FALCON_STORAGE_TESTS_MOCK_HTTP_SERVER_HPP

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <windows.h>
#define CLOSE_SOCKET(fd) closesocket(fd)
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/time.h>
#define CLOSE_SOCKET(fd) close(fd)
#endif

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

/// 存储浏览器 mock 服务器：一连接一请求，应答由编程式 handler 决定
class MockHttpServer {
public:
    struct Response {
        int status = 200;
        std::string body = "ok";
        std::map<std::string, std::string> headers;  // 额外响应头
    };
    using Handler = std::function<Response(const std::string& method,
                                           const std::string& path)>;

    explicit MockHttpServer(Handler handler) : handler_(std::move(handler)) {}
    ~MockHttpServer() { stop(); }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    bool start() {
#ifdef _WIN32
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
#endif
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = 0;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listen_fd_, 8) != 0) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
            return false;
        }

        sockaddr_in bound{};
#ifdef _WIN32
        int len = sizeof(bound);
#else
        socklen_t len = sizeof(bound);
#endif
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            stop();
            return false;
        }
        port_ = ntohs(bound.sin_port);

        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) {
            return;
        }
        if (listen_fd_ >= 0) {
            // shutdown 唤醒阻塞在 accept 的线程（close 不会）
#ifdef _WIN32
            ::shutdown(listen_fd_, SD_BOTH);
#else
            ::shutdown(listen_fd_, SHUT_RDWR);
#endif
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }
    }

    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port_); }

    std::vector<std::pair<std::string, std::string>> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    void accept_loop() {
        while (running_.load()) {
            int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                return;  // listen fd 已关闭（stop）
            }
            handle_connection(client);
            CLOSE_SOCKET(client);
        }
    }

    void handle_connection(int client) {
        // 读到请求头结束（请求体不参与断言，交由收尾排空）
        std::string request;
        char buf[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            int n = static_cast<int>(::recv(client, buf, sizeof(buf), 0));
            if (n <= 0) return;
            request.append(buf, static_cast<size_t>(n));
            if (request.size() > 64 * 1024) return;
        }

        // 请求行：METHOD SP PATH SP VERSION
        const std::string line = request.substr(0, request.find("\r\n"));
        const auto sp1 = line.find(' ');
        const auto sp2 = line.find(' ', sp1 + 1);
        if (sp1 == std::string::npos || sp2 == std::string::npos) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.emplace_back(line.substr(0, sp1), line.substr(sp1 + 1, sp2 - sp1 - 1));
        }

        const Response resp = handler_(line.substr(0, sp1),
                                       line.substr(sp1 + 1, sp2 - sp1 - 1));
        // 自定义头与固定头冲突时（如 HEAD 用例自定 Content-Length）以自定义为准，
        // 重复且不一致的 Content-Length 会被 curl 拒绝（Weird server reply）
        const bool has_cl = resp.headers.count("Content-Length") > 0;
        const bool has_ct = resp.headers.count("Content-Type") > 0;
        std::string raw = "HTTP/1.1 " + std::to_string(resp.status) +
                          (resp.status < 400 ? " OK" : " Error") + "\r\n";
        if (!has_ct) {
            raw += "Content-Type: application/json\r\n";
        }
        if (!has_cl) {
            raw += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
        }
        raw += "Connection: close\r\n";
        for (const auto& [k, v] : resp.headers) {
            raw += k + ": " + v + "\r\n";
        }
        raw += "\r\n" + resp.body;
        if (::send(client, raw.c_str(), raw.size(), 0) < 0) {
            return;
        }

        // 半关闭写端并排空读端：未读的请求体不排空就 close 会以 RST 收场，
        // 响应可能未及送达；Connection: close 下客户端读完即关，排空自然终止
#ifdef _WIN32
        ::shutdown(client, SD_SEND);
        DWORD timeout_ms = 2000;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        ::shutdown(client, SHUT_WR);
        timeval tv{};
        tv.tv_sec = 2;
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        while (::recv(client, buf, sizeof(buf), 0) > 0) {
        }
    }

    Handler handler_;
    int listen_fd_ = -1;
    uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, std::string>> requests_;
};

#endif  // FALCON_STORAGE_TESTS_MOCK_HTTP_SERVER_HPP
