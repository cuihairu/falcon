/**
 * @file resource_search_coverage_test.cpp
 * @brief 资源搜索覆盖率补充测试（离线逻辑 + 本地回环 HTTP 服务器）
 * @author Falcon Team
 * @date 2026-09-05
 */

#include <gtest/gtest.h>
#include <falcon/drives/resource_search.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <functional>
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

using namespace falcon::search;
using falcon::search::detail::apply_selector_field;
using falcon::search::detail::parse_html_by_selectors;
using falcon::search::detail::parse_magnet_link;
using falcon::search::detail::parse_size;
using falcon::search::detail::url_decode;
using falcon::search::detail::validate_url;

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

unsigned long long current_pid() {
#ifdef _WIN32
    return static_cast<unsigned long long>(::GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(::getpid());
#endif
}

class TempFile {
public:
    TempFile(const std::string& content) {
        static std::atomic<unsigned long long> seq{0};
        path_ = "falcon_cov_cfg_" + std::to_string(current_pid()) + "_" +
                std::to_string(seq.fetch_add(1)) + ".json";
        std::ofstream out(path_, std::ios::trunc);
        out << content;
    }
    ~TempFile() { std::remove(path_.c_str()); }
    TempFile(const TempFile&) = delete;
    TempFile& operator=(const TempFile&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

class CovMockProvider : public ISearchProvider {
public:
    CovMockProvider(std::string name,
                    std::vector<SearchResult> results,
                    bool available = true,
                    bool throw_on_search = false)
        : name_(std::move(name)),
          results_(std::move(results)),
          available_(available),
          throw_on_search_(throw_on_search) {}

    std::string name() const override { return name_; }

    std::vector<SearchResult> search(const SearchQuery&) override {
        if (throw_on_search_) {
            throw std::runtime_error("provider failure: " + name_);
        }
        return results_;
    }

    bool validate_url(const std::string& url) override {
        return url.find("magnet:") == 0 || url.find("http") == 0;
    }

    SearchResult get_details(const std::string& url) override {
        SearchResult r;
        r.url = url;
        r.source = name_;
        return r;
    }

    bool is_available() override { return available_; }
    int get_delay() const override { return 0; }

private:
    std::string name_;
    std::vector<SearchResult> results_;
    bool available_;
    bool throw_on_search_;
};

SearchResult make_result(const std::string& title,
                         const std::string& url,
                         const std::string& hash,
                         size_t size,
                         int seeds,
                         double confidence,
                         const std::string& publish_date = "") {
    SearchResult r;
    r.title = title;
    r.url = url;
    r.hash = hash;
    r.size = size;
    r.seeds = seeds;
    r.confidence = confidence;
    r.publish_date = publish_date;
    r.source = "CovMock";
    return r;
}

const char* kJsonResultsBody = R"({"results":[
    {"title":"Ubuntu 24.04 ISO","url":"magnet:?xt=urn:btih:aaaa","hash":"hashAAAA","size":5368709120,"seeds":120,"leeches":15,"type":"iso","source":"LocalJson"},
    {"title":"Big Pack","url":"magnet:?xt=urn:btih:bbbb","hash":"hashBBBB","size":16106127360,"seeds":60,"leeches":7,"type":"pack","source":"LocalJson"},
    {"title":"Tiny Doc","url":"magnet:?xt=urn:btih:cccc","hash":"hashCCCC","size":1048576,"seeds":5,"leeches":1,"type":"doc","source":"LocalJson"}
]})";

const char* kJsonArrayBody = R"([
    {"title":"Array Item One","magnet":"magnet:?xt=urn:btih:dddd","url":"https://fallback.example/one","hash":"hashDDDD","size":2097152,"seeds":33,"leeches":4,"type":"magnet","source":"LocalArray"},
    {"title":"Array Item Two","url":"https://array.example/two.torrent","hash":"hashEEEE","size":3145728,"seeds":21,"leeches":2,"type":"torrent","source":"LocalArray"},
    {"url":"https://array.example/missing-title"}
])";

const char* kJsonSingleBody = R"({"title":"Single Object Item","url":"https://single.example/s.iso","hash":"hashSSSS","size":4096,"seeds":9,"leeches":3,"type":"iso","source":"LocalSingle"})";

