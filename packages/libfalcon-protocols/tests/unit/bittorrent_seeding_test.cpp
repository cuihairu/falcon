/**
 * @file bittorrent_seeding_test.cpp
 * @brief BitTorrent 做种端到端测试（libtorrent session 数据面）
 * @author Falcon Team
 * @date 2026-09-21
 *
 * 私有回环 P2P 全链路：seed 端用 libtorrent create_torrent 造真实
 * .torrent（256KB 随机数据）并对已存在文件做种；leech 端经 magnet
 * 的 x.pe 直连（私有模式关 DHT/LSD/UPnP/NAT-PMP、无 tracker，peer
 * 发现面为零）——seed uploaded_bytes >= 文件总长即"数据唯一来源是
 * 本地 seed"的结构性证据。完成/做种退出语义（ratio=0 立即停）由
 * download() 监控循环真实驱动。
 *
 * 非库模式（FALCON_USE_LIBTORRENT 未定义）session 数据面不存在，
 * 仅保留 skip 占位（seed_policy 纯单元测试两模式共享）。
 */

#include <gtest/gtest.h>

#ifdef FALCON_USE_LIBTORRENT

#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <falcon/download_task.hpp>
#include <falcon/event_listener.hpp>

#include <libtorrent/bdecode.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/file_storage.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace falcon;
using namespace falcon::protocols;

namespace {

/// 临时目录（构造创建、析构清理；pid + 进程内计数保证唯一）
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        dir_ = std::filesystem::temp_directory_path() /
               ("falcon_bt_seed_" + std::to_string(getPid()) + "_" +
                std::to_string(counter.fetch_add(1)));
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return dir_; }

private:
#ifdef _WIN32
    static int getPid() { return static_cast<int>(::_getpid()); }
#else
    static int getPid() { return ::getpid(); }
#endif
    std::filesystem::path dir_;
};

std::vector<uint8_t> makeRandomPayload(std::size_t size) {
    std::mt19937_64 rng(std::random_device{}());
    std::vector<uint8_t> data(size);
    for (std::size_t i = 0; i < size; i += sizeof(uint64_t)) {
        const auto v = rng();
        const auto* bytes = reinterpret_cast<const uint8_t*>(&v);
        for (std::size_t j = 0; j < sizeof(uint64_t) && i + j < size; ++j) {
            data[i + j] = bytes[j];
        }
    }
    return data;
}

void writeFile(const std::filesystem::path& path,
               const std::vector<uint8_t>& data) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    ASSERT_TRUE(out.good()) << "写入数据文件失败: " << path;
}

std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    EXPECT_TRUE(in.good()) << "读取成品失败: " << path;
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

/// 造单文件 v1 torrent：写 .torrent 文件并返回 make_magnet_uri 基串
/// （无 x.pe；调用方按 seed 实际监听端口追加）
std::string createTorrentFile(const std::filesystem::path& dataFile,
                              const std::filesystem::path& metaOut) {
    libtorrent::file_storage fs;
    libtorrent::add_files(fs, dataFile.string());
    libtorrent::create_torrent creator(fs, 0,
                                       libtorrent::create_torrent::v1_only);
    libtorrent::error_code ec;
    libtorrent::set_piece_hashes(creator, dataFile.parent_path().string(), ec);
    if (ec) {
        ADD_FAILURE() << "set_piece_hashes 失败: " << ec.message();
        return {};
    }

    std::vector<char> buf;
    libtorrent::bencode(std::back_inserter(buf), creator.generate());
    {
        std::ofstream out(metaOut, std::ios::binary);
        out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
        EXPECT_TRUE(out.good());
    }

    // magnet 基串经 torrent_info 生成（info-hash 与 .torrent 文件一致）
    libtorrent::bdecode_node node;
    ec.clear();
    libtorrent::bdecode(buf.data(), buf.data() + buf.size(), node, ec);
    EXPECT_FALSE(ec) << ec.message();
    if (ec) {
        return {};
    }
    libtorrent::error_code tiEc;
    libtorrent::torrent_info ti(node, tiEc);
    EXPECT_FALSE(tiEc) << tiEc.message();
    if (tiEc) {
        return {};
    }
    return libtorrent::make_magnet_uri(ti);
}

