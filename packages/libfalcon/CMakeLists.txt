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
    # Resource search
    src/resource_search.cpp
    # Cloud storage
    src/cloud_storage_plugin.cpp
    # Resource browser
    src/resource_browser.cpp
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
endif()

# Link libcurl for HTTP requests
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(falcon PRIVATE CURL::libcurl)
    target_compile_definitions(falcon PRIVATE FALCON_USE_CURL)
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
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_HTTP_PLUGIN)
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
    target_sources(falcon PRIVATE
        src/cloud_storage_plugin.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_CLOUD_STORAGE)
    # 需要libcurl用于HTTP请求
    if(NOT CURL_FOUND)
        message(WARNING "Cloud storage plugins require libcurl")
    endif()
endif()

# Resource search functionality
if(FALCON_ENABLE_RESOURCE_SEARCH)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_RESOURCE_SEARCH)
    # 需要nlohmann/json用于配置解析
    if(NOT nlohmann_json_FOUND)
        message(WARNING "Resource search requires nlohmann_json")
    endif()
    # 需要libcurl用于网页请求
    if(NOT CURL_FOUND)
        message(WARNING "Resource search requires libcurl")
    endif()
endif()

# Resource browser functionality
if(FALCON_ENABLE_RESOURCE_BROWSER)
    target_sources(falcon PRIVATE
        plugins/ftp/ftp_browser.cpp
        plugins/s3/s3_browser.cpp
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_RESOURCE_BROWSER)
    # 需要libcurl用于FTP/S3请求
    if(NOT CURL_FOUND)
        message(WARNING "Resource browser requires libcurl")
    endif()
    # 需要nlohmann/json用于S3解析
    if(NOT nlohmann_json_FOUND)
        target_compile_definitions(falcon PRIVATE FALCON_BROWSER_NO_JSON)
    endif()
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon
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
