cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_bittorrent
    VERSION 0.1.0
    DESCRIPTION "Falcon BitTorrent/Magnet protocol plugin"
    LANGUAGES CXX
)

# 选项控制是否启用 libtorrent
option(FALCON_USE_LIBTORRENT "Use libtorrent-rasterbar for BitTorrent support" ON)

if(FALCON_USE_LIBTORRENT)
    # 查找 libtorrent-rasterbar
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBTORRENT libtorrent-rasterbar QUIET)
    endif()

    if(NOT LIBTORRENT_FOUND)
        find_package(LibtorrentRasterbar 2.0 QUIET)
        if(LibtorrentRasterbar_FOUND)
            set(LIBTORRENT_FOUND TRUE)
            set(LIBTORRENT_LIBRARIES LibtorrentRasterbar::torrent-rasterbar)
            set(LIBTORRENT_INCLUDE_DIRS "")
        endif()
    endif()

    if(LIBTORRENT_FOUND)
        message(STATUS "BitTorrent plugin: Using libtorrent-rasterbar")
        add_compile_definitions(FALCON_USE_LIBTORRENT)
    else()
        message(WARNING "libtorrent-rasterbar not found, BitTorrent support will be limited")
    endif()
endif()

add_library(falcon_plugin_bittorrent
    bittorrent_plugin.cpp
)

target_include_directories(falcon_plugin_bittorrent
    PRIVATE
        ${CMAKE_SOURCE_DIR}/packages/libfalcon/include
        ${LIBTORRENT_INCLUDE_DIRS}
)

target_link_libraries(falcon_plugin_bittorrent
    PRIVATE
        ${LIBTORRENT_LIBRARIES}
        OpenSSL::SSL
        OpenSSL::Crypto
        spdlog::spdlog
)

target_compile_features(falcon_plugin_bittorrent PRIVATE cxx_std_17)

# 平台特定设置
if(UNIX AND NOT APPLE)
    target_link_libraries(falcon_plugin_bittorrent PRIVATE pthread)
endif()

# 测试
if(FALCON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