const char* kBadJsonBody = "this is definitely { not valid json";

const char* kTypeErrBody = R"({"results":[{"title":"BadType","url":"magnet:?xt=urn:btih:eeee","size":"not-a-number"}]})";

const char* kHtmlItemsBody = R"(<ul>
<li><b>Cov Title One</b> <span class="sz">2GB</span> <span class="sd">77</span> <a href="magnet:?xt=urn:btih:cov1">get</a></li>
<li><b>Cov Title Two</b> <span class="sz">512KB</span> <span class="sd">12</span> <a href="magnet:?xt=urn:btih:cov2">get</a></li>
</ul>)";

std::string build_engine_config(const std::string& base_url,
                                const std::string& search_path,
                                const std::string& response_format,
                                const std::map<std::string, std::string>& params,
                                const std::map<std::string, std::string>& selectors = {},
                                const std::string& path_pattern = "",
                                const std::string& engine_name = "LocalEngine") {
    nlohmann::json engine;
    engine["name"] = engine_name;
    engine["base_url"] = base_url;
    engine["search_path"] = search_path;
    engine["enabled"] = true;
    engine["delay_ms"] = 0;
    if (!response_format.empty()) engine["response_format"] = response_format;
    if (!params.empty()) engine["params"] = params;
    if (!selectors.empty()) engine["selectors"] = selectors;
    if (!path_pattern.empty()) engine["path_pattern"] = path_pattern;

    nlohmann::json config;
    config["global_settings"] = {{"default_delay_ms", 0}};
    config["search_engines"] = nlohmann::json::array({engine});
    return config.dump();
}

} // namespace

// ============================================================================
// detail 工具函数补充覆盖
// ============================================================================

TEST(ResourceSearchCovDetailTest, ParseSizeAllUnits) {
    EXPECT_EQ(parse_size("1024B"), 1024u);
    EXPECT_EQ(parse_size("1.5KB"), 1536u);
    EXPECT_EQ(parse_size("2 MB"), 2u * 1024u * 1024u);
    EXPECT_EQ(parse_size("3gb"), 3ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size("1TB"), 1024ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(parse_size("size: 4.5GB total"), 4ULL * 1024ULL * 1024ULL * 1024ULL + 512ULL * 1024ULL * 1024ULL);
}

TEST(ResourceSearchCovDetailTest, ParseSizeInvalidInputs) {
    EXPECT_EQ(parse_size(""), 0u);
    EXPECT_EQ(parse_size("no digits here"), 0u);
    EXPECT_EQ(parse_size("100"), 0u);
    EXPECT_EQ(parse_size("12 XY"), 0u);
}

TEST(ResourceSearchCovDetailTest, ApplySelectorMagnetSizeLeeches) {
    SearchResult r;
    apply_selector_field(r, "magnet", "magnet:?xt=urn:btih:zzzz");
    EXPECT_EQ(r.url, "magnet:?xt=urn:btih:zzzz");
    EXPECT_EQ(r.type, "magnet");

    r.type = "video";
    apply_selector_field(r, "magnet", "magnet:?xt=urn:btih:yyyy");
    EXPECT_EQ(r.type, "video");

    apply_selector_field(r, "size", "2GB");
    EXPECT_EQ(r.size, 2ULL * 1024ULL * 1024ULL * 1024ULL);

    apply_selector_field(r, "leeches", "7");
    EXPECT_EQ(r.peers, 7);

    apply_selector_field(r, "leeches", "not-a-number");
    EXPECT_EQ(r.peers, 7);
}

TEST(ResourceSearchCovDetailTest, ParseHtmlInvalidItemRegexReturnsEmpty) {
    SearchEngineConfig config;
    config.name = "BadItemEngine";
    config.selectors = {
        {"item", "([unclosed-character-class"},
        {"title", R"re(<b>([^<]+)</b>)re"}
    };

    auto results = parse_html_by_selectors("<li><b>X</b></li>", config);
    EXPECT_TRUE(results.empty());
}

TEST(ResourceSearchCovDetailTest, ParseHtmlSkipsEmptySelectorValues) {
    SearchEngineConfig config;
    config.name = "EmptySelectorEngine";
    config.selectors = {
        {"item", R"re(<li>[\s\S]*?</li>)re"},
        {"title", ""}
    };

    auto results = parse_html_by_selectors("<li><b>X</b></li>", config);
    EXPECT_TRUE(results.empty());
}

// ============================================================================
// ResourceSearchManager 补充覆盖（mock provider，无网络）
// ============================================================================

class ResourceSearchCovManagerTest : public ::testing::Test {
protected:
    std::vector<SearchResult> sorted_fixtures() {
        std::vector<SearchResult> results;
        results.push_back(make_result("Alpha", "magnet:?xt=a", "ha", 300, 10, 0.5, "2024-01-01"));
        results.push_back(make_result("Beta", "magnet:?xt=b", "hb", 100, 30, 0.9, "2026-05-01"));
        results.push_back(make_result("Gamma", "magnet:?xt=c", "hc", 200, 20, 0.7, "2025-12-31"));
        return results;
    }
};

TEST_F(ResourceSearchCovManagerTest, SearchAllSortsBySizeDescAndLimits) {
    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<CovMockProvider>("Sizer", sorted_fixtures()));

    SearchQuery query;
    query.keyword = "any";
    query.sort_by = "size";
    query.sort_desc = true;
    query.limit = 2;

    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].title, "Alpha");
    EXPECT_EQ(results[1].title, "Gamma");
}

