cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_bittorrent
    VERSION 0.1.0
    DESCRIPTION "Falcon BitTorrent/Magnet protocol plugin"
    LANGUAGES CXX
)

# 选项控制是否启用 libtorrent
option(FALCON_USE_LIBTORRENT "Use libtorrent-rasterbar for BitTorrent support" ON)

add_library(falcon_plugin_bittorrent
    bittorrent_plugin.cpp
    seed_policy.cpp
    dht_node.cpp
    pex_protocol.cpp
    bencode.cpp
)

if(FALCON_USE_LIBTORRENT)
    # 优先 CMake 配置包（vcpkg 提供完整 imported target），pkg-config
    # imported target 兜底——两条路径都携带 include 路径、链接路径与
    # 传递依赖。bittorrent_plugin.hpp 在该模式下暴露 libtorrent::session
    # 成员，消费者 TU（测试等）必须拿到同款定义与链接（ODR 同布局红线，
    # 同 FALCON_USE_OPENSSL 先例）：定义 PUBLIC、依赖 PUBLIC
    find_package(LibtorrentRasterbar 2.0 QUIET)
    if(LibtorrentRasterbar_FOUND)
        set(LIBTORRENT_TARGET LibtorrentRasterbar::torrent-rasterbar)
    else()
        find_package(PkgConfig QUIET)
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBTORRENT IMPORTED_TARGET libtorrent-rasterbar)
            if(LIBTORRENT_FOUND)
                set(LIBTORRENT_TARGET PkgConfig::LIBTORRENT)
            endif()
        endif()
    endif()

    if(LIBTORRENT_TARGET)
        message(STATUS "BitTorrent plugin: Using libtorrent-rasterbar")
        target_compile_definitions(falcon_plugin_bittorrent PUBLIC FALCON_USE_LIBTORRENT)
    else()
        message(WARNING "libtorrent-rasterbar not found, BitTorrent support will be limited")
    endif()
endif()

target_include_directories(falcon_plugin_bittorrent
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

if(LIBTORRENT_TARGET)
    target_link_libraries(falcon_plugin_bittorrent PUBLIC ${LIBTORRENT_TARGET})
endif()

target_link_libraries(falcon_plugin_bittorrent
    PRIVATE
        falcon_core
        OpenSSL::SSL
        OpenSSL::Crypto
)

if(spdlog_FOUND)
    target_link_libraries(falcon_plugin_bittorrent PRIVATE spdlog::spdlog)
    target_compile_definitions(falcon_plugin_bittorrent PRIVATE FALCON_USE_SPDLOG)
endif()

target_compile_features(falcon_plugin_bittorrent PRIVATE cxx_std_17)

# 平台特定设置
if(UNIX AND NOT APPLE)
    target_link_libraries(falcon_plugin_bittorrent PRIVATE pthread)
endif()
