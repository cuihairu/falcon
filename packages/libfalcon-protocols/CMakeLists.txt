cmake_minimum_required(VERSION 3.15)

project(libfalcon-protocols
    VERSION 0.1.0
    DESCRIPTION "Falcon protocol handlers library"
    LANGUAGES CXX
)

# ============================================================================
# Export set anchor（先于一切 install/add_subdirectory 计算）
#
# monorepo 树内（根已建 falcon_core）沿用 FalconTargets 单体导出（#12
# 消费路径零变化）；独立 configure（vcpkg 端口形态，本包为顶层 project）
# 自带 FalconProtocolsTargets 每包导出集。判定锚 = TARGET falcon_core
# （根必然已建该目标，端口树内必然没有——不引入第二个真值来源）。
# 插件与 vendored libmetalink 的 install 同随此锚分支——一个目标不能
# 同时进两个导出集
# ============================================================================

if(TARGET falcon_core)
    set(FALCON_PROTOCOLS_EXPORT_SET FalconTargets)
    set(FALCON_PROTOCOLS_STANDALONE OFF)
else()
    set(FALCON_PROTOCOLS_EXPORT_SET FalconProtocolsTargets)
    set(FALCON_PROTOCOLS_STANDALONE ON)
endif()

# ============================================================================
# 独立 configure 自足块（vcpkg 端口形态）
#
# monorepo 树内根已做过全部 find_package，整块跳过（零行为变化）；
# 独立形态下补齐本包消费的 finder 与上游包。option 只在未定义时兜底
# （与根 CMakeLists 的 option 同值）——定义权留根目录，避免两处描述
# 漂移。libssh2/miniupnpc/natpmp/libtorrent/expat 的查找在下方插件块
# 或 vendored 子目录内自带（目录作用域内执行），无需在此重复
# ============================================================================

if(NOT DEFINED FALCON_ENABLE_HTTP)
    set(FALCON_ENABLE_HTTP ON)
endif()
if(NOT DEFINED FALCON_ENABLE_FTP)
    set(FALCON_ENABLE_FTP ON)
endif()
if(NOT DEFINED FALCON_ENABLE_BITTORRENT)
    set(FALCON_ENABLE_BITTORRENT ON)
endif()
if(NOT DEFINED FALCON_ENABLE_SFTP)
    set(FALCON_ENABLE_SFTP ON)
endif()
if(NOT DEFINED FALCON_ENABLE_WEBDAV)
    set(FALCON_ENABLE_WEBDAV ON)
endif()
if(NOT DEFINED FALCON_ENABLE_METALINK)
    set(FALCON_ENABLE_METALINK ON)
endif()
if(NOT DEFINED FALCON_ENABLE_NAT)
    set(FALCON_ENABLE_NAT ON)
endif()
if(NOT DEFINED FALCON_ENABLE_THUNDER)
    set(FALCON_ENABLE_THUNDER OFF)
endif()
if(NOT DEFINED FALCON_ENABLE_QQDL)
    set(FALCON_ENABLE_QQDL OFF)
endif()
if(NOT DEFINED FALCON_ENABLE_FLASHGET)
    set(FALCON_ENABLE_FLASHGET OFF)
endif()
if(NOT DEFINED FALCON_ENABLE_ED2K)
    set(FALCON_ENABLE_ED2K OFF)
endif()
if(NOT DEFINED FALCON_ENABLE_HLS)
    set(FALCON_ENABLE_HLS OFF)
endif()
if(NOT DEFINED FALCON_ENABLE_FAILURE_INJECTION)
    set(FALCON_ENABLE_FAILURE_INJECTION OFF)
endif()

if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    # 本包与插件的既有链接点全部裸写 falcon_core——独立 configure 下以
    # 别名对齐 Falcon::core（目录级即可：消费方全在本目录子树内）
    add_library(falcon_core ALIAS Falcon::core)
    find_package(spdlog QUIET)
    find_package(nlohmann_json QUIET)
    # 查找门与根 cmake/Dependencies.cmake 同款（HTTP/FTP；WEBDAV 沿用
    # 同一结果的既有行为——两门全关时不找 curl）
    if(FALCON_ENABLE_HTTP OR FALCON_ENABLE_FTP)
        find_package(CURL QUIET)
    endif()
    find_package(OpenSSL QUIET)
endif()

# 故障注入宏与根目录同款触发条件，且必须先于本目录一切 add_library
# （add_compile_definitions 只作用于其后创建的目标——放晚了会导致
# falcon_protocols 与 tests 的 injection.hpp 内联布局分叉，ODR 红线）。
# monorepo 下根目录已在 add_subdirectory 前全局定义，此处跳过以免
# 重复 -D 令本包全部 TU 重编
if(NOT TARGET falcon_core
        AND (FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS))
    add_compile_definitions(FALCON_FAILURE_INJECTION=1)
endif()

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
    src/swarm_mirror_source.cpp
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