TEST_F(ResourceSearchCovManagerTest, SearchAllSortsBySeedsAsc) {
    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<CovMockProvider>("Seeder", sorted_fixtures()));

    SearchQuery query;
    query.keyword = "any";
    query.sort_by = "seeds";
    query.sort_desc = false;

    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 3u);
    EXPECT_EQ(results[0].seeds, 10);
    EXPECT_EQ(results[1].seeds, 20);
    EXPECT_EQ(results[2].seeds, 30);
}

TEST_F(ResourceSearchCovManagerTest, SearchAllSortsByDateAndDefaultConfidence) {
    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<CovMockProvider>("Dater", sorted_fixtures()));

    SearchQuery by_date;
    by_date.keyword = "any";
    by_date.sort_by = "date";
    by_date.sort_desc = true;
    auto dated = manager.search_all(by_date);
    ASSERT_EQ(dated.size(), 3u);
    EXPECT_EQ(dated[0].publish_date, "2026-05-01");
    EXPECT_EQ(dated[1].publish_date, "2025-12-31");
    EXPECT_EQ(dated[2].publish_date, "2024-01-01");

    SearchQuery by_confidence;
    by_confidence.keyword = "any";
    auto ranked = manager.search_all(by_confidence);
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0].title, "Beta");
    EXPECT_EQ(ranked[1].title, "Gamma");
    EXPECT_EQ(ranked[2].title, "Alpha");
}

TEST_F(ResourceSearchCovManagerTest, SearchAllDeduplicatesByHashAndUrl) {
    std::vector<SearchResult> first;
    first.push_back(make_result("SameHash1", "magnet:?xt=1", "dup-hash", 100, 1, 0.9));
    first.push_back(make_result("UniqueUrl", "https://u.example/one", "", 200, 2, 0.6));

    std::vector<SearchResult> second;
    second.push_back(make_result("SameHash2", "magnet:?xt=2", "dup-hash", 300, 3, 0.8));
    second.push_back(make_result("SameUrlAgain", "https://u.example/one", "", 400, 4, 0.7));

    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<CovMockProvider>("Dedup1", first));
    manager.register_provider(std::make_unique<CovMockProvider>("Dedup2", second));

    SearchQuery query;
    query.keyword = "any";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].title, "SameHash1");
    EXPECT_EQ(results[1].title, "UniqueUrl");
}

TEST_F(ResourceSearchCovManagerTest, SearchAllHandlesThrowingProvider) {
    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<CovMockProvider>(
        "Thrower", std::vector<SearchResult>(), true, true));
    manager.register_provider(std::make_unique<CovMockProvider>("Healthy", sorted_fixtures()));

    SearchQuery query;
    query.keyword = "any";
    auto results = manager.search_all(query);
    EXPECT_EQ(results.size(), 3u);
}

