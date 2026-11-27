/**
 * @file resource_search_coverage_test_part2.cpp
 * @brief 资源搜索覆盖率补充测试（离线逻辑 + 本地回环 HTTP 服务器）
 * @author Falcon Team
 * @date 2026-09-05
 */

#include <csignal>
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

// 条目不带 source 字段的形态——来源应缺省为引擎名（B17 走查曝光）
const char* kJsonNoSourceBody = R"([
    {"title":"No Source One","url":"https://ns.example/one.bin","size":1288490188,"seeds":42,"type":"video"},
    {"title":"No Source Two","url":"https://ns.example/two.bin","size":838860800,"seeds":17,"type":"music"}
])";

const char* kBadJsonBody = "this is definitely { not valid json";

const char* kTypeErrBody = R"({"results":[{"title":"BadType","url":"magnet:?xt=urn:btih:eeee","size":"not-a-number"}]})";

// 字段类型混乱形态：size/seeds 以字符串给出（"1.5 GB" 人类可读尺寸
// + 纯数字字符串 seeds）——宽容提取应解析而非丢弃（B17 走查曝光）
const char* kStringSizeBody = R"({"results":[
    {"title":"String Size Item","url":"https://ss.example/one.bin","size":"1.5 GB","seeds":"42","type":"video"}
]})";


// 数值类型混乱形态：float size、负整数、超界无符号、非数字字符串
// seeds、bool 字段——json_size/json_int/json_str 各宽容分支
const char* kMixedNumBody = R"({"results":[
    {"title":"Float Size Item","url":"https://mix.example/a.bin","size":1.5,"seeds":7,"leeches":3.9},
    {"title":"Neg Size Item","url":"https://mix.example/b.bin","size":-100,"seeds":-5,"leeches":-2},
    {"title":"Huge Size Item","url":"https://mix.example/c.bin","size":18446744073709551615,"seeds":2147483648},
    {"title":"Bad Seeds Item","url":"https://mix.example/d.bin","size":2048,"seeds":"not-a-number","leeches":"zz"},
    {"title":"Bool Fields Item","url":"https://mix.example/e.bin","size":true,"seeds":false,"leeches":true,"title":5,"url":6,"hash":null}
]})";

const char* kMixedNumBody2 = R"({"results":[
    {"title":"Huge Float Size","url":"https://mix2.example/f.bin","size":1.8e19,"seeds":2},
    {"title":"Tiny Float Size","url":"https://mix2.example/g.bin","size":0.5,"seeds":1}
]})";


// 数组内混入标量条目（非对象）：extract_json_item 对标量取键抛
// nlohmann type_error → 走 std::exception 分支，解析不崩溃
const char* kScalarItemBody = R"([
    {"title":"Good Item","url":"https://scalar.example/good.bin","size":1024,"seeds":3},
    "scalar-junk",
    42,
    {"title":"Second Good","url":"https://scalar.example/two.bin","size":2048,"seeds":4}
])";

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


TEST_F(ResourceSearchCovNetTest, SearchToleratesMixedNumericFieldTypes) {
    // 站点字段类型不可控：float/负整数/超界无符号/垃圾字符串/bool 混入，
    // 宽容提取逐字段降级而不丢结果（json_str/json_int/json_size 各分支）
    TempFile config(build_engine_config(server_.base_url(), "/mixednum", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "mix";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 5u);
    EXPECT_EQ(results[0].size, 1u);           // 1.5 float → 截断 1
    EXPECT_EQ(results[0].seeds, 7);
    EXPECT_EQ(results[0].peers, 3);           // 3.9 float → 截断 3
    EXPECT_EQ(results[1].size, 0u);           // -100 → 0
    EXPECT_EQ(results[1].seeds, -5);
    EXPECT_EQ(results[1].peers, -2);
    EXPECT_EQ(results[2].size, 18446744073709551615ULL);
    EXPECT_EQ(results[2].seeds, 0);           // 2147483648 超 int 界 → 0
    EXPECT_EQ(results[3].size, 2048u);
    EXPECT_EQ(results[3].seeds, 0);           // stoi 抛 → 0
    EXPECT_EQ(results[3].peers, 0);
    // 数字 title/url 经 json_str 数字分支转字符串；bool size/seeds/leeches
    // 非数字类型 → json_size/json_int 落 0；hash=null → json_str 空
    EXPECT_EQ(results[4].title, "5");
    EXPECT_EQ(results[4].url, "6");
    EXPECT_EQ(results[4].hash, "");
    EXPECT_EQ(results[4].size, 0u);
    EXPECT_EQ(results[4].seeds, 0);
    EXPECT_EQ(results[4].peers, 0);
}


TEST_F(ResourceSearchCovNetTest, SearchToleratesScalarArrayItems) {
    // 数组含标量垃圾条目：整段解析仍成功，合法对象条目保留
    TempFile config(build_engine_config(server_.base_url(), "/scalaritem", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "scalar";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].title, "Good Item");
    EXPECT_EQ(results[0].size, 1024u);
    EXPECT_EQ(results[1].title, "Second Good");
    EXPECT_EQ(results[1].size, 2048u);
}

TEST_F(ResourceSearchCovNetTest, SearchClampsOutOfRangeFloatSizes) {
    // 1.8e19 超 uint64 上限 → 0；0.5 合法正浮点 → 0（截断）
    TempFile config(build_engine_config(server_.base_url(), "/mixednum2", "json", {}));
    ResourceSearchManager manager;
    ASSERT_TRUE(manager.load_config(config.path()));

    SearchQuery query;
    query.keyword = "mix2";
    auto results = manager.search_all(query);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(results[0].size, 0u);
    EXPECT_EQ(results[1].size, 0u);
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
// detail 提升函数补充覆盖（parse_magnet_link / url_decode）
// ============================================================================

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
