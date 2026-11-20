/**
 * @file url_detector_test.cpp
 * @brief UrlDetector 网盘识别迁移回归（规则唯一事实源在 drives 层
 * CloudLinkDetector；本测试钉住迁移后的 UI 侧行为）
 *
 * 迁移前形态锚：五平台（百度/阿里/夸克/天翼/蓝奏）的识别与
 * 「平台名 (分享码)」文件名格式必须与迁移前逐字一致；
 * 扩面锚：drives 层目录中的其余八平台自迁移起可识别。
 */

#include <gtest/gtest.h>

#include "../src/utils/url_detector.hpp"

namespace {

using falcon::desktop::UrlDetector;
using falcon::desktop::UrlInfo;
using falcon::desktop::UrlProtocol;

TEST(UrlDetectorTest, DetectsLegacyCloudPlatforms) {
    const struct {
        const char* url;
        UrlProtocol protocol;
        const char* file_name;
    } cases[] = {
        {"https://pan.baidu.com/s/1abc_XY", UrlProtocol::BAIDU, "Baidu Pan (1abc_XY)"},
        {"https://www.alipan.com/s/zz9", UrlProtocol::ALIYUN, "Aliyun Drive (zz9)"},
        {"https://www.aliyundrive.com/s/old1", UrlProtocol::ALIYUN, "Aliyun Drive (old1)"},
        {"https://pan.quark.cn/s/q1", UrlProtocol::QUARK, "Quark Drive (q1)"},
        {"https://cloud.189.cn/t/ab12cd", UrlProtocol::TIANYI, "Tianyi Cloud (ab12cd)"},
        {"https://www.lanzoux.com/iabc", UrlProtocol::LANZHOU, "Lanzou Cloud (iabc)"},
    };

    for (const auto& entry : cases) {
        const UrlInfo info = UrlDetector::parse_url(entry.url);
        EXPECT_TRUE(info.is_valid) << entry.url;
        EXPECT_EQ(entry.protocol, info.protocol) << entry.url;
        EXPECT_EQ(QString(entry.file_name), info.file_name) << entry.url;
    }
}

TEST(UrlDetectorTest, DetectsExtendedCloudPlatforms) {
    const struct {
        const char* url;
        UrlProtocol protocol;
    } cases[] = {
        {"https://share.weiyun.com/wy77", UrlProtocol::WEIYUN},
        {"https://115.com/s/q1", UrlProtocol::CLOUD115},
        {"https://share.pikpak.com/s/p3", UrlProtocol::PIKPAK},
        {"https://mega.nz/#Ab_12", UrlProtocol::MEGA},
        {"https://drive.google.com/file/d/1B2_c/view", UrlProtocol::GDRIVE},
        {"https://onedrive.live.com/x?id=x", UrlProtocol::ONEDRIVE},
        {"https://www.dropbox.com/s/abc123/file", UrlProtocol::DROPBOX},
        {"https://disk.yandex.ru/d/yx8/file", UrlProtocol::YANDEX},
    };

    for (const auto& entry : cases) {
        const UrlInfo info = UrlDetector::parse_url(entry.url);
        EXPECT_TRUE(info.is_valid) << entry.url;
        EXPECT_EQ(entry.protocol, info.protocol) << entry.url;
        EXPECT_FALSE(info.file_name.isEmpty()) << entry.url;
    }
}

TEST(UrlDetectorTest, CloudDetectionTakesPrecedenceOverPlainHttps) {
    // 网盘链接先于标准协议判定——不得落到 HTTPS
    EXPECT_EQ(UrlProtocol::BAIDU,
              UrlDetector::parse_url("https://pan.baidu.com/s/1abc").protocol);
}

TEST(UrlDetectorTest, PlainHttpsUnaffected) {
    const UrlInfo info = UrlDetector::parse_url("https://example.com/file.zip");
    EXPECT_TRUE(info.is_valid);
    EXPECT_EQ(UrlProtocol::HTTPS, info.protocol);
    EXPECT_EQ(QString("file.zip"), info.file_name);
}

TEST(UrlDetectorTest, SchemelessCloudDomainDetectedViaDrivesNormalize) {
    // drives 层 normalize_url 会补 https://——无 scheme 的裸域名
    // 网盘链接自迁移起同样识别（旧行为因 pattern 锚定 ^https?:// 不识别）
    EXPECT_EQ(UrlProtocol::BAIDU,
              UrlDetector::parse_url("pan.baidu.com/s/1abc").protocol);
}

TEST(UrlDetectorTest, GetProtocolNameFromDrivesCatalog) {
    EXPECT_EQ(QString("Baidu Pan"), UrlDetector::get_protocol_name(UrlProtocol::BAIDU));
    EXPECT_EQ(QString("Tianyi Cloud"), UrlDetector::get_protocol_name(UrlProtocol::TIANYI));
    EXPECT_EQ(QString("Lanzou Cloud"), UrlDetector::get_protocol_name(UrlProtocol::LANZHOU));
    EXPECT_EQ(QString("Google Drive"), UrlDetector::get_protocol_name(UrlProtocol::GDRIVE));
    EXPECT_EQ(QString("Magnet"), UrlDetector::get_protocol_name(UrlProtocol::MAGNET));
    EXPECT_EQ(QString("Unknown"), UrlDetector::get_protocol_name(UrlProtocol::UNKNOWN));
}

TEST(UrlDetectorTest, ContainsUrlCloudAndGarbage) {
    EXPECT_TRUE(UrlDetector::contains_url("https://pan.baidu.com/s/1abc"));
    EXPECT_TRUE(UrlDetector::contains_url("https://mega.nz/#Ab_12"));
    EXPECT_TRUE(UrlDetector::contains_url("https://example.com/file.zip"));
    EXPECT_FALSE(UrlDetector::contains_url("hello world"));
    EXPECT_FALSE(UrlDetector::contains_url(""));
}

// 剪贴板弹窗门（looks_like_download_url）：下载语义协议与网盘一律算；
// HTTP/HTTPS 必须路径末段带文件扩展名——纯网页 URL 不弹。
TEST(UrlDetectorTest, LooksLikeDownloadUrlClipboardGate) {
    // 下载语义协议：无条件弹
    EXPECT_TRUE(UrlDetector::looks_like_download_url(
        "magnet:?xt=urn:btih:abcdef&dn=ubuntu.iso"));
    EXPECT_TRUE(UrlDetector::looks_like_download_url(
        "ed2k://|file|demo.bin|1024|AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA|/"));
    EXPECT_TRUE(UrlDetector::looks_like_download_url("ftp://host/pub/tools/7zip.exe"));
    EXPECT_TRUE(UrlDetector::looks_like_download_url("thunder://QUFodHRwOi8v"));

    // 网盘分享链接：一律弹
    EXPECT_TRUE(UrlDetector::looks_like_download_url("https://pan.baidu.com/s/1abc_XY"));
    EXPECT_TRUE(UrlDetector::looks_like_download_url("https://www.alipan.com/s/zz9"));

    // HTTP 文件 URL（路径末段带扩展名）：弹
    EXPECT_TRUE(UrlDetector::looks_like_download_url("https://example.com/files/setup.zip"));
    EXPECT_TRUE(UrlDetector::looks_like_download_url("https://example.com/media/movie.mp4"));
    EXPECT_TRUE(UrlDetector::looks_like_download_url("https://example.com/d/iso/ubuntu-24.04-live-server-amd64.iso"));

    // 纯网页 URL：不弹（用户反馈的误弹形态）
    EXPECT_FALSE(UrlDetector::looks_like_download_url("https://www.qidian.com/"));
    EXPECT_FALSE(UrlDetector::looks_like_download_url("https://github.com/cuihairu/falcon"));
    EXPECT_FALSE(UrlDetector::looks_like_download_url("https://example.com/article?id=123"));
    EXPECT_FALSE(UrlDetector::looks_like_download_url("https://example.com/docs/page.html"));
    EXPECT_FALSE(UrlDetector::looks_like_download_url("https://example.com/files/"));
    EXPECT_FALSE(UrlDetector::looks_like_download_url("https://example.com/api/v2/items.json"));

    // 垃圾文本与空串：不弹
    EXPECT_FALSE(UrlDetector::looks_like_download_url("hello world"));
    EXPECT_FALSE(UrlDetector::looks_like_download_url(""));
}

TEST(UrlDetectorTest, PrivateProtocolsUnaffected) {
    const UrlInfo magnet = UrlDetector::parse_url(
        "magnet:?xt=urn:btih:abcdef&dn=ubuntu.iso&xl=123");
    EXPECT_EQ(UrlProtocol::MAGNET, magnet.protocol);
    EXPECT_EQ(QString("ubuntu.iso"), magnet.file_name);
    EXPECT_EQ(QString("123"), magnet.file_size);

    // thunder://AA<url>ZZ 的 base64 包装解码
    // （QUFodHRw…Wlo= 即 base64("AAhttp://example.com/a.zipZZ")）
    const UrlInfo thunder = UrlDetector::parse_url("thunder://QUFodHRwOi8vZXhhbXBsZS5jb20vYS56aXBaWg==");
    EXPECT_EQ(UrlProtocol::THUNDER, thunder.protocol);
    EXPECT_EQ(QString("http://example.com/a.zip"), thunder.decoded_url);
    EXPECT_EQ(QString("a.zip"), thunder.file_name);

    EXPECT_EQ(UrlProtocol::ED2K,
              UrlDetector::parse_url("ed2k://|file|a.iso|1024|0123456789abcdef|/").protocol);
}

} // namespace
