/**
 * @file bittorrent_parse_test.cpp
 * @brief BitTorrent 插件 magnet/.torrent 解析测试（纯 C++ 模式）
 * @author Falcon Team
 * @date 2026-09-13
 *
 * 覆盖 can_handle 的 info-hash 校验（hex/base32/边界）、get_file_info 的
 * .torrent 解析（单文件/多文件/file:// 前缀/缺文件/非法内容）。
 */

#include <falcon/plugins/bittorrent/bittorrent_plugin.hpp>
#include <falcon/exceptions.hpp>

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace falcon;
using namespace falcon::protocols;

namespace {

/// 临时目录（构造创建、析构清理）
class TempDir {
public:
    TempDir() {
        dir_ = std::filesystem::temp_directory_path() /
               ("falcon_bt_parse_" + std::to_string(::getpid()));
        std::filesystem::create_directories(dir_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir_, ec);
    }

    const std::filesystem::path& path() const { return dir_; }

private:
#ifdef _WIN32
    static int getpid() { return static_cast<int>(::_getpid()); }
#else
    static int getpid() { return ::getpid(); }
#endif
    std::filesystem::path dir_;
};

/// 写出 torrent 数据文件并返回路径字符串
std::string writeTorrent(const TempDir& dir, const std::string& name,
                         const std::string& bencodeData) {
    const auto file = dir.path() / name;
    std::ofstream out(file, std::ios::binary);
    out << bencodeData;
    return file.string();
}

const std::string kSingleFileTorrent =
    "d8:announce40:http://tracker.example.com:6969/announce"
    "4:infod6:lengthi1048576e4:name13:test_file.zip"
    "12:piece lengthi262144e"
    "6:pieces22:abcdefghijklmnopqrstuv"
    "ee";

const std::string kMultiFileTorrent =
    "d8:announce40:http://tracker.example.com:6969/announce"
    "4:infod5:files"
    "ld6:lengthi100e4:pathl2:aa1:1ee"    // aa/1
    "d6:lengthi264e4:pathl2:bb1:2ee"     // bb/2
    "e"                                   // files 列表闭合
    "4:name4:pack12:piece lengthi262144e"
    "6:pieces22:abcdefghijklmnopqrstuv"
    "ee";

} // namespace

//==============================================================================
// can_handle：magnet info-hash 校验
//==============================================================================

