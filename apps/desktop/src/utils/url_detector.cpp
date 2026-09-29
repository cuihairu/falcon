/**
 * @file url_detector.cpp
 * @brief URL detector implementation
 * @author Falcon Team
 * @date 2025-12-28
 */

#include "url_detector.hpp"

#include <QByteArray>
#include <QUrl>
#include <QUrlQuery>

namespace falcon::desktop {

//==============================================================================
// URL Pattern Definitions（标准/私有协议——网盘规则在 drives 层，见 detect_protocol）
//==============================================================================

// HTTP/HTTPS pattern
const QRegularExpression HTTP_PATTERN(
    R"(^https?://[^\s/$.?#].[^\s]*$)",
    QRegularExpression::CaseInsensitiveOption
);

// FTP pattern
const QRegularExpression FTP_PATTERN(
    R"(^ftps?://[^\s/$.?#].[^\s]*$)",
    QRegularExpression::CaseInsensitiveOption
);

// Magnet pattern
const QRegularExpression MAGNET_PATTERN(
    R"(^magnet:\?xt=[^\s]+$)",
    QRegularExpression::CaseInsensitiveOption
);

// Thunder pattern
const QRegularExpression THUNDER_PATTERN(
    R"(^thunder://[A-Za-z0-9+/=]+$)",
    QRegularExpression::CaseInsensitiveOption
);

// QQDL pattern
const QRegularExpression QQLINK_PATTERN(
    R"(^qqlink://[A-Za-z0-9+/=]+$)",
    QRegularExpression::CaseInsensitiveOption
);

// Flashget pattern
const QRegularExpression FLASHGET_PATTERN(
    R"(^flashget://[A-Za-z0-9+/=]+\[FLASHGET\]$)",
    QRegularExpression::CaseInsensitiveOption
);

// ED2K pattern
const QRegularExpression ED2K_PATTERN(
    R"(^ed2k://\|file\|([^|]+)\|([0-9]+)\|([A-Fa-f0-9]+)\|/?)",
    QRegularExpression::CaseInsensitiveOption
);

//==============================================================================
// Public Methods
//==============================================================================

bool UrlDetector::contains_url(const QString& text)
{
    if (text.isEmpty()) {
        return false;
    }

    // Try to match each pattern
    if (HTTP_PATTERN.match(text).hasMatch()) return true;
    if (FTP_PATTERN.match(text).hasMatch()) return true;
    if (MAGNET_PATTERN.match(text).hasMatch()) return true;
    if (THUNDER_PATTERN.match(text).hasMatch()) return true;
    if (QQLINK_PATTERN.match(text).hasMatch()) return true;
    if (FLASHGET_PATTERN.match(text).hasMatch()) return true;
    if (ED2K_PATTERN.match(text).hasMatch()) return true;

    // 网盘分享链接：drives 层平台元数据判定
    if (CloudLinkDetector::detect_platform(text.toStdString()) != CloudPlatform::Unknown) {
        return true;
    }

    return false;
}

UrlInfo UrlDetector::parse_url(const QString& text)
{
    UrlInfo info;
    info.original_url = text.trimmed();

    // Detect protocol
    info.protocol = detect_protocol(info.original_url);

    if (info.protocol == UrlProtocol::UNKNOWN) {
        info.is_valid = false;
        return info;
    }

    info.is_valid = true;

    // Parse based on protocol
    switch (info.protocol) {
        case UrlProtocol::HTTP:
        case UrlProtocol::HTTPS:
        case UrlProtocol::FTP:
            info.decoded_url = info.original_url;
            info.file_name = extract_file_name(info.original_url);
            break;

        case UrlProtocol::MAGNET:
            info = parse_magnet_url(info.original_url);
            break;

        case UrlProtocol::THUNDER:
            info.decoded_url = parse_thunder_url(info.original_url);
            info.file_name = extract_file_name(info.decoded_url);
            break;

        case UrlProtocol::QQLINK:
            info.decoded_url = parse_qqlink_url(info.original_url);
            info.file_name = extract_file_name(info.decoded_url);
            break;

        case UrlProtocol::FLASHGET:
            info.decoded_url = parse_flashget_url(info.original_url);
            info.file_name = extract_file_name(info.decoded_url);
            break;

        case UrlProtocol::ED2K:
            info = parse_ed2k_url(info.original_url);
            break;

        default: {
            // 网盘协议族：drives 层平台识别 + 元数据解析
            const CloudPlatform platform = to_cloud_platform(info.protocol);
            if (platform != CloudPlatform::Unknown) {
                info = parse_cloud_url(info.original_url, platform);
            } else {
                info.is_valid = false;
            }
            break;
        }
    }

    return info;
}

QString UrlDetector::get_protocol_name(UrlProtocol protocol)
{
    switch (protocol) {
        case UrlProtocol::HTTP:   return "HTTP";
        case UrlProtocol::HTTPS:  return "HTTPS";
        case UrlProtocol::FTP:    return "FTP";
        case UrlProtocol::MAGNET:  return "Magnet";
        case UrlProtocol::THUNDER: return "Thunder";
        case UrlProtocol::QQLINK:  return "QQDL";
        case UrlProtocol::FLASHGET: return "Flashget";
        case UrlProtocol::ED2K:    return "ED2K";
        default:                   break;
    }

    // 网盘平台名：drives 层元数据（不在 UI 侧二次维护）
    const CloudPlatform platform = to_cloud_platform(protocol);
    if (platform != CloudPlatform::Unknown) {
        return QString::fromStdString(CloudLinkDetector::platform_display_name(platform));
    }
    return "Unknown";
}

QString UrlDetector::extract_file_name(const QString& url)
{
    QUrl qurl(url);
    QString path = qurl.path();

    // Extract filename from path
    qsizetype last_slash = path.lastIndexOf('/');
    if (last_slash >= 0 && last_slash < path.length() - 1) {
        return path.mid(last_slash + 1);
    }

    return QString();
}

bool UrlDetector::is_supported_protocol(const QString& url)
{
    UrlProtocol protocol = detect_protocol(url);
    return protocol != UrlProtocol::UNKNOWN;
}

//==============================================================================
// Private Methods
//==============================================================================

UrlProtocol UrlDetector::detect_protocol(const QString& url)
{
    QString lower_url = url.toLower();

    // Check for special protocols first
    if (lower_url.startsWith("magnet:")) {
        return UrlProtocol::MAGNET;
    } else if (lower_url.startsWith("thunder://")) {
        return UrlProtocol::THUNDER;
    } else if (lower_url.startsWith("qqlink://")) {
        return UrlProtocol::QQLINK;
    } else if (lower_url.startsWith("flashget://")) {
        return UrlProtocol::FLASHGET;
    } else if (lower_url.startsWith("ed2k://")) {
        return UrlProtocol::ED2K;
    }

    // 网盘分享链接：drives 层 CloudLinkDetector 唯一事实源
    // （含 normalize_url——无 scheme 的裸域名网盘链接同样识别）
    const CloudPlatform platform = CloudLinkDetector::detect_platform(url.toStdString());
    if (platform != CloudPlatform::Unknown) {
        return to_url_protocol(platform);
    }

    // Standard protocols
    if (lower_url.startsWith("http://")) {
        return UrlProtocol::HTTP;
    } else if (lower_url.startsWith("https://")) {
        return UrlProtocol::HTTPS;
    } else if (lower_url.startsWith("ftp://") || lower_url.startsWith("ftps://")) {
        return UrlProtocol::FTP;
    }

    return UrlProtocol::UNKNOWN;
}

UrlProtocol UrlDetector::to_url_protocol(falcon::CloudPlatform platform)
{
    switch (platform) {
        case CloudPlatform::BaiduNetdisk:  return UrlProtocol::BAIDU;
        case CloudPlatform::LanzouCloud:   return UrlProtocol::LANZHOU;
        case CloudPlatform::AlibabaCloud:  return UrlProtocol::ALIYUN;
        case CloudPlatform::TencentWeiyun: return UrlProtocol::WEIYUN;
        case CloudPlatform::Cloud115:      return UrlProtocol::CLOUD115;
        case CloudPlatform::Quark:         return UrlProtocol::QUARK;
        case CloudPlatform::TianyiCloud:   return UrlProtocol::TIANYI;
        case CloudPlatform::PikPak:        return UrlProtocol::PIKPAK;
        case CloudPlatform::Mega:          return UrlProtocol::MEGA;
        case CloudPlatform::GoogleDrive:   return UrlProtocol::GDRIVE;
        case CloudPlatform::OneDrive:      return UrlProtocol::ONEDRIVE;
        case CloudPlatform::Dropbox:       return UrlProtocol::DROPBOX;
        case CloudPlatform::YandexDisk:    return UrlProtocol::YANDEX;
        default:                           return UrlProtocol::UNKNOWN;
    }
}

falcon::CloudPlatform UrlDetector::to_cloud_platform(UrlProtocol protocol)
{
    switch (protocol) {
        case UrlProtocol::BAIDU:    return CloudPlatform::BaiduNetdisk;
        case UrlProtocol::LANZHOU:  return CloudPlatform::LanzouCloud;
        case UrlProtocol::ALIYUN:   return CloudPlatform::AlibabaCloud;
        case UrlProtocol::WEIYUN:   return CloudPlatform::TencentWeiyun;
        case UrlProtocol::CLOUD115: return CloudPlatform::Cloud115;
        case UrlProtocol::QUARK:    return CloudPlatform::Quark;
        case UrlProtocol::TIANYI:   return CloudPlatform::TianyiCloud;
        case UrlProtocol::PIKPAK:   return CloudPlatform::PikPak;
        case UrlProtocol::MEGA:     return CloudPlatform::Mega;
        case UrlProtocol::GDRIVE:   return CloudPlatform::GoogleDrive;
        case UrlProtocol::ONEDRIVE: return CloudPlatform::OneDrive;
        case UrlProtocol::DROPBOX:  return CloudPlatform::Dropbox;
        case UrlProtocol::YANDEX:   return CloudPlatform::YandexDisk;
        default:                    return CloudPlatform::Unknown;
    }
}

UrlInfo UrlDetector::parse_cloud_url(const QString& url, falcon::CloudPlatform platform)
{
    UrlInfo info;
    info.protocol = to_url_protocol(platform);
    info.original_url = url;
    info.decoded_url = url;
    info.is_valid = true;

    const QString display_name =
        QString::fromStdString(CloudLinkDetector::platform_display_name(platform));
    const QString share_code = QString::fromStdString(
        CloudLinkDetector::extract_file_id(url.toStdString(), platform));

    if (share_code.isEmpty()) {
        info.file_name = display_name + " Share";
    } else {
        info.file_name = display_name + " (" + share_code + ")";
    }

    return info;
}

QString UrlDetector::parse_thunder_url(const QString& url)
{
    // thunder://URL format: thunder://BASE64_ENCODED_STRING
    // The encoded string starts with "AA" and ends with "ZZ"
    // After decoding, it starts with "http://" or "https://"

    QString encoded = url.mid(10); // Remove "thunder://"
    QString decoded = base64_decode(encoded);

    // Remove "AA" prefix and "ZZ" suffix if present
    if (decoded.startsWith("AA")) {
        decoded = decoded.mid(2);
    }
    if (decoded.endsWith("ZZ")) {
        decoded = decoded.left(decoded.length() - 2);
    }

    return decoded;
}

QString UrlDetector::parse_qqlink_url(const QString& url)
{
    // qqlink://URL format similar to thunder
    QString encoded = url.mid(9); // Remove "qqlink://"
    QString decoded = base64_decode(encoded);

    // Remove "AA" prefix and "ZZ" suffix if present
    if (decoded.startsWith("AA")) {
        decoded = decoded.mid(2);
    }
    if (decoded.endsWith("ZZ")) {
        decoded = decoded.left(decoded.length() - 2);
    }

    return decoded;
}

QString UrlDetector::parse_flashget_url(const QString& url)
{
    // flashget://URL format: flashget://BASE64[FLASHGET]
    QString without_prefix = url.mid(11); // Remove "flashget://"
    without_prefix = without_prefix.replace("[FLASHGET]", "");

    QString decoded = base64_decode(without_prefix);

    // Flashget encodes as: [FLASHGET]URL[FLASHGET]
    if (decoded.startsWith("[FLASHGET]")) {
        decoded = decoded.mid(10); // Remove "[FLASHGET]"
    }
    if (decoded.endsWith("[FLASHGET]")) {
        decoded = decoded.left(decoded.length() - 10);
    }

    return decoded;
}

UrlInfo UrlDetector::parse_magnet_url(const QString& url)
{
    UrlInfo info;
    info.protocol = UrlProtocol::MAGNET;
    info.original_url = url;
    info.decoded_url = url;
    info.is_valid = true;

    QUrl qurl(url);
    QUrlQuery query(qurl);

    // Extract display name (dn parameter)
    QString display_name = query.queryItemValue("dn");
    if (!display_name.isEmpty()) {
        info.file_name = display_name;
    } else {
        info.file_name = "Magnet Torrent";
    }

    // Extract file size if available
    QString xl = query.queryItemValue("xl");
    if (!xl.isEmpty()) {
        info.file_size = xl;
    }

    return info;
}

UrlInfo UrlDetector::parse_ed2k_url(const QString& url)
{
    UrlInfo info;
    info.protocol = UrlProtocol::ED2K;
    info.original_url = url;
    info.decoded_url = url;
    info.is_valid = true;

    // ED2K format: ed2k://|file|filename|size|hash|/
    QRegularExpressionMatch match = ED2K_PATTERN.match(url);
    if (match.hasMatch()) {
        info.file_name = match.captured(1);
        info.file_size = match.captured(2);
        // hash = match.captured(3); // Could store if needed
    } else {
        info.file_name = "ED2K File";
    }

    return info;
}

QString UrlDetector::base64_decode(const QString& data)
{
    QByteArray decoded_bytes = QByteArray::fromBase64(data.toUtf8());
    return QString::fromUtf8(decoded_bytes);
}

} // namespace falcon::desktop
