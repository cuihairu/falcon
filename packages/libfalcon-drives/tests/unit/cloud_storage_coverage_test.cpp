/**
 * @file cloud_storage_coverage_test.cpp
 * @brief 网盘插件覆盖率补充测试（链接识别 + 本地回环 HTTP 服务器驱动内置插件）
 * @author Falcon Team
 * @date 2026-09-05
 */

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
#include <sstream>
#include <string>
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

TEST(CloudLinkCovTest, DetectPlatformMoreFormats) {
    const std::map<std::string, CloudPlatform> cases = {
        {"https://115.com/s/abc1", CloudPlatform::Cloud115},
        {"https://anxia.com/s/abc2", CloudPlatform::Cloud115},
        {"https://pan.quark.cn/s/abc3", CloudPlatform::Quark},
        {"quark://abc4", CloudPlatform::Quark},
        {"https://mypikpak.com/s/abc5", CloudPlatform::PikPak},
        {"https://share.pikpak.com/s/abc6", CloudPlatform::PikPak},
        {"https://mega.co.nz/#abc7", CloudPlatform::Mega},
        {"https://mega.nz/#ab_c8", CloudPlatform::Mega},
        {"mega://key!9", CloudPlatform::Mega},
        {"https://docs.google.com/spreadsheets/x", CloudPlatform::GoogleDrive},
        {"https://yadi.sk/d/abc10/file", CloudPlatform::YandexDisk},
        {"https://pan.qq.com/s/abc11", CloudPlatform::TencentWeiyun}
    };

    for (const auto& entry : cases) {
        EXPECT_EQ(CloudLinkDetector::detect_platform(entry.first), entry.second)
            << "Failed for URL: " << entry.first;
    }
}

TEST(CloudLinkCovTest, ExtractFileIdAllPlatforms) {
    EXPECT_EQ("1abc_XY", CloudLinkDetector::extract_file_id(
                             "https://pan.baidu.com/s/1abc_XY", CloudPlatform::BaiduNetdisk));
    EXPECT_EQ("iabc", CloudLinkDetector::extract_file_id(
                          "https://www.lanzoux.com/iabc?x=1", CloudPlatform::LanzouCloud));
    EXPECT_EQ("zz9", CloudLinkDetector::extract_file_id(
                         "https://www.alipan.com/s/zz9", CloudPlatform::AlibabaCloud));
    EXPECT_EQ("q1", CloudLinkDetector::extract_file_id(
                        "https://115.com/s/q1", CloudPlatform::Cloud115));
    EXPECT_EQ("q2", CloudLinkDetector::extract_file_id(
                        "https://pan.quark.cn/s/q2", CloudPlatform::Quark));
    EXPECT_EQ("p3", CloudLinkDetector::extract_file_id(
                        "https://share.pikpak.com/s/p3", CloudPlatform::PikPak));
    EXPECT_EQ("wy77", CloudLinkDetector::extract_file_id(
                          "https://share.weiyun.com/wy77", CloudPlatform::TencentWeiyun));
    EXPECT_EQ("Ab_12", CloudLinkDetector::extract_file_id(
                           "https://mega.nz/#Ab_12", CloudPlatform::Mega));
    EXPECT_EQ("1B2_c", CloudLinkDetector::extract_file_id(
                           "https://drive.google.com/file/d/1B2_c/view", CloudPlatform::GoogleDrive));
    EXPECT_EQ("id9", CloudLinkDetector::extract_file_id(
                         "https://drive.google.com/open?id=id9", CloudPlatform::GoogleDrive));
    EXPECT_EQ("abc123", CloudLinkDetector::extract_file_id(
                            "https://www.dropbox.com/s/abc123/file", CloudPlatform::Dropbox));
    EXPECT_EQ("yx8", CloudLinkDetector::extract_file_id(
                         "https://disk.yandex.ru/d/yx8/file", CloudPlatform::YandexDisk));
}