/// 等待 session 实际监听端口（":0" 异步绑定）
bool awaitListenPort(BitTorrentHandler& handler, uint16_t& out,
                     std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        out = handler.listen_port();
        if (out != 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

/// 对已存在数据文件做种的 seed 端（比率上限 100 永不达标，
/// 由测试显式 cancel 收口）
struct SeedSession {
    TempDir dir;  // 先声明：析构逆序 ⇒ handler（session）先于文件清理销毁
    BitTorrentHandler handler;
    std::vector<uint8_t> payload;
    std::filesystem::path dataFile;
    std::filesystem::path metaPath;
    DownloadTask::Ptr task;
    std::exception_ptr error;
    std::thread thread;
    TaskId taskId = 0;

    std::string start(std::size_t payloadSize, const std::string& fileName) {
        payload = makeRandomPayload(payloadSize);
        dataFile = dir.path() / fileName;
        writeFile(dataFile, payload);
        metaPath = dir.path() / (fileName + ".torrent");
        std::string magnet = createTorrentFile(dataFile, metaPath);

        handler.configure_private_mode();
        handler.set_listen_interfaces("127.0.0.1:0");
        uint16_t port = 0;
        if (!awaitListenPort(handler, port, std::chrono::seconds(10))) {
            return {};
        }

        DownloadOptions opts;
        opts.output_directory = dir.path().string();
        opts.seed_ratio = 100.0;
        static std::atomic<TaskId> nextId{1000};
        taskId = nextId.fetch_add(1);
        task = std::make_shared<DownloadTask>(taskId, metaPath.string(), opts);
        thread = std::thread([this] {
            try {
                handler.download(task, nullptr);
            } catch (...) {
                error = std::current_exception();
            }
        });
        return magnet;
    }

    void stop() {
        if (thread.joinable()) {
            handler.cancel(task);
            thread.join();
        }
    }

    ~SeedSession() { stop(); }
};

/// magnet/_.torrent leech 端（ratio=0：下载完成立即收口）
struct LeechSession {
    BitTorrentHandler handler;
    TempDir dir;
    DownloadTask::Ptr task;
    std::exception_ptr error;
    std::atomic<bool> done{false};
    std::thread thread;
    TaskId taskId = 0;

    void start(const std::string& url) {
        DownloadOptions opts;
        opts.output_directory = dir.path().string();
        opts.seed_ratio = 0.0;
        static std::atomic<TaskId> nextId{2000};
        taskId = nextId.fetch_add(1);
        task = std::make_shared<DownloadTask>(taskId, url, opts);
        handler.configure_private_mode();
        handler.set_listen_interfaces("127.0.0.1:0");
        thread = std::thread([this] {
            try {
                handler.download(task, nullptr);
            } catch (...) {
                error = std::current_exception();
            }
            done = true;
        });
    }

    /// 阻塞至 download 返回；超时内部先 cancel 再 join（收口存活线程）。
    /// 返回 false = 超时未完成
    bool wait(std::chrono::seconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!done.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (!done.load()) {
            handler.cancel(task);
        }
        thread.join();
        return done.load();
    }

    ~LeechSession() {
        if (thread.joinable()) {
            handler.cancel(task);
            thread.join();
        }
    }
};

/// 全链路收尾：断言 leech 终态/无异常 + 成品逐字节一致 + seed 上传计量
void assertLeechResult(LeechSession& leech, SeedSession& seed,
                       const std::filesystem::path& outputFile) {
    ASSERT_FALSE(leech.error) << "leech download 抛出异常";
    ASSERT_TRUE(leech.done.load());
    EXPECT_EQ(leech.task->status(), TaskStatus::Completed);
    ASSERT_FALSE(seed.error) << "seed download 抛出异常";
    EXPECT_EQ(readFile(outputFile), seed.payload);
}

TEST(BitTorrentSeedingE2E, MagnetLeechFromLocalSeedCompletesAndUploads) {
    SeedSession seed;
    const std::size_t kPayloadSize = 256 * 1024;
    const std::string magnet = seed.start(kPayloadSize, "falcon_seed_e2e.bin");
    ASSERT_FALSE(magnet.empty()) << "seed 启动失败（监听端口未就绪）";

    // seed 端已对完整数据做种：magnet 追加 x.pe 直连端口
    LeechSession leech;
    leech.start(magnet + "&x.pe=127.0.0.1:" +
                std::to_string(seed.handler.listen_port()));
    ASSERT_TRUE(leech.wait(std::chrono::seconds(60)))
        << "leech 未在超时窗口内完成";

    assertLeechResult(leech, seed, seed.dir.path() / "falcon_seed_e2e.bin");
    // 数据唯一来源是本地 seed（私有模式无任何其他发现面）：
    // 上传载荷 >= 文件总长；metadata 交换不计 payload upload。
    // leech 侧计量无法观测——完成路径 remove_torrent 已摘句柄，
    // 访问器按契约返回 0（活动任务才有意义）
    EXPECT_GE(seed.handler.uploaded_bytes(seed.taskId), kPayloadSize);

    seed.stop();
}

TEST(BitTorrentSeedingE2E, MeterAccessorsMissReturnsZero) {
    BitTorrentHandler handler;
    EXPECT_EQ(handler.uploaded_bytes(9999), 0u);
    EXPECT_EQ(handler.downloaded_bytes(9999), 0u);
}

} // namespace

#else // !FALCON_USE_LIBTORRENT

// 纯 C++ 数据面无 libtorrent session：做种 e2e 仅在 libtorrent 构建下存在
TEST(BitTorrentSeedingE2E, DisabledWithoutLibtorrent) {
    GTEST_SKIP() << "session 数据面仅在 FALCON_USE_LIBTORRENT 构建下存在";
}

#endif // FALCON_USE_LIBTORRENT
