cmake_minimum_required(VERSION 3.15)

project(libfalcon-protocols
    VERSION 0.1.0
    DESCRIPTION "Falcon protocol handlers library"
    LANGUAGES CXX
)

# ============================================================================
# Protocols library
# ============================================================================

add_library(falcon_protocols
    src/download_engine_v2.cpp
    src/segment_downloader.cpp
    src/incremental_download.cpp
    src/request_group.cpp
    src/file_hash.cpp
    src/commands/command.cpp
    src/commands/http_commands.cpp
    src/net/event_poll_poll.cpp
    src/net/event_poll_factory.cpp
    src/net/socket_pool.cpp
    src/builtin_protocol_handlers.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_sources(falcon_protocols PRIVATE src/net/event_poll_epoll.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
       CMAKE_SYSTEM_NAME STREQUAL "FreeBSD" OR
       CMAKE_SYSTEM_NAME STREQUAL "NetBSD" OR
       CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
    target_sources(falcon_protocols PRIVATE src/net/event_poll_kqueue.cpp)
endif()

add_library(Falcon::protocols ALIAS falcon_protocols)

target_include_directories(falcon_protocols
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_protocols PUBLIC cxx_std_17)

# Prevent Windows.h from defining min/max macros
if(WIN32)
    target_compile_definitions(falcon_protocols PUBLIC NOMINMAX)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(falcon_protocols
        PRIVATE FALCON_EXPORTS
        INTERFACE FALCON_IMPORTS
    )
endif()

target_link_libraries(falcon_protocols
    PUBLIC
        falcon_core
)

if(CURL_FOUND)
    target_link_libraries(falcon_protocols PUBLIC CURL::libcurl)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_USE_CURL)
endif()

if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
    target_link_libraries(falcon_protocols PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_USE_OPENSSL)
endif()

# ============================================================================
# Protocol plugins
# ============================================================================

# HTTP/HTTPS plugin
if(FALCON_ENABLE_HTTP)
    add_subdirectory(plugins/http)
    target_sources(falcon_protocols PRIVATE
        $<TARGET_OBJECTS:falcon_plugin_http>
    )
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_HTTP_PLUGIN FALCON_ENABLE_HTTP)
endif()

# FTP plugin
if(FALCON_ENABLE_FTP)
    add_subdirectory(plugins/ftp)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_ftp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_FTP_PLUGIN)
endif()

# BitTorrent plugin
if(FALCON_ENABLE_BITTORRENT)
    add_subdirectory(plugins/bittorrent)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_bittorrent)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_BT_PLUGIN)
endif()

# SFTP plugin
if(FALCON_ENABLE_SFTP)
    add_subdirectory(plugins/sftp)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_sftp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_SFTP_PLUGIN)
endif()

# Metalink plugin
if(FALCON_ENABLE_METALINK)
    add_subdirectory(plugins/metalink)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_metalink)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_METALINK_PLUGIN)
endif()

# Thunder protocol
if(FALCON_ENABLE_THUNDER)
    add_subdirectory(plugins/thunder)
    target_sources(falcon_protocols PRIVATE plugins/thunder/thunder_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_THUNDER_PLUGIN)
endif()

# QQDL protocol
if(FALCON_ENABLE_QQDL)
    target_sources(falcon_protocols PRIVATE plugins/qqdl/qqdl_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_QQDL_PLUGIN)
endif()

# FlashGet protocol
if(FALCON_ENABLE_FLASHGET)
    target_sources(falcon_protocols PRIVATE plugins/flashget/flashget_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_FLASHGET_PLUGIN)
endif()

# ED2K protocol
if(FALCON_ENABLE_ED2K)
    target_sources(falcon_protocols PRIVATE plugins/ed2k/ed2k_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_ED2K_PLUGIN)
endif()

# HLS/DASH protocol
if(FALCON_ENABLE_HLS)
    target_sources(falcon_protocols PRIVATE plugins/hls/hls_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_HLS_PLUGIN)
    if(NOT nlohmann_json_FOUND)
        target_compile_definitions(falcon_protocols PRIVATE FALCON_HLS_NO_JSON)
    endif()
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon_protocols
    EXPORT FalconTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
