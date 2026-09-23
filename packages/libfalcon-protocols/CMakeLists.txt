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
    src/v2_engine_host.cpp
    src/segment_downloader.cpp
    src/incremental_download.cpp
    src/request_group.cpp
    src/resume_control.cpp
    src/file_hash.cpp
    src/commands/command.cpp
    src/commands/http_commands.cpp
    src/net/event_poll_poll.cpp
    src/net/event_poll_factory.cpp
    src/net/socket_pool.cpp
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

# Support both OpenSSL_FOUND (modern) and OPENSSL_FOUND (legacy)
# When using vcpkg, the targets may not be available during the if() check
# but will be available when target_link_libraries is called
message(STATUS "OpenSSL_FOUND: ${OpenSSL_FOUND}, OPENSSL_FOUND: ${OPENSSL_FOUND}")
if(OpenSSL_FOUND)
    message(STATUS "Using OpenSSL_FOUND branch")
    target_link_libraries(falcon_protocols PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    # PUBLIC：http_commands.hpp 等公共头按这些宏条件声明成员，消费方（tests/
    # daemon/CLI）必须与库看到同一布局，否则 ODR 错位；测试同样借此启用
    # OpenSSL 路径（否则 file_hash/incremental_download 全套运行时 skip）
    target_compile_definitions(falcon_protocols PUBLIC FALCON_USE_OPENSSL FALCON_ENABLE_OPENSSL)
elseif(OPENSSL_FOUND)
    message(STATUS "Using OPENSSL_FOUND branch")
    target_link_libraries(falcon_protocols PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(falcon_protocols PUBLIC FALCON_USE_OPENSSL FALCON_ENABLE_OPENSSL)
else()
    message(STATUS "OpenSSL not found, TLS support will be disabled")
endif()

# ============================================================================
# Protocol plugins
# ============================================================================

# HTTP/HTTPS plugin
if(FALCON_ENABLE_HTTP AND CURL_FOUND)
    add_subdirectory(plugins/http)
    target_sources(falcon_protocols PRIVATE
        $<TARGET_OBJECTS:falcon_plugin_http>
    )
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_HTTP_PLUGIN FALCON_ENABLE_HTTP)
elseif(FALCON_ENABLE_HTTP)
    message(WARNING "HTTP plugin requires libcurl; disabling FALCON_ENABLE_HTTP")
    set(FALCON_ENABLE_HTTP OFF CACHE BOOL "Enable HTTP/HTTPS plugin" FORCE)
endif()

# FTP plugin
if(FALCON_ENABLE_FTP AND CURL_FOUND)
    add_subdirectory(plugins/ftp)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_ftp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_FTP_PLUGIN)
elseif(FALCON_ENABLE_FTP)
    message(WARNING "FTP plugin requires libcurl; disabling FALCON_ENABLE_FTP")
    set(FALCON_ENABLE_FTP OFF CACHE BOOL "Enable FTP plugin" FORCE)
endif()

# BitTorrent plugin
if(FALCON_ENABLE_BITTORRENT AND OPENSSL_FOUND AND TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
    add_subdirectory(plugins/bittorrent)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_bittorrent)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_BT_PLUGIN)
elseif(FALCON_ENABLE_BITTORRENT)
    message(WARNING "BitTorrent plugin requires OpenSSL; disabling FALCON_ENABLE_BITTORRENT")
    set(FALCON_ENABLE_BITTORRENT OFF CACHE BOOL "Enable BitTorrent plugin" FORCE)
endif()

# SFTP plugin
if(FALCON_ENABLE_SFTP AND LIBSSH_FOUND)
    add_subdirectory(plugins/sftp)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_sftp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_SFTP_PLUGIN)
elseif(FALCON_ENABLE_SFTP)
    message(WARNING "SFTP plugin requires libssh; disabling FALCON_ENABLE_SFTP")
    set(FALCON_ENABLE_SFTP OFF CACHE BOOL "Enable SFTP plugin" FORCE)
endif()

