cmake_minimum_required(VERSION 3.15)

project(libfalcon
    VERSION 0.1.0
    DESCRIPTION "Falcon core download engine library"
    LANGUAGES CXX
)

# ============================================================================
# Library target
# ============================================================================

add_library(falcon
    # Core implementation
    src/download_engine.cpp
    src/download_task.cpp
    src/task_manager.cpp
    src/thread_pool.cpp
    src/plugin_manager.cpp
    src/event_dispatcher.cpp
    src/segment_downloader.cpp
    src/resource_browser.cpp
    src/password_manager.cpp
    src/version.cpp
    # aria2 风格命令系统
    src/commands/http_commands.cpp
    src/net/event_poll_epoll.cpp
    src/net/event_poll_kqueue.cpp
    src/net/event_poll_poll.cpp
    src/net/event_poll_factory.cpp
    src/net/socket_pool.cpp
    src/request_group.cpp
    src/download_engine_v2.cpp
    src/file_hash.cpp
    # Resource search - TODO: Fix compilation issues
    # src/resource_search.cpp
    # Cloud storage - TODO: Fix compilation issues
    # src/cloud_storage_plugin.cpp
    # Resource browser - TODO: Fix compilation issues
    # src/resource_browser.cpp
)

# Alias for consistent usage
add_library(Falcon::falcon ALIAS falcon)

# ============================================================================
# Include directories
# ============================================================================

target_include_directories(falcon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# Compile features
# ============================================================================

target_compile_features(falcon PUBLIC cxx_std_17)

# ============================================================================
# Link dependencies
# ============================================================================

find_package(Threads REQUIRED)

target_link_libraries(falcon
    PUBLIC
        Threads::Threads
)

# Windows Socket library
if(WIN32)
    target_link_libraries(falcon PUBLIC ws2_32)
endif()

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_link_libraries(falcon PUBLIC spdlog::spdlog)
    target_compile_definitions(falcon PUBLIC FALCON_USE_SPDLOG)
endif()

# Optional: nlohmann_json for config
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(falcon PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon PUBLIC FALCON_USE_JSON)
    target_compile_definitions(falcon PRIVATE HAVE_NLOHMANN_JSON)
endif()

# Link libcurl for HTTP requests
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(falcon PRIVATE CURL::libcurl)
    target_compile_definitions(falcon PRIVATE FALCON_USE_CURL)
endif()

# Link OpenSSL for file hash verification
find_package(OpenSSL QUIET)
if(OPENSSL_FOUND)
    target_link_libraries(falcon PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(falcon PRIVATE FALCON_USE_OPENSSL)
endif()

# ============================================================================
# Export symbols (Windows DLL)
# ============================================================================

if(BUILD_SHARED_LIBS)
    target_compile_definitions(falcon
        PRIVATE FALCON_EXPORTS
        INTERFACE FALCON_IMPORTS
    )
endif()

# ============================================================================
# Optional features
# ============================================================================

option(FALCON_ENABLE_CLOUD_STORAGE "Enable cloud storage plugins" ON)
option(FALCON_ENABLE_RESOURCE_SEARCH "Enable resource search functionality" ON)
option(FALCON_ENABLE_RESOURCE_BROWSER "Enable resource browser functionality" ON)

# ============================================================================
# Protocol plugins
# ============================================================================

# HTTP/HTTPS plugin
if(FALCON_ENABLE_HTTP)
    add_subdirectory(plugins/http)
    target_sources(falcon PRIVATE
        $<TARGET_OBJECTS:falcon_plugin_http>
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_HTTP_PLUGIN FALCON_ENABLE_HTTP)
    # Link curl to falcon directly if http plugin uses it
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(falcon PRIVATE CURL::libcurl)
    endif()
endif()

# FTP plugin
if(FALCON_ENABLE_FTP)
    add_subdirectory(plugins/ftp)
    target_link_libraries(falcon PRIVATE falcon_plugin_ftp)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_FTP_PLUGIN)
endif()

# BitTorrent plugin
if(FALCON_ENABLE_BITTORRENT)
    add_subdirectory(plugins/bittorrent)
    target_link_libraries(falcon PRIVATE falcon_plugin_bittorrent)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_BT_PLUGIN)
endif()

# SFTP plugin
if(FALCON_ENABLE_SFTP)
    add_subdirectory(plugins/sftp)
    target_link_libraries(falcon PRIVATE falcon_plugin_sftp)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_SFTP_PLUGIN)
endif()

# Metalink plugin
if(FALCON_ENABLE_METALINK)
    add_subdirectory(plugins/metalink)
    target_link_libraries(falcon PRIVATE falcon_plugin_metalink)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_METALINK_PLUGIN)
endif()

# Thunder plugin
if(FALCON_ENABLE_THUNDER)
    add_subdirectory(plugins/thunder)
    target_sources(falcon PRIVATE
        plugins/thunder/thunder_plugin.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_THUNDER_PLUGIN)
endif()