TEST(CloudLinkCovTest, ExtractFileIdFallbacks) {
    EXPECT_EQ("", CloudLinkDetector::extract_file_id("https://example.com/s/x",
                                                     CloudPlatform::Unknown));
    EXPECT_EQ("", CloudLinkDetector::extract_file_id("https://pan.baidu.com/other/path",
                                                     CloudPlatform::BaiduNetdisk));
    // OneDrive 的 id 正则无锚定，会先命中主机名段（当前实现行为）
    EXPECT_EQ("one", CloudLinkDetector::extract_file_id("https://one.example/no-id",
                                                         CloudPlatform::OneDrive));
}

TEST(CloudLinkCovTest, NormalizeUrlQueryAndSchemeCombined) {
    EXPECT_EQ("https://pan.baidu.com/s/1abc",
              CloudLinkDetector::normalize_url("pan.baidu.com/s/1abc?pwd=xyz"));
    EXPECT_EQ("https://plain.host/path",
              CloudLinkDetector::normalize_url("plain.host/path"));
}

// ============================================================================
// 本地回环 HTTP 服务器驱动的内置网盘插件覆盖
// ============================================================================

class CloudStorageCovLocalTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(server_.start());
        const std::string base = server_.base_url();
        server_.set_handler([base](const HttpRequest& req) { return route(req, base); });

        lz_direct_ = base + "/lzdirect?src=https://lanzoux.com/ab1";
        lz_pwd_ = base + "/lzpwd?src=https://lanzoux.com/ab2";
        lz_badjson_ = base + "/lzbadjson?src=https://lanzoux.com/ab3";
        lz_nourl_ = base + "/lznourl?src=https://lanzoux.com/ab4";
        lz_nopass_ = base + "/lznopass?src=https://lanzoux.com/ab5";
        lz_nodl_ = base + "/lznodl?src=https://lanzoux.com/ab6";
        lz_empty_ = base + "/lzempty?src=https://lanzoux.com/ab7";
        lz_noname_ = base + "/lznofilename?src=https://lanzoux.com/ab8";
        lz_unit_ = base + "/lzunitsize?src=https://lanzoux.com/ab9";
        baidu_title_ = base + "/s/bdtitle?ref=https://pan.baidu.com/s/1xyz01";
        baidu_no_title_ = base + "/s/bdnotitle?ref=https://pan.baidu.com/s/1xyz02";
        baidu_empty_ = base + "/s/bdempty?ref=https://pan.baidu.com/s/1xyz03";
        aliyun_title_ = base + "/s/ali1?ref=https://www.aliyundrive.com/s/zz01";
        quark_title_ = base + "/s/qk1?ref=https://pan.quark.cn/s/zz01";
        weiyun_ = base + "/s/wy1?ref=https://share.weiyun.com/wy01";
        weiyun_no_title_ = base + "/s/wy2?ref=https://share.weiyun.com/wy02";
        cloud115_ = base + "/s/c115?ref=https://115.com/s/zz01";
        pikpak_ = base + "/s/pp1?ref=https://share.pikpak.com/s/zz01";
        mega_ = base + "/f#mg1?ref=https://mega.nz/#zz01";
        gdrive_ = base + "/file/d/gd1?ref=https://drive.google.com/file/d/zz01";
        onedrive_ = base + "/od1?ref=https://1drv.ms/u/s!Zz01";
        dropbox_ = base + "/s/db1?ref=https://www.dropbox.com/s/zz01/file.zip";
        yandex_ = base + "/d/yx1?ref=https://disk.yandex.ru/d/zz01/file.zip";
    }

    void TearDown() override { server_.stop(); }

    static std::string route(const HttpRequest& req, const std::string& base) {
        if (req.method == "POST") {
            if (req.path == "/lzpwd") return "{\"url\":\"" + base + "/dl/pwd\"}";
            if (req.path == "/lzbadjson") return "<<< not json >>>";
            if (req.path == "/lznourl") return "{}";
            return "";
        }

        if (req.path == "/lzdirect") {
            return "\"filename\":\"pack.zip\",\"size\":\"3M\" 'url': '" + base + "/dlfile', 'pwd': ''";
        }
        if (req.path == "/lzpwd" || req.path == "/lzbadjson" || req.path == "/lznourl") {
            return "\"filename\":\"secret.zip\",\"size\":\"2G\" 'url': '" + base + "/tmp', 'pwd': '123'";
        }
        if (req.path == "/lznopass") {
            return "\"filename\":\"guarded.zip\",\"size\":\"1K\" 'url': '" + base + "/tmp', 'pwd': '999'";
        }
        if (req.path == "/lznodl") {
            return "\"filename\":\"only-meta.zip\",\"size\":\"512K\"";
        }
        if (req.path == "/lzempty") return "";
        if (req.path == "/lznofilename") {
            return "'url': '" + base + "/dlnf', 'pwd': ''";
        }
        if (req.path == "/lzunitsize") {
            return "\"filename\":\"unit.zip\",\"size\":\"2048\" 'url': '" + base + "/dlunit', 'pwd': ''";
        }
        if (req.path == "/s/bdtitle") {
            return "<html><head><title>Summer Movie 2026 - 百度网盘-Share </title></head></html>";
        }
        if (req.path == "/s/bdnotitle") return "<html><body>no title here</body></html>";
        if (req.path == "/s/bdempty") return "";
        if (req.path == "/s/ali1") return "<title>Ali Share File 阿里云盘 - 云存储</title>";
        if (req.path == "/s/qk1") return "<title>Quark File 夸克网盘--</title>";
        if (req.path == "/s/wy1") return "<title>Weiyun File_腾讯微云_|</title>";
        if (req.path == "/s/wy2") return "<html><body></body></html>";
        if (req.path == "/s/c115") return "<title>File115 - 115网盘</title>";
        if (req.path == "/s/pp1") return "<title>Pik File - PikPak</title>";
        if (req.path == "/f") return "<title>MG File | MEGA</title>";
        if (req.path == "/file/d/gd1") return "<title>GD File - Google Drive</title>";
        if (req.path == "/od1") return "<title>OD File - OneDrive</title>";
        if (req.path == "/s/db1") return "<title>DB File - Dropbox</title>";
        if (req.path == "/d/yx1") return "<title>YX File - Yandex Disk</title>";
        if (req.path == "/") return "OK";
        return "404 Not Found";
    }

    static bool saw_request(const std::vector<std::string>& requests,
                            const std::string& needle) {
        return std::any_of(requests.begin(), requests.end(),
                           [&](const std::string& line) {
                               return line.find(needle) != std::string::npos;
                           });
    }

    LocalHttpServer server_;
    std::string lz_direct_;
    std::string lz_pwd_;
    std::string lz_badjson_;
    std::string lz_nourl_;
    std::string lz_nopass_;
    std::string lz_nodl_;
    std::string lz_empty_;
    std::string lz_noname_;
    std::string lz_unit_;
    std::string baidu_title_;
    std::string baidu_no_title_;
    std::string baidu_empty_;
    std::string aliyun_title_;
    std::string quark_title_;
    std::string weiyun_;
    std::string weiyun_no_title_;
    std::string cloud115_;
    std::string pikpak_;
    std::string mega_;
    std::string gdrive_;
    std::string onedrive_;
    std::string dropbox_;
    std::string yandex_;
};

