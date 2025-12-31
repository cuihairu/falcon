// Falcon Download Integration Tests
// Copyright (c) 2025 Falcon Project

#include <falcon/download_engine.hpp>
#include <falcon/download_options.hpp>
#include <falcon/download_engine_v2.hpp>
#include <gtest/gtest.h>

#include <fstream>
#include <filesystem>
#include <thread>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace falcon;

namespace {

bool env_truthy(const char* name) {
    const char* value = std::getenv(name);
    if (!value) return false;
    return std::string(value) == "1" || std::string(value) == "true" || std::string(value) == "TRUE";
}

class LocalHttpServer {
public:
    LocalHttpServer() = default;
    ~LocalHttpServer() { stop(); }

    LocalHttpServer(const LocalHttpServer&) = delete;
    LocalHttpServer& operator=(const LocalHttpServer&) = delete;

    void add_file(std::string path, std::vector<std::uint8_t> data, std::string content_type) {
        if (path.empty() || path[0] != '/') {
            path.insert(path.begin(), '/');
        }
        routes_[std::move(path)] = Route{std::move(data), std::move(content_type)};
    }

    bool start() {
        stop();

#ifdef _WIN32
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
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
        addr.sin_port = 0;  // ephemeral

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
            close_socket(listen_fd_);
            listen_fd_ = -1;
        }

        if (accept_thread_.joinable()) {
            accept_thread_.join();
        }

        for (auto& t : client_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        client_threads_.clear();

        cleanup_winsock();
    }

    std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_);
    }