TEST_F(ResourceSearchCovManagerTest, SearchAllSkipsUnavailableProvider) {
    ResourceSearchManager manager;
    manager.register_provider(
        std::make_unique<CovMockProvider>("Offline", std::vector<SearchResult>(), false, false));
    manager.register_provider(std::make_unique<CovMockProvider>("Online", sorted_fixtures()));

    SearchQuery query;
    query.keyword = "any";
    auto results = manager.search_all(query);
    EXPECT_EQ(results.size(), 3u);
    for (const auto& r : results) {
        EXPECT_EQ(r.source, "CovMock");
    }
}

TEST_F(ResourceSearchCovManagerTest, SearchProvidersSelectsByName) {
    std::vector<SearchResult> a;
    a.push_back(make_result("FromA", "magnet:?xt=a1", "", 1, 1, 0.5));
    std::vector<SearchResult> b;
    b.push_back(make_result("FromB", "magnet:?xt=b1", "", 2, 2, 0.5));
    std::vector<SearchResult> c;
    c.push_back(make_result("FromC", "magnet:?xt=c1", "", 3, 3, 0.5));

    ResourceSearchManager manager;
    manager.register_provider(std::make_unique<CovMockProvider>("EngineA", a));
    manager.register_provider(std::make_unique<CovMockProvider>("EngineB", b));
    manager.register_provider(std::make_unique<CovMockProvider>("EngineC", c));

    auto results = manager.search_providers(SearchQuery{}, {"EngineA", "EngineC", "NoSuchEngine"});
    ASSERT_EQ(results.size(), 2u);

    bool has_a = false;
    bool has_c = false;
    for (const auto& r : results) {
        if (r.title == "FromA") has_a = true;
        if (r.title == "FromC") has_c = true;
    }
    EXPECT_TRUE(has_a);
    EXPECT_TRUE(has_c);
}

TEST_F(ResourceSearchCovManagerTest, GetSuggestionsAppendsSuffixes) {
    ResourceSearchManager manager;
    auto suggestions = manager.get_suggestions("ubuntu");
    EXPECT_EQ(suggestions.size(), 17u);
    for (const auto& s : suggestions) {
        EXPECT_EQ(s.find("ubuntu"), 0u);
    }
    EXPECT_EQ(suggestions[0], "ubuntu 1080p");
    EXPECT_NE(std::find(suggestions.begin(), suggestions.end(), std::string("ubuntu x265")),
              suggestions.end());
}

TEST_F(ResourceSearchCovManagerTest, DelayEnableAndNullRegisterAreSafe) {
    ResourceSearchManager manager;
    manager.set_global_delay(321);

    TempFile config(build_engine_config("https://offline.example", "/s", "json", {}));
    ASSERT_TRUE(manager.load_config(config.path()));

    manager.enable_provider("LocalEngine", false);
    manager.enable_provider("UnknownEngine", true);

    manager.register_provider(nullptr);
    EXPECT_EQ(manager.get_providers().size(), 1u);
}

// ============================================================================
// load_config 补充覆盖（代理/参数/selectors/path_pattern 解析）
// ============================================================================

TEST(ResourceSearchCovConfigTest, LoadConfigParsesProxyAndAdvancedFields) {
    nlohmann::json engine;
    engine["name"] = "AdvEngine";
    engine["base_url"] = "https://adv.example";
    engine["search_path"] = "/search";
    engine["response_format"] = "html";
    engine["params"] = nlohmann::json{{"q", ""}, {"lang", "en"}};
    engine["selectors"] = nlohmann::json{{"item", "<li>[\\s\\S]*?</li>"},
                                         {"title", "<b>([^<]+)</b>"}};
    engine["path_pattern"] = "/p/{query}";
    engine["headers"] = nlohmann::json{{"User-Agent", "CovTest"}};

    nlohmann::json config;
    config["global_settings"] = nlohmann::json{
        {"default_delay_ms", 100},
        {"proxy", nlohmann::json{{"enabled", true},
                                 {"host", "proxy.local"},
                                 {"port", 7890},
                                 {"type", "socks5"},
                                 {"username", "u1"},
                                 {"password", "p1"}}}
    };
    config["search_engines"] = nlohmann::json::array({engine});

    TempFile file(config.dump());
    ResourceSearchManager manager;
    EXPECT_TRUE(manager.load_config(file.path()));

    auto providers = manager.get_providers();
    ASSERT_EQ(providers.size(), 1u);
    EXPECT_EQ(providers[0], "AdvEngine");
}