TEST_F(CloudStorageCovLocalTest, LanzouDirectDownloadFlow) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_direct_);

    EXPECT_TRUE(result.recognized);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.platform_name, "LanzouCloud");
    EXPECT_EQ(result.platform_type, CloudPlatform::LanzouCloud);
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].id, "lzdirect");
    EXPECT_EQ(result.files[0].name, "pack.zip");
    EXPECT_EQ(result.files[0].size, 3u * 1024u * 1024u);
    EXPECT_EQ(result.files[0].download_url, server_.base_url() + "/dlfile");
}

TEST_F(CloudStorageCovLocalTest, LanzouPasswordVerificationSuccess) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_pwd_, "123");

    EXPECT_TRUE(result.recognized);
    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].name, "secret.zip");
    EXPECT_EQ(result.files[0].size, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(result.files[0].download_url, server_.base_url() + "/dl/pwd");
    EXPECT_TRUE(saw_request(server_.requests(), "POST /lzpwd?ajax=1"));
}

TEST_F(CloudStorageCovLocalTest, LanzouPasswordBadJsonResponse) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_badjson_, "123");

    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("密码验证失败"), std::string::npos);
    EXPECT_EQ(result.files.size(), 1u);
}

TEST_F(CloudStorageCovLocalTest, LanzouPasswordResponseWithoutUrl) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_nourl_, "123");

    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_NE(result.error_message.find("未返回下载链接"), std::string::npos);
}