# Metalink protocol(手写 mini XML 解析器,零第三方依赖)
if(FALCON_ENABLE_METALINK)
    target_sources(falcon_protocols PRIVATE
        plugins/metalink/mini_xml_parser.cpp
        plugins/metalink/metalink_handler.cpp
    )
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_METALINK_PLUGIN)
endif()

# NAT 端口映射门面（miniupnpc = UPnP IGD + libnatpmp = NAT-PMP）。
# port_mapper.cpp 无条件编译：库缺失时工厂走返回 nullptr 的 #else 分支
# （消费方按 nullptr 优雅降级），真库用例运行时 skip。
if(FALCON_ENABLE_NAT)
    find_package(miniupnpc CONFIG QUIET)
    find_path(NATPMP_INCLUDE_DIR natpmp.h)
    find_library(NATPMP_LIBRARY NAMES natpmp)
    target_sources(falcon_protocols PRIVATE src/net/port_mapper.cpp)
    if(TARGET miniupnpc::miniupnpc AND NATPMP_INCLUDE_DIR AND NATPMP_LIBRARY)
        target_link_libraries(falcon_protocols
            PRIVATE miniupnpc::miniupnpc ${NATPMP_LIBRARY})
        target_include_directories(falcon_protocols
            PRIVATE ${NATPMP_INCLUDE_DIR})
        target_compile_definitions(falcon_protocols
            PRIVATE FALCON_ENABLE_NAT_UPNP FALCON_ENABLE_NAT_NATPMP)
        set(FALCON_NAT_BACKENDS_ENABLED ON)
        if(WIN32)
            # 两库静态链接时的 Winsock 依赖与 dllexport 抑制宏
            # （vcpkg usage 要求消费方显式定义 *_STATICLIB）
            target_link_libraries(falcon_protocols PRIVATE ws2_32 iphlpapi)
            target_compile_definitions(falcon_protocols
                PRIVATE NATPMP_STATICLIB MINIUPNP_STATICLIB)
        endif()
    else()
        message(WARNING
            "NAT port mapping libraries not found "
            "(miniupnpc target: ${miniupnpc_FOUND}, "
            "natpmp.h: ${NATPMP_INCLUDE_DIR}, libnatpmp: ${NATPMP_LIBRARY}); "
            "port mapping backends will report unavailable at runtime")
    endif()
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
# 内置协议处理器注册锚点（对象库）
#
# register_builtin_protocol_handlers 的强符号定义必须以目标文件形式进入
# 最终链接，否则静态库场景下 core 中的弱符号 stub 可能优先被解析，
# 导致可执行文件（CLI/Daemon）拿不到任何内置协议处理器。
# 需要内置协议的可执行文件除链接 Falcon::protocols 外，
# 还应链接 falcon_builtin_protocol_handlers 以强制纳入强符号定义。
# ============================================================================

add_library(falcon_builtin_protocol_handlers OBJECT
    src/builtin_protocol_handlers.cpp
)
add_library(Falcon::builtin_protocol_handlers ALIAS falcon_builtin_protocol_handlers)

target_compile_features(falcon_builtin_protocol_handlers PRIVATE cxx_std_17)

target_link_libraries(falcon_builtin_protocol_handlers
    PRIVATE
        falcon_core
)

target_include_directories(falcon_builtin_protocol_handlers
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(WIN32)
    target_compile_definitions(falcon_builtin_protocol_handlers PRIVATE NOMINMAX)
endif()

# 镜像 falcon_protocols 的插件编译宏，保证强符号与库内编译结果一致
get_target_property(FALCON_PROTO_COMPILE_DEFS falcon_protocols COMPILE_DEFINITIONS)
if(FALCON_PROTO_COMPILE_DEFS)
    target_compile_definitions(falcon_builtin_protocol_handlers
        PRIVATE ${FALCON_PROTO_COMPILE_DEFS}
    )
endif()

# 对象同时进入 falcon_protocols 归档，保持库内容不变
target_sources(falcon_protocols PRIVATE
    $<TARGET_OBJECTS:falcon_builtin_protocol_handlers>
)

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