# QQDL plugin
if(FALCON_ENABLE_QQDL)
    target_sources(falcon PRIVATE
        plugins/qqdl/qqdl_plugin.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_QQDL_PLUGIN)
endif()

# FlashGet plugin
if(FALCON_ENABLE_FLASHGET)
    target_sources(falcon PRIVATE
        plugins/flashget/flashget_plugin.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_FLASHGET_PLUGIN)
endif()

# ED2K plugin
if(FALCON_ENABLE_ED2K)
    target_sources(falcon PRIVATE
        plugins/ed2k/ed2k_plugin.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_ED2K_PLUGIN)
endif()

# HLS/DASH plugin
if(FALCON_ENABLE_HLS)
    target_sources(falcon PRIVATE
        plugins/hls/hls_plugin.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_HLS_PLUGIN)
    # 需要nlohmann/json用于DASH解析
    if(NOT nlohmann_json_FOUND)
        target_compile_definitions(falcon PRIVATE FALCON_HLS_NO_JSON)
    endif()
endif()

# Cloud storage support
if(FALCON_ENABLE_CLOUD_STORAGE)
    if(NOT CURL_FOUND)
        message(WARNING "Cloud storage plugins require libcurl; disabling FALCON_ENABLE_CLOUD_STORAGE")
        set(FALCON_ENABLE_CLOUD_STORAGE OFF CACHE BOOL "Enable cloud storage plugins" FORCE)
    else()
        target_sources(falcon PRIVATE
            src/cloud_storage_plugin.cpp
        )
        target_compile_definitions(falcon PRIVATE FALCON_ENABLE_CLOUD_STORAGE)
    endif()
endif()

# Resource search functionality
if(FALCON_ENABLE_RESOURCE_SEARCH)
    if(NOT CURL_FOUND OR NOT nlohmann_json_FOUND)
        message(WARNING "Resource search requires libcurl and nlohmann_json; disabling FALCON_ENABLE_RESOURCE_SEARCH")
        set(FALCON_ENABLE_RESOURCE_SEARCH OFF CACHE BOOL "Enable resource search functionality" FORCE)
    else()
        target_sources(falcon PRIVATE
            src/resource_search.cpp
        )
        target_compile_definitions(falcon PRIVATE FALCON_ENABLE_RESOURCE_SEARCH)
        target_link_libraries(falcon PRIVATE CURL::libcurl)
    endif()
endif()

# Resource browser functionality
if(FALCON_ENABLE_RESOURCE_BROWSER)
    if(NOT CURL_FOUND OR NOT nlohmann_json_FOUND)
        message(WARNING "Resource browser requires libcurl and nlohmann_json; disabling FALCON_ENABLE_RESOURCE_BROWSER")
        set(FALCON_ENABLE_RESOURCE_BROWSER OFF CACHE BOOL "Enable resource browser functionality" FORCE)
    else()
        target_sources(falcon PRIVATE
            plugins/ftp/ftp_browser.cpp
            plugins/s3/s3_browser.cpp
        )
        target_compile_definitions(falcon PRIVATE FALCON_ENABLE_RESOURCE_BROWSER)

        find_package(OpenSSL QUIET)
        if(OpenSSL_FOUND)
            target_sources(falcon PRIVATE
                plugins/oss/oss_browser.cpp
                plugins/cos/cos_browser.cpp
                plugins/kodo/kodo_browser.cpp
                plugins/upyun/upyun_browser.cpp
            )
            target_link_libraries(falcon PRIVATE OpenSSL::Crypto)
        else()
            message(STATUS "OpenSSL not found: OSS/COS/Kodo/Upyun browsers will not be built")
        endif()
    endif()
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

# Install falcon library (HTTP plugin is embedded as OBJECT library)
# Only standalone plugins need to be installed separately
set(FALCON_PLUGIN_TARGETS)
if(FALCON_ENABLE_FTP)
    list(APPEND FALCON_PLUGIN_TARGETS falcon_plugin_ftp)
endif()
if(FALCON_ENABLE_BITTORRENT)
    list(APPEND FALCON_PLUGIN_TARGETS falcon_plugin_bittorrent)
endif()

# Install all targets
install(TARGETS falcon ${FALCON_PLUGIN_TARGETS}
    EXPORT FalconTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Add new plugins to plugin targets list
if(FALCON_ENABLE_SFTP)
    list(APPEND FALCON_PLUGIN_TARGETS falcon_plugin_sftp)
endif()
if(FALCON_ENABLE_METALINK)
    list(APPEND FALCON_PLUGIN_TARGETS falcon_plugin_metalink)
endif()

install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)

    # 设置代码覆盖率目标
    if(FALCON_ENABLE_COVERAGE)
        include(cmake/CodeCoverage.cmake OPTIONAL)
        if(COMMAND setup_coverage_target)
            setup_coverage_target(falcon_coverage
                "*/libfalcon/tests/*"
                "*/build-*/*"
            )
        endif()
    endif()
endif()