TEST_F(CloudStorageCovLocalTest, LanzouRequiresPasswordWhenNoneProvided) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_nopass_);

    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "需要提取密码");
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].password, "999");
    EXPECT_EQ(result.files[0].size, 1024u);
}

TEST_F(CloudStorageCovLocalTest, LanzouPageWithoutDownloadLink) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_nodl_);

    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "未能从分享页面提取下载链接");
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].name, "only-meta.zip");
    EXPECT_EQ(result.files[0].size, 512u * 1024u);
}

TEST_F(CloudStorageCovLocalTest, LanzouEmptyPageIsUnreachable) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_empty_);

    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "无法访问分享页面");
    EXPECT_TRUE(result.files.empty());
}

TEST_F(CloudStorageCovLocalTest, LanzouPageWithoutFileNameStillSucceeds) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_noname_);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_TRUE(result.files[0].name.empty());
    EXPECT_EQ(result.files[0].size, 0u);
    EXPECT_EQ(result.files[0].download_url, server_.base_url() + "/dlnf");
}

TEST_F(CloudStorageCovLocalTest, LanzouPlainByteSizeWithoutSuffix) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(lz_unit_);

    EXPECT_TRUE(result.success);
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].name, "unit.zip");
    EXPECT_EQ(result.files[0].size, 2048u);
}

TEST_F(CloudStorageCovLocalTest, BaiduTitleParsedFromPage) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(baidu_title_, "pw1");

    EXPECT_EQ(result.platform_name, "BaiduNetdisk");
    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].name, "Summer Movie 2026");
    EXPECT_EQ(result.files[0].id, "bdtitle");
    EXPECT_EQ(result.files[0].share_url, server_.base_url() + "/s/bdtitle");
    EXPECT_EQ(result.files[0].password, "pw1");
    EXPECT_NE(result.error_message.find("百度网盘客户端"), std::string::npos);
}

TEST_F(CloudStorageCovLocalTest, BaiduNoTitleAndEmptyPageKeepMetadata) {
    CloudStorageManager manager;

    auto no_title = manager.handle_share_link(baidu_no_title_);
    EXPECT_TRUE(no_title.recognized);
    EXPECT_FALSE(no_title.success);
    ASSERT_EQ(no_title.files.size(), 1u);
    EXPECT_TRUE(no_title.files[0].name.empty());

    auto empty_page = manager.handle_share_link(baidu_empty_);
    EXPECT_TRUE(empty_page.recognized);
    EXPECT_FALSE(empty_page.success);
    ASSERT_EQ(empty_page.files.size(), 1u);
    EXPECT_TRUE(empty_page.files[0].name.empty());
}

TEST_F(CloudStorageCovLocalTest, BaiduInvalidSchemeLinkRejected) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link("baidupan://zzz123");

    EXPECT_FALSE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "无效的百度网盘链接");
    EXPECT_TRUE(result.files.empty());
}

TEST_F(CloudStorageCovLocalTest, AliyunTitleParsedAndInvalidSchemeRejected) {
    CloudStorageManager manager;

    auto titled = manager.handle_share_link(aliyun_title_);
    EXPECT_EQ(titled.platform_name, "AliyunDrive");
    EXPECT_TRUE(titled.recognized);
    EXPECT_FALSE(titled.success);
    ASSERT_EQ(titled.files.size(), 1u);
    EXPECT_EQ(titled.files[0].name, "Ali Share File");

    auto invalid = manager.handle_share_link("alipan://zzz123");
    EXPECT_FALSE(invalid.recognized);
    EXPECT_EQ(invalid.error_message, "无效的阿里云盘链接");
}