TEST(ResourceSearchCovConfigTest, LoadConfigInvalidFieldTypeFails) {
    nlohmann::json engine;
    engine["name"] = 42;  // name 必须是字符串，类型错误触发异常并被捕获
    engine["base_url"] = "https://broken.example";

    nlohmann::json config;
    config["search_engines"] = nlohmann::json::array({engine});

    TempFile file(config.dump());
    ResourceSearchManager manager;
    EXPECT_FALSE(manager.load_config(file.path()));
    EXPECT_TRUE(manager.get_providers().empty());
}

// ============================================================================
// 本地回环 HTTP 服务器驱动的 GenericSearchProvider 覆盖
// ============================================================================

class ResourceSearchCovNetTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(server_.start());
        server_.set_handler([](const HttpRequest& req) { return route(req); });
    }

    void TearDown() override { server_.stop(); }

    static std::string route(const HttpRequest& req) {
        if (req.path == "/search") return kJsonResultsBody;
        if (req.path == "/array") return kJsonArrayBody;
        if (req.path == "/single") return kJsonSingleBody;
        if (req.path == "/badjson") return kBadJsonBody;
        if (req.path == "/typeerr") return kTypeErrBody;
        if (req.path == "/empty") return "";
        if (req.path == "/items") return kHtmlItemsBody;
        if (req.path.rfind("/find/", 0) == 0) return kJsonArrayBody;
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
};