# 导出名与文档/别名一致（Falcon::protocols），理由同 falcon_core
set_target_properties(falcon_protocols PROPERTIES EXPORT_NAME protocols)

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

# SFTP plugin（libssh2：vcpkg port 存在且轻量；libssh 不在 vcpkg 稳定面）
# CONFIG 查找命中 vcpkg 安装；pkg-config 兜底覆盖发行版自装形态
find_package(libssh2 CONFIG QUIET)
if(NOT TARGET Libssh2::libssh2)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBSSH2 QUIET IMPORTED_TARGET libssh2)
    endif()
endif()
if(TARGET Libssh2::libssh2)
    set(LIBSSH2_TARGET Libssh2::libssh2)
elseif(TARGET PkgConfig::LIBSSH2)
    set(LIBSSH2_TARGET PkgConfig::LIBSSH2)
endif()

if(FALCON_ENABLE_SFTP AND LIBSSH2_TARGET)
    add_subdirectory(plugins/sftp)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_sftp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_SFTP_PLUGIN)
    # 导出闭包引用 libssh2 的 imported target（vcpkg 形态为
    # libssh2::libssh2_static），消费方由 FalconConfig 重建；
    # 本目录的 imported target 根目录不可见，只能在这里判定
    set(FALCON_PACKAGE_NEEDS_LIBSSH2 ON CACHE INTERNAL "consumer needs libssh2" FORCE)
elseif(FALCON_ENABLE_SFTP)
    message(WARNING "SFTP plugin requires libssh2; disabling FALCON_ENABLE_SFTP")
    set(FALCON_ENABLE_SFTP OFF CACHE BOOL "Enable SFTP plugin" FORCE)
endif()

# WebDAV plugin（纯 libcurl 数据面：dav→http / davs→https scheme 重写，
# Basic/Digest 经 CURLOPT_HTTPAUTH(CURLAUTH_ANY) 按 401 挑战协商）
if(FALCON_ENABLE_WEBDAV AND CURL_FOUND)
    add_subdirectory(plugins/webdav)
    target_link_libraries(falcon_protocols PRIVATE falcon_plugin_webdav)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_WEBDAV_PLUGIN)
elseif(FALCON_ENABLE_WEBDAV)
    message(WARNING "WebDAV plugin requires libcurl; disabling FALCON_ENABLE_WEBDAV")
    set(FALCON_ENABLE_WEBDAV OFF CACHE BOOL "Enable WebDAV plugin" FORCE)
endif()

# Metalink protocol（vendored libmetalink 语义解析 + expat 格式预检）
if(FALCON_ENABLE_METALINK)
    # Metalink 文档语义解析交给 vendored libmetalink（aria2 同款，MIT）；
    # XML 格式预检（行列号错误）与字段映射在 metalink_handler.cpp
    # vendored 子目录的 install 导出集由本包注入（独立形态随
    # FalconProtocolsTargets；monorepo 树内维持 FalconTargets）
    set(FALCON_METALINK_EXPORT_SET ${FALCON_PROTOCOLS_EXPORT_SET})
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/libmetalink
                     ${CMAKE_BINARY_DIR}/third_party/libmetalink)
    target_sources(falcon_protocols PRIVATE
        plugins/metalink/metalink_handler.cpp
    )
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_METALINK_PLUGIN)
    # vendored libmetalink 随本导出集一起 install（third_party
    # CMakeLists 内 install(TARGETS ... EXPORT FalconTargets)）：
    # PRIVATE 静态链接在导出接口里是 $<LINK_ONLY:Falcon::libmetalink>，
    # 消费方经 FalconConfig 的 find_dependency(expat) 拿到 expat 目标
    target_link_libraries(falcon_protocols PRIVATE
        Metalink::libmetalink)
endif()

# NAT 端口映射门面（miniupnpc = UPnP IGD + libnatpmp = NAT-PMP）。
# port_mapper.cpp 无条件编译：库缺失时工厂走返回 nullptr 的 #else 分支
# （消费方按 nullptr 优雅降级），真库用例运行时 skip。
if(FALCON_ENABLE_NAT)
    find_package(miniupnpc CONFIG QUIET)
    find_path(NATPMP_INCLUDE_DIR natpmp.h)
    find_library(NATPMP_LIBRARY NAMES natpmp)
    # libnatpmp 无 config 包（纯 C 库），手工派生 imported target：
    # 导出闭包需要目标名而非绝对路径（绝对路径导出不可重定位），
    # 消费方由 FalconConfig 按同一逻辑重建该目标
    if(NATPMP_INCLUDE_DIR AND NATPMP_LIBRARY AND NOT TARGET natpmp::natpmp)
        add_library(natpmp::natpmp UNKNOWN IMPORTED)
        set_target_properties(natpmp::natpmp PROPERTIES
            IMPORTED_LOCATION "${NATPMP_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${NATPMP_INCLUDE_DIR}")
    endif()
    target_sources(falcon_protocols PRIVATE src/net/port_mapper.cpp)
    if(TARGET miniupnpc::miniupnpc AND TARGET natpmp::natpmp)
        target_link_libraries(falcon_protocols
            PRIVATE miniupnpc::miniupnpc natpmp::natpmp)
        target_compile_definitions(falcon_protocols
            PRIVATE FALCON_ENABLE_NAT_UPNP FALCON_ENABLE_NAT_NATPMP)
        set(FALCON_NAT_BACKENDS_ENABLED ON)
        # 导出闭包引用这两个 imported target，消费方由 FalconConfig 重建；
        # 本目录创建/找到的 imported target 根目录不可见，只能在这里判定
        set(FALCON_PACKAGE_NEEDS_MINIUPNPC ON CACHE INTERNAL "consumer needs miniupnpc" FORCE)
        set(FALCON_PACKAGE_NEEDS_NATPMP ON CACHE INTERNAL "consumer needs libnatpmp" FORCE)
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
    EXPORT ${FALCON_PROTOCOLS_EXPORT_SET}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# 独立形态专属：静态 PRIVATE 链接的插件随本导出集一起 install