TEST_F(CloudStorageCovLocalTest, QuarkTitleParsedAndInvalidSchemeRejected) {
    CloudStorageManager manager;

    auto titled = manager.handle_share_link(quark_title_);
    EXPECT_EQ(titled.platform_name, "QuarkDrive");
    EXPECT_TRUE(titled.recognized);
    EXPECT_FALSE(titled.success);
    ASSERT_EQ(titled.files.size(), 1u);
    EXPECT_EQ(titled.files[0].name, "Quark File");

    auto invalid = manager.handle_share_link("quark://zzz123");
    EXPECT_FALSE(invalid.recognized);
    EXPECT_EQ(invalid.error_message, "无效的夸克网盘链接");
}

TEST_F(CloudStorageCovLocalTest, MegaInvalidSchemeLinkRejected) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link("mega://key!123");

    EXPECT_FALSE(result.recognized);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.error_message, "无效的MEGA链接");
}

TEST_F(CloudStorageCovLocalTest, LightweightPluginsRecognizeAndParseTitle) {
    const std::map<std::string, std::pair<std::string, std::string>> cases = {
        {weiyun_, {"TencentWeiyun", "Weiyun File"}},
        {cloud115_, {"Cloud115", "File115"}},
        {pikpak_, {"PikPak", "Pik File"}},
        {mega_, {"MEGA", "MG File"}},
        {gdrive_, {"GoogleDrive", "GD File"}},
        {onedrive_, {"OneDrive", "OD File"}},
        {dropbox_, {"Dropbox", "DB File"}},
        {yandex_, {"YandexDisk", "YX File - Yandex Disk"}}
    };

    CloudStorageManager manager;
    for (const auto& entry : cases) {
        auto result = manager.handle_share_link(entry.first);
        EXPECT_EQ(result.platform_name, entry.second.first) << "URL: " << entry.first;
        EXPECT_TRUE(result.recognized) << "URL: " << entry.first;
        EXPECT_FALSE(result.success) << "URL: " << entry.first;
        ASSERT_EQ(result.files.size(), 1u) << "URL: " << entry.first;
        EXPECT_EQ(result.files[0].name, entry.second.second) << "URL: " << entry.first;
        EXPECT_FALSE(result.error_message.empty()) << "URL: " << entry.first;
    }
}

TEST_F(CloudStorageCovLocalTest, LightweightNoTitlePageStillRecognized) {
    CloudStorageManager manager;
    auto result = manager.handle_share_link(weiyun_no_title_);

    EXPECT_EQ(result.platform_name, "TencentWeiyun");
    EXPECT_TRUE(result.recognized);
    EXPECT_FALSE(result.success);
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_TRUE(result.files[0].name.empty());
}

TEST_F(CloudStorageCovLocalTest, GetDirectDownloadUrlSuccess) {
    CloudStorageManager manager;
    auto url = manager.get_direct_download_url(lz_direct_);
    EXPECT_EQ(url, server_.base_url() + "/dlfile");
}

TEST_F(CloudStorageCovLocalTest, GetDirectDownloadUrlNeedsFurtherProcessing) {
    CloudStorageManager manager;
    auto url = manager.get_direct_download_url(baidu_title_);
    EXPECT_TRUE(url.empty());
}

TEST_F(CloudStorageCovLocalTest, BatchExtractWithLocalUrlsAndPasswords) {
    CloudStorageManager manager;

    std::vector<std::string> urls = {lz_direct_, lz_pwd_, baidu_title_};
    std::map<std::string, std::string> passwords = {{lz_pwd_, "123"}};

    auto results = manager.batch_extract(urls, passwords);
    ASSERT_EQ(results.size(), 3u);

    EXPECT_TRUE(results[0].success);
    EXPECT_EQ(results[0].files[0].download_url, server_.base_url() + "/dlfile");

    EXPECT_TRUE(results[1].success);
    EXPECT_EQ(results[1].files[0].download_url, server_.base_url() + "/dl/pwd");

    EXPECT_FALSE(results[2].success);
    EXPECT_TRUE(results[2].recognized);

    auto no_password_results = manager.batch_extract({lz_direct_});
    ASSERT_EQ(no_password_results.size(), 1u);
    EXPECT_TRUE(no_password_results[0].success);
}