private:
    struct Route {
        std::vector<std::uint8_t> data;
        std::string content_type;
    };

    static void close_socket(int fd) {
#ifdef _WIN32
        ::closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
    }

    static bool send_all(int fd, const char* data, std::size_t len) {
        while (len > 0) {
#ifdef _WIN32
            const int chunk = static_cast<int>(std::min<std::size_t>(len, static_cast<std::size_t>(INT_MAX)));
            const int sent = ::send(static_cast<SOCKET>(fd), data, chunk, 0);
            if (sent <= 0) {
                return false;
            }
            data += sent;
            len -= static_cast<std::size_t>(sent);
#else
            const ssize_t sent = ::send(fd, data, len, 0);
            if (sent <= 0) {
                return false;
            }
            data += static_cast<std::size_t>(sent);
            len -= static_cast<std::size_t>(sent);
#endif
        }
        return true;
    }

    static void cleanup_winsock() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    void accept_loop() {
        while (running_.load()) {
            sockaddr_in client{};
            socklen_t len = sizeof(client);
            int fd = static_cast<int>(::accept(listen_fd_, reinterpret_cast<sockaddr*>(&client), &len));
            if (fd < 0) {
                if (running_.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                continue;
            }

            client_threads_.emplace_back([this, fd] { handle_client(fd); });
        }
    }

    static bool recv_until(int fd, std::string& out, const char* needle, std::size_t max_bytes) {
        out.clear();
        while (out.size() < max_bytes) {
            char buf[2048];
#ifdef _WIN32
            int n = ::recv(static_cast<SOCKET>(fd), buf, static_cast<int>(sizeof(buf)), 0);
#else
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
#endif
            if (n <= 0) {
                return false;
            }
            out.append(buf, static_cast<std::size_t>(n));
            if (out.find(needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

    static std::string trim(std::string s) {
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) {
            s.pop_back();
        }
        std::size_t i = 0;
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) {
            ++i;
        }
        s.erase(0, i);
        return s;
    }

    static bool parse_range_header(const std::string& value, std::size_t total, std::size_t& start, std::size_t& end) {
        // Expect: bytes=start-end or bytes=start-
        if (value.rfind("bytes=", 0) != 0) {
            return false;
        }
        std::string spec = value.substr(6);
        auto dash = spec.find('-');
        if (dash == std::string::npos) {
            return false;
        }

        std::string start_str = spec.substr(0, dash);
        std::string end_str = spec.substr(dash + 1);
        if (start_str.empty()) {
            return false;
        }

        std::size_t s = static_cast<std::size_t>(std::stoull(start_str));
        std::size_t e = total > 0 ? total - 1 : 0;
        if (!end_str.empty()) {
            e = static_cast<std::size_t>(std::stoull(end_str));
        }

        if (s >= total) {
            return false;
        }
        if (e >= total) {
            e = total - 1;
        }
        if (e < s) {
            return false;
        }

        start = s;
        end = e;
        return true;
    }

    void handle_client(int fd) {
        std::string req;
        if (!recv_until(fd, req, "\r\n\r\n", 64 * 1024)) {
            close_socket(fd);
            return;
        }

        // Parse request line
        std::string method;
        std::string path;
        {
            auto line_end = req.find("\r\n");
            if (line_end == std::string::npos) {
                close_socket(fd);
                return;
            }
            std::string request_line = req.substr(0, line_end);
            std::istringstream iss(request_line);
            iss >> method >> path;
        }

        // Parse headers
        std::unordered_map<std::string, std::string> headers;
        {
            std::size_t pos = req.find("\r\n") + 2;
            while (pos < req.size()) {
                auto next = req.find("\r\n", pos);
                if (next == std::string::npos) break;
                if (next == pos) break;  // blank line
                std::string line = req.substr(pos, next - pos);
                pos = next + 2;
                auto colon = line.find(':');
                if (colon == std::string::npos) continue;
                std::string key = line.substr(0, colon);
                std::string val = trim(line.substr(colon + 1));
                for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                headers[key] = val;
            }
        }

        auto route_it = routes_.find(path);
        if (route_it == routes_.end()) {
            const char* resp = "HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            (void)send_all(fd, resp, std::strlen(resp));
            close_socket(fd);
            return;
        }

        const Route& route = route_it->second;
        const std::size_t total = route.data.size();

        bool has_range = false;
        std::size_t range_start = 0;
        std::size_t range_end = total > 0 ? total - 1 : 0;

        auto it = headers.find("range");
        if (it != headers.end()) {
            has_range = parse_range_header(it->second, total, range_start, range_end);
        }

        const bool is_head = (method == "HEAD");
        const bool is_get = (method == "GET");
        if (!is_head && !is_get) {
            const char* resp = "HTTP/1.1 405 Method Not Allowed\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            (void)send_all(fd, resp, std::strlen(resp));
            close_socket(fd);
            return;
        }

        std::ostringstream oss;
        if (has_range) {
            const std::size_t len_bytes = range_end - range_start + 1;
            oss << "HTTP/1.1 206 Partial Content\r\n";
            oss << "Content-Length: " << len_bytes << "\r\n";
            oss << "Content-Range: bytes " << range_start << "-" << range_end << "/" << total << "\r\n";
        } else {
            oss << "HTTP/1.1 200 OK\r\n";
            oss << "Content-Length: " << total << "\r\n";
        }
        oss << "Content-Type: " << route.content_type << "\r\n";
        oss << "Accept-Ranges: bytes\r\n";
        oss << "Connection: close\r\n\r\n";

        std::string header_blob = oss.str();
        (void)send_all(fd, header_blob.data(), header_blob.size());

        if (!is_head && total > 0) {
            const std::uint8_t* begin = route.data.data() + (has_range ? range_start : 0);
            std::size_t to_send = has_range ? (range_end - range_start + 1) : total;
            (void)send_all(fd, reinterpret_cast<const char*>(begin), to_send);
        }

        close_socket(fd);
    }

    std::atomic<bool> running_{false};
    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::thread accept_thread_;
    std::vector<std::thread> client_threads_;
    std::unordered_map<std::string, Route> routes_;
};

} // namespace

class DownloadIntegrationTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Create test directory
        test_dir_ = std::filesystem::temp_directory_path() / "falcon_test";
        std::filesystem::create_directories(test_dir_);
    }

    void TearDown() override {
        // Clean up test directory
        std::error_code ec;
        std::filesystem::remove_all(test_dir_, ec);
    }

    std::filesystem::path test_dir_;
};