TEST_F(ResourceSearchCovNetTest, SearchParsesJsonObjectResponse) {
    TempFile config(build_engine_config(server_.base_url(), "/search", "json",
                                        {{"q", ""}, {"sort", "new"}}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "ubuntu";
    query.limit = 10;
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 3u);

    bool found_ubuntu = false;
    for (const auto& r : results) {
        if (r.title == "Ubuntu 24.04 ISO") {
            found_ubuntu = true;
            EXPECT_EQ(r.size, 5368709120u);
            EXPECT_EQ(r.seeds, 120);
            EXPECT_EQ(r.peers, 15);
            EXPECT_EQ(r.type, "iso");
            EXPECT_EQ(r.source, "LocalJson");
        }
    }
    EXPECT_TRUE(found_ubuntu);

    EXPECT_TRUE(saw_request(server_.requests(), "GET /search?q=ubuntu&sort=new"));
}

TEST_F(ResourceSearchCovNetTest, SearchAppliesFiltersFromQuery) {
    TempFile config(build_engine_config(server_.base_url(), "/search", "json",
                                        {{"q", ""}}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "ubuntu";
    query.min_seeds = 50;
    query.sort_by = "seeds";
    query.sort_desc = true;
    query.limit = 2;
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].seeds, 120);
    EXPECT_EQ(results[1].seeds, 60);
}

TEST_F(ResourceSearchCovNetTest, SearchParsesJsonArrayAndMagnetOverride) {
    TempFile config(build_engine_config(server_.base_url(), "/array", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "list";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);

    bool found_override = false;
    for (const auto& r : results) {
        if (r.title == "Array Item One") {
            found_override = true;
            EXPECT_EQ(r.url, "magnet:?xt=urn:btih:dddd");
            EXPECT_EQ(r.hash, "hashDDDD");
            EXPECT_EQ(r.size, 2097152u);
            EXPECT_EQ(r.seeds, 33);
            EXPECT_EQ(r.peers, 4);
        }
    }
    EXPECT_TRUE(found_override);
}

TEST_F(ResourceSearchCovNetTest, SearchParsesSingleObjectResponse) {
    TempFile config(build_engine_config(server_.base_url(), "/single", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "one";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].title, "Single Object Item");
    EXPECT_EQ(results[0].url, "https://single.example/s.iso");
    EXPECT_EQ(results[0].size, 4096u);
    EXPECT_EQ(results[0].seeds, 9);
    EXPECT_EQ(results[0].peers, 3);
    EXPECT_EQ(results[0].source, "LocalSingle");
}

TEST_F(ResourceSearchCovNetTest, SearchHandlesBadJsonAndTypeErrorResponses) {
    nlohmann::json bad;
    bad["name"] = "BadJsonEngine";
    bad["base_url"] = server_.base_url();
    bad["search_path"] = "/badjson";
    bad["response_format"] = "json";
    bad["delay_ms"] = 0;

    nlohmann::json type_err;
    type_err["name"] = "TypeErrorEngine";
    type_err["base_url"] = server_.base_url();
    type_err["search_path"] = "/typeerr";
    type_err["response_format"] = "json";
    type_err["delay_ms"] = 0;

    nlohmann::json config;
    config["search_engines"] = nlohmann::json::array({bad, type_err});

    TempFile file(config.dump());
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(file.path()));

    SearchQuery query;
    query.keyword = "x";
    auto results = manager.search_all(query);
    EXPECT_TRUE(results.empty());
}

TEST_F(ResourceSearchCovNetTest, SearchParsesHtmlWithSelectors) {
    TempFile config(build_engine_config(
        server_.base_url(), "/items", "html", {},
        {{"item", R"re(<li>[\s\S]*?</li>)re"},
         {"title", R"re(<b>([^<]+)</b>)re"},
         {"magnet", R"re(href="(magnet:[^"]+)")re"},
         {"size", R"re(class="sz">([^<]+)<)re"},
         {"seeds", R"re(class="sd">(\d+)<)re"}}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "html";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);

    EXPECT_EQ(results[0].title, "Cov Title One");
    EXPECT_EQ(results[0].url, "magnet:?xt=urn:btih:cov1");
    EXPECT_EQ(results[0].type, "magnet");
    EXPECT_EQ(results[0].size, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    EXPECT_EQ(results[0].seeds, 77);
    EXPECT_EQ(results[0].source, "LocalEngine");
    EXPECT_DOUBLE_EQ(results[0].confidence, 0.8);

    EXPECT_EQ(results[1].title, "Cov Title Two");
    EXPECT_EQ(results[1].size, 512u * 1024u);

    EXPECT_TRUE(saw_request(server_.requests(), "GET /items"));
}

TEST_F(ResourceSearchCovNetTest, SearchBuildsUrlFromPathPattern) {
    TempFile config(build_engine_config(
        server_.base_url(), "", "json", {{"fmt", "json"}}, {},
        "/find/{query_letter}/{page}?kw={encoded_query}"));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "test";
    query.page = 2;
    auto results = manager.search_all(query);
    EXPECT_FALSE(results.empty());
    EXPECT_TRUE(saw_request(server_.requests(), "GET /find/t/2?kw=test&fmt=json"));
}

TEST_F(ResourceSearchCovNetTest, SearchPathPatternDigitKeywordAndUrlEncoding) {
    TempFile config(build_engine_config(
        server_.base_url(), "", "json", {}, {},
        "/find/{query_letter}/{first_char}/{page}?kw={encoded_query}", "DigitEngine"));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "42 club/2";
    query.page = 1;
    auto results = manager.search_all(query);
    EXPECT_FALSE(results.empty());
    EXPECT_TRUE(saw_request(server_.requests(), "GET /find/4/0/1?kw=42%20club%2F2"));
}

TEST_F(ResourceSearchCovNetTest, SearchEmptyResponseBodyYieldsNoResults) {
    TempFile config(build_engine_config(server_.base_url(), "/empty", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "nothing";
    auto results = manager.search_all(query);
    EXPECT_TRUE(results.empty());
}

TEST_F(ResourceSearchCovNetTest, SearchUnavailableServerYieldsNoResults) {
    TempFile config(build_engine_config("http://127.0.0.1:1", "/search", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "offline";
    auto results = manager.search_all(query);
    EXPECT_TRUE(results.empty());
}

// ============================================================================
// detail 提升函数补充覆盖（validate_url / parse_magnet_link / url_decode）
// ============================================================================

TEST(ResourceSearchCovDetailTest, ValidateUrlPrefixWhitelist) {
    EXPECT_FALSE(validate_url(""));
    EXPECT_TRUE(validate_url("magnet:?xt=urn:btih:" + std::string(40, 'a')));
    EXPECT_TRUE(validate_url("http://example.com/file.iso"));
    EXPECT_TRUE(validate_url("https://example.com/file.iso"));
    EXPECT_TRUE(validate_url("ftp://ftp.example.com/file.iso"));

    // 前缀白名单之外的 scheme 一律拒绝。
    EXPECT_FALSE(validate_url("ed2k://|file|name|1024|deadbeef|/"));
    EXPECT_FALSE(validate_url("thunder://QUFodHRw"));
    // "https" 开头但非 "https:" 前缀同样拒绝。
    EXPECT_FALSE(validate_url("httpsfake"));
    EXPECT_FALSE(validate_url("plain-string"));
}

TEST(ResourceSearchCovDetailTest, ParseMagnetLinkExtractsHashAndDecodesName) {
    const std::string magnet =
        "magnet:?xt=urn:btih:0123456789abcdef0123456789abcdef01234567"
        "&dn=Ubuntu%2024.04%20LTS&tr=http%3A%2F%2Ftracker.example%2Fannounce";

    const auto result = parse_magnet_link(magnet);
    EXPECT_EQ(result.url, magnet);
    EXPECT_EQ(result.type, "magnet");
    EXPECT_EQ(result.hash, "0123456789abcdef0123456789abcdef01234567");
    // dn 仅取到下一个 '&'，tr 参数不参与标题。
    EXPECT_EQ(result.title, "Ubuntu 24.04 LTS");
}

TEST(ResourceSearchCovDetailTest, ParseMagnetLinkToleratesMissingFields) {
    // 非 40 位十六进制的 btih 不匹配 hash 正则。
    const auto bare = parse_magnet_link("magnet:?xt=urn:btih:zzzz");
    EXPECT_EQ(bare.type, "magnet");
    EXPECT_EQ(bare.hash, "");
    EXPECT_EQ(bare.title, "");

    // 只有显示名也能出结果：'+' 转空格、%2F 解码为 '/'。
    const auto named = parse_magnet_link("magnet:?dn=big+file%2Fiso");
    EXPECT_EQ(named.hash, "");
    EXPECT_EQ(named.title, "big file/iso");
}

TEST(ResourceSearchCovDetailTest, UrlDecodeKeepsInvalidEscapesAndTrailingPercent) {
    // 非法十六进制转义原样保留。
    EXPECT_EQ(url_decode("100%zz"), "100%zz");
    // 结尾孤立 '%' 与 '%' 后仅剩一个字符均原样保留。
    EXPECT_EQ(url_decode("abc%"), "abc%");
    EXPECT_EQ(url_decode("a%2"), "a%2");
    // 普通字符串原样往返。
    EXPECT_EQ(url_decode("no escapes here"), "no escapes here");
}

// ============================================================================
// GenericSearchProvider 过滤排序 / 空 URL 快速返回
// ============================================================================

TEST_F(ResourceSearchCovNetTest, ProviderFilterSortsBySizeAndTrimsToLimit) {
    TempFile config(build_engine_config(server_.base_url(), "/search", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "sort";
    query.sort_by = "size";
    query.sort_desc = true;
    query.limit = 2;

    // provider 层排序（区别于 manager 的全局排序）：size 降序后截断到 limit。
    auto results = manager.search_providers(query, {"LocalEngine"});
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].title, "Big Pack");
    EXPECT_EQ(results[1].title, "Ubuntu 24.04 ISO");
}

TEST_F(ResourceSearchCovNetTest, ProviderFilterSortFallbackKeyUsesConfidence) {
    TempFile config(build_engine_config(server_.base_url(), "/search", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "sort";
    query.sort_by = "confidence";  // size/seeds 之外的兜底排序键

    // JSON 响应不带 confidence 字段（全 0），排序稳定且全部保留。
    auto results = manager.search_providers(query, {"LocalEngine"});
    ASSERT_EQ(results.size(), 3u);
}

TEST(ResourceSearchCovEmptyUrlTest, ProviderEmptySearchUrlReturnsBeforeRequest) {
    // base_url 与 search_path 均为空 => build_search_url 返回空串，
    // search() 在发起任何网络请求之前提前返回（离线安全）。
    TempFile config(build_engine_config("", "", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));
    ASSERT_EQ(manager.get_providers().size(), 1u);

    SearchQuery query;
    query.keyword = "anything";
    auto results = manager.search_providers(query, {"LocalEngine"});
    EXPECT_TRUE(results.empty());
}