namespace {

class CovKnownTypePlugin : public ICloudStoragePlugin {
public:
    std::string platform_name() const override { return "CovKnown"; }
    CloudPlatform platform_type() const override { return CloudPlatform::BaiduNetdisk; }

    bool can_handle(const std::string& url) const override {
        return url.rfind("cov-known://", 0) == 0;
    }

    CloudExtractionResult extract_share_link(const std::string& share_url,
                                             const std::string& password = "") override {
        (void)share_url;
        (void)password;
        CloudExtractionResult result;
        result.success = true;
        result.recognized = true;
        result.platform_name = platform_name();
        result.platform_type = platform_type();
        CloudFileInfo file;
        file.name = "cov.bin";
        file.download_url = "http://cdn.cov/x";
        result.files.push_back(file);
        return result;
    }

    std::string get_download_url(const std::string& file_id,
                                 const CloudDownloadOptions& options = {}) override {
        (void)file_id;
        (void)options;
        return "http://cdn.cov/x";
    }

    bool authenticate(const std::string& token) override { return !token.empty(); }
    std::map<std::string, std::string> get_user_info() override { return {}; }
    std::map<std::string, size_t> get_quota_info() override { return {}; }
};

} // namespace

TEST_F(CloudStorageCovLocalTest, ManagerFallsBackToCustomPluginWithKnownPlatformType) {
    CloudStorageManager manager;
    manager.register_plugin(std::make_unique<CovKnownTypePlugin>());

    auto result = manager.handle_share_link("cov-known://xyz");
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.platform_name, "CovKnown");
    ASSERT_EQ(result.files.size(), 1u);
    EXPECT_EQ(result.files[0].name, "cov.bin");
}

// ============================================================================
// 失败路径：detect 命中但 file_id 提取为空 / 无插件可路由（覆盖率批次 M）
// ============================================================================

// 蓝奏云：detect 的 [\w]+ 接受下划线开头，extract 的 [a-zA-Z0-9]+ 不接受
// ——「识别到平台但拿不到文件 id」直接短路为无效链接（不发起网络请求）
TEST(CloudStorageCovTest, LanzouInvalidLinkRejected) {
    CloudStorageManager manager;

    auto result = manager.handle_share_link("https://www.lanzoux.com/_-");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.recognized);
    EXPECT_EQ(result.error_message, "无效的蓝奏云链接");
    EXPECT_EQ(result.platform_name, "LanzouCloud");
    EXPECT_EQ(result.platform_type, CloudPlatform::LanzouCloud);
    EXPECT_TRUE(result.files.empty());
}

// Google Drive：docs.google.com 形态 detect 必命中（[^\s]+），
// 但无 /file/d/ 与 ?id= 时 extract 为空——轻量基类的无效链接分支
TEST(CloudStorageCovTest, GoogleDriveDocsLinkWithoutFileId) {
    CloudStorageManager manager;

    auto result = manager.handle_share_link("https://docs.google.com/xyz");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.recognized);
    EXPECT_EQ(result.error_message, "无效的Google Drive链接");
    EXPECT_EQ(result.platform_name, "GoogleDrive");
    EXPECT_EQ(result.platform_type, CloudPlatform::GoogleDrive);
    EXPECT_TRUE(result.files.empty());
}

// 未知平台且无自定义插件接手：三循环路由全部落空后给出统一错误
TEST(CloudStorageCovTest, UnknownLinkReportsNoPlugin) {
    CloudStorageManager manager;

    auto result = manager.handle_share_link("https://example.com/file.zip");
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.recognized);
    EXPECT_EQ(result.error_message, "未找到对应的网盘插件");
    EXPECT_EQ(result.platform_type, CloudPlatform::Unknown);
    EXPECT_TRUE(result.files.empty());
}