TEST_F(DownloadIntegrationTest, LocalHttpDownloadFile) {
    LocalHttpServer server;
    const std::string payload = R"({"hello":"world"})";
    server.add_file("/test.json",
                    std::vector<std::uint8_t>(payload.begin(), payload.end()),
                    "application/json");
    ASSERT_TRUE(server.start());

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "test_download.json";
    options.max_connections = 1;
    options.timeout_seconds = 10;
    options.resume_enabled = false;

    std::string url = server.base_url() + "/test.json";
    auto task = engine.add_task(url, options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    ASSERT_TRUE(task->wait_for(std::chrono::seconds(20)));
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    std::filesystem::path output_file = test_dir_ / "test_download.json";
    ASSERT_TRUE(std::filesystem::exists(output_file));

    std::ifstream file(output_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_NE(content.find("\"hello\""), std::string::npos);
}

TEST_F(DownloadIntegrationTest, LocalHttpSegmentedDownloadFile) {
    LocalHttpServer server;

    std::vector<std::uint8_t> payload(512 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i % 251);
    }

    server.add_file("/blob.bin", payload, "application/octet-stream");
    ASSERT_TRUE(server.start());

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "blob.bin";
    options.max_connections = 4;
    options.min_segment_size = 64 * 1024;
    options.timeout_seconds = 10;
    options.resume_enabled = false;

    std::string url = server.base_url() + "/blob.bin";
    auto task = engine.add_task(url, options);
    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    ASSERT_TRUE(task->wait_for(std::chrono::seconds(20)));
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    std::filesystem::path output_file = test_dir_ / "blob.bin";
    ASSERT_TRUE(std::filesystem::exists(output_file));
    EXPECT_EQ(std::filesystem::file_size(output_file), payload.size());

    std::ifstream file(output_file, std::ios::binary);
    std::vector<std::uint8_t> downloaded((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());

    ASSERT_EQ(downloaded.size(), payload.size());
    EXPECT_EQ(std::memcmp(downloaded.data(), payload.data(), payload.size()), 0);
}

TEST_F(DownloadIntegrationTest, LocalHttpDownloadFileV2) {
    LocalHttpServer server;
    const std::string payload = R"({"hello":"world"})";
    server.add_file("/test.json",
                    std::vector<std::uint8_t>(payload.begin(), payload.end()),
                    "application/json");
    ASSERT_TRUE(server.start());

    EngineConfigV2 config;
    config.max_concurrent_tasks = 2;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "v2_test.json";
    options.max_connections = 1;
    options.timeout_seconds = 10;
    options.resume_enabled = false;

    TaskId task_id = engine.add_download(server.base_url() + "/test.json", options);
    engine.run();

    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    std::filesystem::path output_file = test_dir_ / "v2_test.json";
    ASSERT_TRUE(std::filesystem::exists(output_file));
}

TEST_F(DownloadIntegrationTest, LocalHttpSegmentedDownloadFileV2) {
    LocalHttpServer server;

    std::vector<std::uint8_t> payload(512 * 1024);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i % 251);
    }

    server.add_file("/blob.bin", payload, "application/octet-stream");
    ASSERT_TRUE(server.start());

    EngineConfigV2 config;
    config.max_concurrent_tasks = 2;
    config.poll_timeout_ms = 10;
    DownloadEngineV2 engine(config);

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "v2_blob.bin";
    options.max_connections = 4;
    options.min_segment_size = 64 * 1024;
    options.timeout_seconds = 10;
    options.resume_enabled = false;

    TaskId task_id = engine.add_download(server.base_url() + "/blob.bin", options);
    engine.run();

    auto* group = engine.request_group_man()->find_group(task_id);
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(group->status(), RequestGroupStatus::COMPLETED);

    std::filesystem::path output_file = test_dir_ / "v2_blob.bin";
    ASSERT_TRUE(std::filesystem::exists(output_file));
    EXPECT_EQ(std::filesystem::file_size(output_file), payload.size());

    std::ifstream file(output_file, std::ios::binary);
    std::vector<std::uint8_t> downloaded((std::istreambuf_iterator<char>(file)),
                                         std::istreambuf_iterator<char>());
    ASSERT_EQ(downloaded.size(), payload.size());
    EXPECT_EQ(std::memcmp(downloaded.data(), payload.data(), payload.size()), 0);
}

TEST_F(DownloadIntegrationTest, HttpDownloadFile) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "test_download.json";
    options.max_connections = 1;
    options.timeout_seconds = 30;
    options.resume_enabled = false;

    // Test with httpbin.org
    std::string url = "https://httpbin.org/json";
    auto task = engine.add_task(url, options);

    ASSERT_NE(task, nullptr);
    EXPECT_EQ(task->id(), 1);
    EXPECT_EQ(task->url(), url);
    EXPECT_EQ(task->status(), TaskStatus::Pending);

    ASSERT_TRUE(engine.start_task(task->id()));
    ASSERT_TRUE(task->wait_for(std::chrono::seconds(60)));

    // Check result
    EXPECT_EQ(task->status(), TaskStatus::Completed);

    // Verify file exists
    std::filesystem::path output_file = test_dir_ / "test_download.json";
    EXPECT_TRUE(std::filesystem::exists(output_file));

    // Check file size (httpbin.org/json returns known content)
    auto file_size = std::filesystem::file_size(output_file);
    EXPECT_GT(file_size, 0);

    // Verify content
    std::ifstream file(output_file);
    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    EXPECT_FALSE(content.empty());
    EXPECT_TRUE(content.find("\"slideshow\"") != std::string::npos);
}

