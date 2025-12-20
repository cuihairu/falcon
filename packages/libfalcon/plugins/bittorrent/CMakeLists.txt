cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_bittorrent
    VERSION 0.1.0
    DESCRIPTION "Falcon BitTorrent/Magnet protocol plugin"
    LANGUAGES CXX
)

add_library(falcon_plugin_bittorrent
    bt_plugin.cpp
)

target_link_libraries(falcon_plugin_bittorrent
    PRIVATE
        # LibtorrentRasterbar::torrent-rasterbar
        spdlog::spdlog
)

target_compile_features(falcon_plugin_bittorrent PRIVATE cxx_std_17)
