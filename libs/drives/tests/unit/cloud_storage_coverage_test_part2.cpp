/**
 * @file cloud_storage_coverage_test_part2.cpp
 * @brief 网盘插件覆盖率补充测试（链接识别 + 本地回环 HTTP 服务器驱动内置插件）
 * @author Falcon Team
 * @date 2026-09-05
 */

#include <csignal>
#include <gtest/gtest.h>
#include <falcon/drives/cloud_storage_plugin.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace falcon;

namespace {
/// 本文件回环基建含「写已断连 socket」剧本（客户端失败收口主动 abort
/// 连接、服务器按剧本 drop 短 body/超限请求、超时清理关 fd 后仍有在途
/// 写）——POSIX 默认对 socket 写的 SIGPIPE 做进程级终止，任一写命中
/// RST 即杀死整个测试进程（RangeLiarServer / TlsTestServer /
/// ProxyTestServer 同族红面先例）。进程级忽略让写失败以 EPIPE 返回
/// 值出现（各 send_all 的 n<=0 分支既有吞错路径正常收口）；Windows 无
/// SIGPIPE 语义无需处理
struct SigpipeImmunity {
    SigpipeImmunity() {
#if !defined(_WIN32)
        ::signal(SIGPIPE, SIG_IGN);
#endif
    }
};

const SigpipeImmunity falcon_test_sigpipe_immunity;

struct HttpRequest {
    std::string method;
    std::string path;
    std::string query;
    std::string body;
};

class LocalHttpServer {
public:
    using Handler = std::function<std::string(const HttpRequest&)>;

    LocalHttpServer() = default;
    ~LocalHttpServer() { stop(); }
    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    void set_handler(Handler handler) { handler_ = std::move(handler); }

    bool start() {
        stop();
#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
#endif
        listen_fd_ = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
        if (listen_fd_ < 0) {
            cleanup_winsock();
            return false;
        }

        int opt = 1;
        (void)::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
#ifdef _WIN32
                           reinterpret_cast<const char*>(&opt),
#else
                           &opt,
#endif
                           sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = -1;
            cleanup_winsock();
            return false;
        }

        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = -1;
            cleanup_winsock();
            return false;
        }
        port_ = ntohs(bound.sin_port);

        if (::listen(listen_fd_, 16) != 0) {
            close_socket(listen_fd_);
            listen_fd_ = -1;
            cleanup_winsock();
            return false;
        }

        running_.store(true);
        accept_thread_ = std::thread([this] { accept_loop(); });
        return true;
    }

    void stop() {
        running_.store(false);
        if (listen_fd_ >= 0) {
#ifdef _WIN32
            ::closesocket(static_cast<SOCKET>(listen_fd_));
#else
            // shutdown 能唤醒阻塞在 accept() 中的线程，close 不能
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
#endif
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& t : client_threads_) {
            if (t.joinable()) t.join();
        }
        client_threads_.clear();
        cleanup_winsock();
    }

    int port() const { return static_cast<int>(port_); }
    std::string base_url() const { return "http://127.0.0.1:" + std::to_string(port()); }

    std::vector<std::string> requests() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

private:
    static void close_socket(int fd) {
#ifdef _WIN32
        ::closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
    }

    static void cleanup_winsock() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    static void set_recv_timeout(int fd) {
#ifdef _WIN32
        DWORD timeout_ms = 5000;
        (void)::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
                           reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
#else
        timeval tv{};
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    static bool send_all(int fd, const std::string& data) {
        std::size_t sent_total = 0;
        while (sent_total < data.size()) {
#ifdef _WIN32
            const int chunk = static_cast<int>(std::min<std::size_t>(data.size() - sent_total, 32768u));
            const int sent = ::send(static_cast<SOCKET>(fd), data.data() + sent_total, chunk, 0);
            if (sent <= 0) return false;
            sent_total += static_cast<std::size_t>(sent);
#else
            const ssize_t sent = ::send(fd, data.data() + sent_total, data.size() - sent_total, 0);
            if (sent <= 0) return false;
            sent_total += static_cast<std::size_t>(sent);
#endif
        }
        return true;
    }

    static bool recv_request(int fd, std::string& raw, std::size_t& header_end) {
        while (true) {
            char buf[2048];
#ifdef _WIN32
            const int n = ::recv(static_cast<SOCKET>(fd), buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) return false;
            raw.append(buf, static_cast<std::size_t>(n));
            const std::size_t pos = raw.find("\r\n\r\n");
            if (pos != std::string::npos) {
                header_end = pos + 4;
                return true;
            }
            if (raw.size() > 64u * 1024u) return false;
        }
    }

    static bool recv_more(int fd, std::string& body, std::size_t expected) {
        while (body.size() < expected) {
            char buf[2048];
#ifdef _WIN32
            const int n = ::recv(static_cast<SOCKET>(fd), buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) return false;
            body.append(buf, static_cast<std::size_t>(n));
        }
        return true;
    }

    void accept_loop() {
        while (running_.load()) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            const int fd = static_cast<int>(
                ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &len));
            if (fd < 0) {
                if (running_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                continue;
            }
            client_threads_.emplace_back([this, fd] { handle_client(fd); });
        }
    }

    void handle_client(int fd) {
        set_recv_timeout(fd);

        std::string raw;
        std::size_t header_end = 0;
        if (!recv_request(fd, raw, header_end)) {
            close_socket(fd);
            return;
        }

        std::string method;
        std::string target;
        {
            const std::size_t line_end = raw.find("\r\n");
            if (line_end == std::string::npos) {
                close_socket(fd);
                return;
            }
            std::istringstream iss(raw.substr(0, line_end));
            iss >> method >> target;
        }

        std::size_t content_length = 0;
        for (std::size_t pos = raw.find("\r\n") + 2; pos < raw.size();) {
            const std::size_t next = raw.find("\r\n", pos);
            if (next == std::string::npos || next == pos) break;
            const std::string line = raw.substr(pos, next - pos);
            pos = next + 2;
            const std::size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            for (auto& c : key) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            if (key == "content-length") {
                content_length = static_cast<std::size_t>(std::stoull(line.substr(colon + 1)));
            }
        }

        std::string body = raw.substr(header_end);
        if (body.size() < content_length && !recv_more(fd, body, content_length)) {
            close_socket(fd);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            requests_.push_back(method + " " + target);
        }

        HttpRequest req;
        req.method = method;
        req.body = body;
        const std::size_t q = target.find('?');
        if (q == std::string::npos) {
            req.path = target;
        } else {
            req.path = target.substr(0, q);
            req.query = target.substr(q + 1);
        }

        const std::string response_body = handler_ ? handler_(req) : std::string();
        const std::string response =
            "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(response_body.size()) +
            "\r\nConnection: close\r\n\r\n" + response_body;
        (void)send_all(fd, response);

#ifdef _WIN32
        ::shutdown(static_cast<SOCKET>(fd), SD_SEND);
#else
        ::shutdown(fd, SHUT_RDWR);
#endif
        close_socket(fd);
    }

    Handler handler_;
    int listen_fd_ = -1;
    unsigned short port_ = 0;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    mutable std::mutex mutex_;
    std::vector<std::string> requests_;
};

} // namespace