TEST_F(DownloadIntegrationTest, MultipleDownloads) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.max_connections = 2;
    options.timeout_seconds = 30;
    options.resume_enabled = false;

    std::vector<std::string> urls = {
        "https://httpbin.org/uuid",
        "https://httpbin.org/ip",
        "https://httpbin.org/user-agent"
    };

    std::vector<std::string> filenames = {
        "uuid.json",
        "ip.json",
        "user-agent.json"
    };

    std::vector<DownloadTask::Ptr> tasks;

    // Add multiple tasks
    for (size_t i = 0; i < urls.size(); ++i) {
        options.output_filename = filenames[i];
        auto task = engine.add_task(urls[i], options);
        ASSERT_NE(task, nullptr);
        ASSERT_TRUE(engine.start_task(task->id()));
        tasks.push_back(task);
    }

    // Wait for all to complete
    for (auto& task : tasks) {
        ASSERT_TRUE(task->wait_for(std::chrono::seconds(60)));
        EXPECT_EQ(task->status(), TaskStatus::Completed);

        // Verify file exists
        std::filesystem::path output_file = test_dir_ / filenames[task->id() - 1];
        EXPECT_TRUE(std::filesystem::exists(output_file));
        EXPECT_GT(std::filesystem::file_size(output_file), 0);
    }
}

TEST_F(DownloadIntegrationTest, PauseAndResume) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "pause_test.bin";
    options.max_connections = 1;
    options.timeout_seconds = 30;
    options.resume_enabled = true;

    // Use a larger file for pause/resume test
    std::string url = "https://httpbin.org/bytes/1024"; // 1KB file
    auto task = engine.add_task(url, options);

    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    // Wait a bit then pause
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(engine.pause_task(task->id()));
    EXPECT_EQ(task->status(), TaskStatus::Paused);

    // Verify partial file exists
    std::filesystem::path output_file = test_dir_ / "pause_test.bin";
    if (std::filesystem::exists(output_file)) {
        auto partial_size = std::filesystem::file_size(output_file);
        EXPECT_GT(partial_size, 0);
        EXPECT_LT(partial_size, 1024);
    }

    // Resume
    EXPECT_TRUE(engine.resume_task(task->id()));

    // Wait for completion
    ASSERT_TRUE(task->wait_for(std::chrono::seconds(60)));

    EXPECT_EQ(task->status(), TaskStatus::Completed);
    EXPECT_TRUE(std::filesystem::exists(output_file));
    EXPECT_EQ(std::filesystem::file_size(output_file), 1024);
}

TEST_F(DownloadIntegrationTest, CancelDownload) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.output_filename = "cancel_test.bin";
    options.max_connections = 1;
    options.timeout_seconds = 30;

    // Use a large file
    std::string url = "https://httpbin.org/bytes/1048576"; // 1MB file
    auto task = engine.add_task(url, options);

    ASSERT_NE(task, nullptr);
    ASSERT_TRUE(engine.start_task(task->id()));

    // Wait a bit then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_TRUE(engine.cancel_task(task->id()));
    EXPECT_EQ(task->status(), TaskStatus::Cancelled);

    // File might exist but should be incomplete
    std::filesystem::path output_file = test_dir_ / "cancel_test.bin";
    if (std::filesystem::exists(output_file)) {
        auto file_size = std::filesystem::file_size(output_file);
        EXPECT_LT(file_size, 1048576);
    }
}

TEST_F(DownloadIntegrationTest, GetStatistics) {
    if (!env_truthy("FALCON_RUN_NETWORK_TESTS")) {
        GTEST_SKIP() << "Set FALCON_RUN_NETWORK_TESTS=1 to enable external network tests";
    }

    DownloadEngine engine;

    DownloadOptions options;
    options.output_directory = test_dir_.string();
    options.max_connections = 2;
    options.timeout_seconds = 30;

    // Add some tasks
    std::vector<std::string> urls = {
        "https://httpbin.org/uuid",
        "https://httpbin.org/ip",
        "https://httpbin.org/user-agent"
    };

    for (const auto& url : urls) {
        auto task = engine.add_task(url, options);
        ASSERT_NE(task, nullptr);
    }

    // Get statistics
    auto tasks = engine.get_all_tasks();
    EXPECT_EQ(tasks.size(), 3);

    auto active_tasks = engine.get_active_tasks();
    EXPECT_GE(active_tasks.size(), 0);

    auto total_speed = engine.get_total_speed();
    EXPECT_GE(total_speed, 0);
}