TEST(BitTorrentCanHandleTest, ValidMagnetHexHash) {
    BitTorrentHandler handler;
    EXPECT_TRUE(handler.can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678"));
    // 大写 hex
    EXPECT_TRUE(handler.can_handle(
        "magnet:?xt=urn:btih:ABCDEF0123456789ABCDEF0123456789ABCDEF01"));
    // 后续参数不影响
    EXPECT_TRUE(handler.can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678&tr=x"));
    // 片段终止符
    EXPECT_TRUE(handler.can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678#frag"));
}

TEST(BitTorrentCanHandleTest, ValidMagnetBase32Hash) {
    BitTorrentHandler handler;
    // 32 个 base32 字符（A-Z、2-7）
    EXPECT_TRUE(handler.can_handle(
        "magnet:?xt=urn:btih:ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"));
    EXPECT_TRUE(handler.can_handle(
        "magnet:?xt=urn:btih:abcdefghijklmnopqrstuvwxyz234567"));
}

TEST(BitTorrentCanHandleTest, InvalidMagnetHashes) {
    BitTorrentHandler handler;
    // 长度 39
    EXPECT_FALSE(handler.can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef123456"));
    // 长度 41
    EXPECT_FALSE(handler.can_handle(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef1234567f8"));
    // 40 个字符但含非 hex
    EXPECT_FALSE(handler.can_handle(
        "magnet:?xt=urn:btih:zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"));
    // 32 个字符但含 base32 之外的数字（0/1/8/9）
    EXPECT_FALSE(handler.can_handle(
        "magnet:?xt=urn:btih:11111111111111111111111111111111"));
    // 缺 xt 参数
    EXPECT_FALSE(handler.can_handle("magnet:?dn=test.zip"));
}

TEST(BitTorrentCanHandleTest, OtherUrlForms) {
    BitTorrentHandler handler;
    EXPECT_TRUE(handler.can_handle("http://x/file.torrent"));
    EXPECT_TRUE(handler.can_handle("bittorrent://infohash/"));
    EXPECT_FALSE(handler.can_handle("http://x/file.zip"));
    EXPECT_FALSE(handler.can_handle(""));
}

//==============================================================================
// get_file_info：.torrent 解析
//==============================================================================

TEST(BitTorrentParseTest, SingleFileTorrentInfo) {
    TempDir dir;
    BitTorrentHandler handler;
    const std::string path = writeTorrent(dir, "single.torrent", kSingleFileTorrent);

    auto info = handler.get_file_info(path, DownloadOptions{});
    EXPECT_EQ(info.filename, "test_file.zip");
    EXPECT_EQ(info.total_size, size_t{1048576});
    EXPECT_TRUE(info.supports_resume);
    EXPECT_EQ(info.url, path);
}

TEST(BitTorrentParseTest, SingleFileTorrentViaFileScheme) {
    TempDir dir;
    BitTorrentHandler handler;
    const std::string path = writeTorrent(dir, "scheme.torrent", kSingleFileTorrent);

    auto info = handler.get_file_info("file://" + path, DownloadOptions{});
    EXPECT_EQ(info.filename, "test_file.zip");
}

TEST(BitTorrentParseTest, MultiFileTorrentSumsLengths) {
    TempDir dir;
    BitTorrentHandler handler;
    const std::string path = writeTorrent(dir, "multi.torrent", kMultiFileTorrent);

    auto info = handler.get_file_info(path, DownloadOptions{});
    EXPECT_EQ(info.filename, "pack");
    EXPECT_EQ(info.total_size, size_t{364});  // 100 + 264
}

TEST(BitTorrentParseTest, MissingTorrentFileThrows) {
    TempDir dir;
    BitTorrentHandler handler;
    const std::string missing = (dir.path() / "nope.torrent").string();
    EXPECT_THROW(handler.get_file_info(missing, DownloadOptions{}), FileIOException);
}

TEST(BitTorrentParseTest, MalformedTorrentThrows) {
    TempDir dir;
    BitTorrentHandler handler;
    const std::string path = writeTorrent(dir, "bad.torrent", "not-bencode");
    EXPECT_THROW(handler.get_file_info(path, DownloadOptions{}), std::exception);
}

TEST(BitTorrentParseTest, TorrentWithoutInfoDictKeepsEmptyInfo) {
    TempDir dir;
    BitTorrentHandler handler;
    // 合法 bencode 但缺 info 字典：validateTorrent 失败，不抛出、字段留空
    const std::string path =
        writeTorrent(dir, "empty.torrent", "d8:announce4:httpe");
    auto info = handler.get_file_info(path, DownloadOptions{});
    EXPECT_TRUE(info.filename.empty());
    EXPECT_EQ(info.total_size, size_t{0});
}

TEST(BitTorrentParseTest, MagnetInfoHasNoSize) {
    BitTorrentHandler handler;
    auto info = handler.get_file_info(
        "magnet:?xt=urn:btih:1234567890abcdef1234567890abcdef12345678&dn=x",
        DownloadOptions{});
    EXPECT_TRUE(info.filename.empty());
    EXPECT_EQ(info.total_size, size_t{0});
    EXPECT_TRUE(info.supports_resume);
}

// 空文件：读成功但 data 为空 → parseBencode 入口的 pos >= length
// 判定直接 throw（截断输入严格化的最短形态）
TEST(BitTorrentParseTest, EmptyTorrentFileThrows) {
    TempDir dir;
    BitTorrentHandler handler;
    const std::string path =
        writeTorrent(dir, "empty.torrent", "");
    EXPECT_THROW(handler.get_file_info(path, DownloadOptions{}), std::exception);
}