// ============================================================================
// CloudLinkDetector 静态逻辑补充覆盖
// ============================================================================


TEST(CloudStorageCovTest, AliyunAndQuarkFourPieceStubs) {
    CloudStorageManager manager;
    for (const auto& [name, platform_key] :
         std::vector<std::pair<std::string, std::string>>{
             {"AliyunDrive", "AliyunDrive"}, {"QuarkDrive", "QuarkDrive"}}) {
        auto* plugin = find_plugin_by_name(manager, name);
        ASSERT_NE(plugin, nullptr) << name;

        EXPECT_EQ(plugin->get_download_url("file123"), "") << name;
        EXPECT_FALSE(plugin->authenticate("")) << name;
        EXPECT_TRUE(plugin->authenticate("token")) << name;

        const auto user = plugin->get_user_info();
        EXPECT_EQ(user.at("platform"), platform_key) << name;
        EXPECT_EQ(user.at("status"), "anonymous") << name;

        const auto quota = plugin->get_quota_info();
        EXPECT_EQ(quota.at("used"), 0u) << name;
        EXPECT_EQ(quota.at("total"), 0u) << name;
    }
}

// 轻量基类四件套（TencentWeiyun/Cloud115/PikPak/Mega/GoogleDrive/
// OneDrive/Dropbox/YandexDisk 共用基类实现）：经任一子类指针驱动
TEST(CloudStorageCovTest, LightweightBaseFourPieceViaSubclassPointer) {
    CloudStorageManager manager;
    auto* plugin = find_plugin_by_name(manager, "TencentWeiyun");
    ASSERT_NE(plugin, nullptr);

    EXPECT_EQ(plugin->get_download_url("file123"), "");
    EXPECT_FALSE(plugin->authenticate(""));
    EXPECT_TRUE(plugin->authenticate("token"));

    const auto user = plugin->get_user_info();
    EXPECT_EQ(user.at("platform"), "TencentWeiyun");
    EXPECT_EQ(user.at("status"), "anonymous");

    const auto quota = plugin->get_quota_info();
    EXPECT_EQ(quota.at("used"), 0u);
    EXPECT_EQ(quota.at("total"), 0u);
}

// 六个轻量平台的中文展示名经 extract 的「无效链接」错误路径生效
//（file_id 提取失败时拼进 error_message）；空 URL 对所有平台统一
// 落入该分支——纯离线，不发任何网络请求
TEST(CloudStorageCovTest, DisplayNamesSurfacedByInvalidLinkErrorPath) {
    CloudStorageManager manager;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"TencentWeiyun", "无效的腾讯微云链接"},
        {"Cloud115", "无效的115网盘链接"},
        {"PikPak", "无效的PikPak链接"},
        {"OneDrive", "无效的OneDrive链接"},
        {"Dropbox", "无效的Dropbox链接"},
        {"YandexDisk", "无效的Yandex Disk链接"},
    };
    for (const auto& [name, expected_error] : cases) {
        auto* plugin = find_plugin_by_name(manager, name);
        ASSERT_NE(plugin, nullptr) << name;

        auto result = plugin->extract_share_link("", "");
        EXPECT_FALSE(result.recognized) << name;
        EXPECT_FALSE(result.success) << name;
        EXPECT_EQ(result.error_message, expected_error) << name;
        EXPECT_TRUE(result.files.empty()) << name;
    }
}
