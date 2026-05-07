#include <falcon/protocol_registry.hpp>

#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
#include "../plugins/http/http_handler.hpp"
#endif
#ifdef FALCON_ENABLE_FTP_PLUGIN
#include "../plugins/ftp/ftp_handler.hpp"
#endif
#ifdef FALCON_ENABLE_BT_PLUGIN
#include "../plugins/bittorrent/bittorrent_plugin.hpp"
#endif

namespace falcon {

std::vector<BuiltinProtocolInfo> describe_builtin_protocols() {
    std::vector<BuiltinProtocolInfo> protocols;

#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
    protocols.push_back({"http", "HTTP/HTTPS", {"http", "https"}, true, true});
#else
    protocols.push_back({"http", "HTTP/HTTPS", {"http", "https"}, false, false});
#endif

#ifdef FALCON_ENABLE_FTP_PLUGIN
    protocols.push_back({"ftp", "FTP/FTPS", {"ftp", "ftps"}, true, true});
#else
    protocols.push_back({"ftp", "FTP/FTPS", {"ftp", "ftps"}, false, false});
#endif

#ifdef FALCON_ENABLE_BT_PLUGIN
    protocols.push_back({"bittorrent", "BitTorrent/Magnet", {"magnet", "torrent", "bittorrent"}, true, false});
#else
    protocols.push_back({"bittorrent", "BitTorrent/Magnet", {"magnet", "torrent", "bittorrent"}, false, false});
#endif

#ifdef FALCON_ENABLE_SFTP_PLUGIN
    protocols.push_back({"sftp", "SFTP", {"sftp"}, true, false});
#else
    protocols.push_back({"sftp", "SFTP", {"sftp"}, false, false});
#endif

#ifdef FALCON_ENABLE_THUNDER_PLUGIN
    protocols.push_back({"thunder", "Thunder", {"thunder", "thunderxl"}, true, false});
#else
    protocols.push_back({"thunder", "Thunder", {"thunder", "thunderxl"}, false, false});
#endif

#ifdef FALCON_ENABLE_QQDL_PLUGIN
    protocols.push_back({"qqdl", "QQDL", {"qqlink"}, true, false});
#else
    protocols.push_back({"qqdl", "QQDL", {"qqlink"}, false, false});
#endif

#ifdef FALCON_ENABLE_FLASHGET_PLUGIN
    protocols.push_back({"flashget", "FlashGet", {"flashget"}, true, false});
#else
    protocols.push_back({"flashget", "FlashGet", {"flashget"}, false, false});
#endif

#ifdef FALCON_ENABLE_ED2K_PLUGIN
    protocols.push_back({"ed2k", "ED2K", {"ed2k"}, true, false});
#else
    protocols.push_back({"ed2k", "ED2K", {"ed2k"}, false, false});
#endif

#ifdef FALCON_ENABLE_HLS_PLUGIN
    protocols.push_back({"hls", "HLS/DASH", {"hls", "m3u8", "mpd"}, true, false});
#else
    protocols.push_back({"hls", "HLS/DASH", {"hls", "m3u8", "mpd"}, false, false});
#endif

    return protocols;
}

void register_builtin_protocol_handlers(ProtocolRegistry& registry) {
#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
    registry.register_handler(protocols::create_http_handler());
#endif

#ifdef FALCON_ENABLE_FTP_PLUGIN
    registry.register_handler(protocols::create_ftp_handler());
#endif

#ifdef FALCON_ENABLE_BT_PLUGIN
    registry.register_handler(protocols::create_bittorrent_handler());
#endif
}

} // namespace falcon
