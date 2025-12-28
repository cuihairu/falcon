/**
 * @file url_detector.hpp
 * @brief URL detector and parser for supported protocols
 * @author Falcon Team
 * @date 2025-12-28
 */

#pragma once

#include <QString>
#include <QRegularExpression>
#include <QStringList>

namespace falcon::desktop {

/**
 * @brief Supported URL protocols
 */
enum class UrlProtocol {
    HTTP,
    HTTPS,
    FTP,
    MAGNET,
    THUNDER,
    QQLINK,
    FLASHGET,
    ED2K,
    UNKNOWN
};

/**
 * @brief URL information structure
 */
struct UrlInfo {
    UrlProtocol protocol;
    QString original_url;
    QString decoded_url;
    QString file_name;
    QString file_size;
    bool is_valid;

    UrlInfo()
        : protocol(UrlProtocol::UNKNOWN)
        , is_valid(false)
    {}
};

/**
 * @brief URL detector and parser
 *
 * Detects and parses various download URL formats including:
 * - HTTP/HTTPS
 * - FTP
 * - Magnet links
 * - Thunder (迅雷)
 * - QQDL (QQ旋风)
 * - Flashget (快车)
 * - ED2K (电驴)
 */
class UrlDetector
{
public:
    /**
     * @brief Detect if text contains a supported URL
     * @param text Text to check
     * @return true if a supported URL is found
     */
    static bool contains_url(const QString& text);

    /**
     * @brief Detect and parse URL from text
     * @param text Text to parse
     * @return UrlInfo structure with parsed information
     */
    static UrlInfo parse_url(const QString& text);

    /**
     * @brief Get protocol name from enum
     * @param protocol Protocol enum
     * @return Protocol name string
     */
    static QString get_protocol_name(UrlProtocol protocol);

    /**
     * @brief Extract file name from URL
     * @param url URL string
     * @return File name or empty string if not found
     */
    static QString extract_file_name(const QString& url);

    /**
     * @brief Check if URL protocol is supported
     * @param url URL string
     * @return true if protocol is supported
     */
    static bool is_supported_protocol(const QString& url);

private:
    /**
     * @brief Detect protocol from URL
     * @param url URL string
     * @return Protocol enum
     */
    static UrlProtocol detect_protocol(const QString& url);

    /**
     * @brief Parse thunder:// URL
     * @param url Thunder URL
     * @return Decoded URL
     */
    static QString parse_thunder_url(const QString& url);

    /**
     * @brief Parse qqlink:// URL
     * @param url QQDL URL
     * @return Decoded URL
     */
    static QString parse_qqlink_url(const QString& url);

    /**
     * @brief Parse flashget:// URL
     * @param url Flashget URL
     * @return Decoded URL
     */
    static QString parse_flashget_url(const QString& url);

    /**
     * @brief Parse magnet link
     * @param url Magnet URL
     * @return UrlInfo with magnet details
     */
    static UrlInfo parse_magnet_url(const QString& url);

    /**
     * @brief Parse ed2k link
     * @param url ED2K URL
     * @return UrlInfo with ed2k details
     */
    static UrlInfo parse_ed2k_url(const QString& url);

    /**
     * @brief Base64 decode
     * @param data Base64 encoded string
     * @return Decoded string
     */
    static QString base64_decode(const QString& data);
};

} // namespace falcon::desktop