# （PRIVATE 依赖经 $<LINK_ONLY> 传播进导出闭包——不装则消费方链接缺
# 目标）。monorepo 树内这些 install 由根目录负责，此分支跳过。
# HTTP 插件是 OBJECT 库（对象并入 falcon_protocols 归档），永不单独
# install；falcon_builtin_protocol_handlers 同为 OBJECT 库
if(FALCON_PROTOCOLS_STANDALONE)
    foreach(_plugin falcon_plugin_ftp falcon_plugin_sftp
                    falcon_plugin_webdav falcon_plugin_bittorrent)
        if(TARGET ${_plugin})
            install(TARGETS ${_plugin}
                EXPORT ${FALCON_PROTOCOLS_EXPORT_SET}
                LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
                ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
                RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
                INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            )
        endif()
    endforeach()
endif()

# include/ 下的 BT 转发 shim（plugins/bittorrent 两枚）用相对路径指回构建树
# 源码，安装前缀下悬空，排除安装；真实头文件由下方 install(FILES) 装到
# 同一公共路径。不能靠「先装 shim 再覆盖」：file(INSTALL) 按时间戳判
# up-to-date，git 检出的源文件 mtime 全同，后装的覆盖会被跳过
install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
        REGEX "/[^/]*\\.hpp$"
        REGEX "/plugins/bittorrent" EXCLUDE
)

# BT 插件公共头安装（bittorrent_plugin.hpp quote-include 同目录的
# dht_node/pex_protocol，必须同装）。构建树不受影响（消费者仍经 include/
# 的 shim 解析）
if(TARGET falcon_plugin_bittorrent)
    install(FILES
        plugins/bittorrent/bittorrent_plugin.hpp
        plugins/bittorrent/dht_node.hpp
        plugins/bittorrent/pex_protocol.hpp
        plugins/bittorrent/bencode.hpp
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/falcon/plugins/bittorrent
    )
endif()

# 独立形态专属：每包 Config（monorepo 树内根目录负责 FalconConfig）
if(FALCON_PROTOCOLS_STANDALONE)
    # 消费侧闭包打标：与上方链接点同条件（target 存在性，非 FOUND 布尔
    # ——构建侧要求 targets 齐备才链接，导出闭包只在链接时才引用它们）。
    # LIBTORRENT/MINIUPNPC/NATPMP/LIBSSH2 复用链接点已写入的 CACHE
    # INTERNAL 标记（单一事实源），此处零二次判定
    set(FALCON_PROTOCOLS_PACKAGE_NEEDS_SPDLOG OFF)
    if(TARGET spdlog::spdlog)
        set(FALCON_PROTOCOLS_PACKAGE_NEEDS_SPDLOG ON)
    endif()
    set(FALCON_PROTOCOLS_PACKAGE_NEEDS_CURL OFF)
    if(TARGET CURL::libcurl)
        set(FALCON_PROTOCOLS_PACKAGE_NEEDS_CURL ON)
    endif()
    set(FALCON_PROTOCOLS_PACKAGE_NEEDS_OPENSSL OFF)
    if(TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
        set(FALCON_PROTOCOLS_PACKAGE_NEEDS_OPENSSL ON)
    endif()
    set(FALCON_PROTOCOLS_PACKAGE_NEEDS_EXPAT OFF)
    if(TARGET libmetalink)
        set(FALCON_PROTOCOLS_PACKAGE_NEEDS_EXPAT ON)
    endif()

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FalconProtocolsConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/FalconProtocolsConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconProtocols
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/FalconProtocolsConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT FalconProtocolsTargets
        FILE FalconProtocolsTargets.cmake
        NAMESPACE Falcon::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconProtocols
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FalconProtocolsConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FalconProtocolsConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconProtocols
    )
endif()

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
