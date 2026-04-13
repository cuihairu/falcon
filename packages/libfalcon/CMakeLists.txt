cmake_minimum_required(VERSION 3.15)

project(libfalcon
    VERSION 0.1.0
    DESCRIPTION "Falcon core download engine library"
    LANGUAGES CXX
)

# ============================================================================
# Optional features
# ============================================================================

option(FALCON_ENABLE_CLOUD_STORAGE "Enable cloud storage plugins" ON)
option(FALCON_ENABLE_RESOURCE_SEARCH "Enable resource search functionality" ON)
option(FALCON_ENABLE_RESOURCE_BROWSER "Enable resource browser functionality" ON)
option(FALCON_ENABLE_CONFIG_MANAGER "Enable encrypted config manager (SQLite3)" ON)

# ============================================================================
# Shared helpers
# ============================================================================

function(falcon_configure_target target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_features(${target} PUBLIC cxx_std_17)

    if(BUILD_SHARED_LIBS)
        target_compile_definitions(${target}
            PRIVATE FALCON_EXPORTS
            INTERFACE FALCON_IMPORTS
        )
    endif()
endfunction()

# ============================================================================
# Dependencies
# ============================================================================

find_package(Threads REQUIRED)
find_package(spdlog QUIET)
find_package(nlohmann_json QUIET)
find_package(CURL QUIET)
find_package(OpenSSL QUIET)
find_package(SQLite3 QUIET)

# ============================================================================
# Core library
# ============================================================================

add_library(falcon_core
    src/download_engine.cpp
    src/download_task.cpp
    src/task_manager.cpp
    src/thread_pool.cpp
    src/plugin_manager.cpp
    src/event_dispatcher.cpp
    src/password_manager.cpp
    src/version.cpp
)

add_library(Falcon::core ALIAS falcon_core)
falcon_configure_target(falcon_core)

target_link_libraries(falcon_core
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(falcon_core PUBLIC ws2_32)
endif()

if(spdlog_FOUND)
    target_link_libraries(falcon_core PUBLIC spdlog::spdlog)
    target_compile_definitions(falcon_core PUBLIC FALCON_USE_SPDLOG)
endif()

if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
    target_link_libraries(falcon_core PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(falcon_core PRIVATE FALCON_USE_OPENSSL)
elseif(OPENSSL_FOUND)
    message(WARNING "OpenSSL found without imported CMake targets; OpenSSL-dependent features will remain disabled")
endif()

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
falcon_configure_target(falcon_protocols)

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

# Optional protocol handlers that are currently built from source files directly
if(FALCON_ENABLE_THUNDER)
    add_subdirectory(plugins/thunder)
    target_sources(falcon_protocols PRIVATE plugins/thunder/thunder_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_THUNDER_PLUGIN)
endif()

if(FALCON_ENABLE_QQDL)
    target_sources(falcon_protocols PRIVATE plugins/qqdl/qqdl_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_QQDL_PLUGIN)
endif()

if(FALCON_ENABLE_FLASHGET)
    target_sources(falcon_protocols PRIVATE plugins/flashget/flashget_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_FLASHGET_PLUGIN)
endif()

if(FALCON_ENABLE_ED2K)
    target_sources(falcon_protocols PRIVATE plugins/ed2k/ed2k_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_ED2K_PLUGIN)
endif()

if(FALCON_ENABLE_HLS)
    target_sources(falcon_protocols PRIVATE plugins/hls/hls_plugin.cpp)
    target_compile_definitions(falcon_protocols PRIVATE FALCON_ENABLE_HLS_PLUGIN)
    if(NOT nlohmann_json_FOUND)
        target_compile_definitions(falcon_protocols PRIVATE FALCON_HLS_NO_JSON)
    endif()
endif()

# ============================================================================
# Storage library
# ============================================================================

add_library(falcon_storage
    src/resource_browser.cpp
    src/resource_browser_utils.cpp
)

add_library(Falcon::storage ALIAS falcon_storage)
falcon_configure_target(falcon_storage)

target_link_libraries(falcon_storage
    PUBLIC
        falcon_core
)

if(FALCON_ENABLE_RESOURCE_BROWSER)
    if(NOT CURL_FOUND OR NOT nlohmann_json_FOUND)
        message(WARNING "Resource browser requires libcurl and nlohmann_json; disabling FALCON_ENABLE_RESOURCE_BROWSER")
        set(FALCON_ENABLE_RESOURCE_BROWSER OFF CACHE BOOL "Enable resource browser functionality" FORCE)
    else()
        target_sources(falcon_storage PRIVATE
            plugins/ftp/ftp_browser.cpp
            plugins/s3/s3_browser.cpp
        )
        target_link_libraries(falcon_storage PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_storage PRIVATE FALCON_ENABLE_RESOURCE_BROWSER)

        if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto)
            target_sources(falcon_storage PRIVATE
                plugins/oss/oss_browser.cpp
                plugins/cos/cos_browser.cpp
                plugins/kodo/kodo_browser.cpp
                plugins/upyun/upyun_browser.cpp
            )
            target_link_libraries(falcon_storage PUBLIC OpenSSL::Crypto)
        else()
            message(STATUS "OpenSSL not found: OSS/COS/Kodo/Upyun browsers will not be built")
        endif()
    endif()
endif()

# ============================================================================
# Drives library
# ============================================================================

add_library(falcon_drives)

add_library(Falcon::drives ALIAS falcon_drives)
falcon_configure_target(falcon_drives)

target_link_libraries(falcon_drives
    PUBLIC
        falcon_core
)

if(nlohmann_json_FOUND)
    target_link_libraries(falcon_drives PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon_drives PUBLIC FALCON_USE_JSON)
    target_compile_definitions(falcon_drives PRIVATE HAVE_NLOHMANN_JSON)
endif()

if(FALCON_ENABLE_CLOUD_STORAGE)
    if(NOT CURL_FOUND)
        message(WARNING "Cloud storage plugins require libcurl; disabling FALCON_ENABLE_CLOUD_STORAGE")
        set(FALCON_ENABLE_CLOUD_STORAGE OFF CACHE BOOL "Enable cloud storage plugins" FORCE)
    else()
        target_sources(falcon_drives PRIVATE src/cloud_storage_plugin.cpp)
        target_link_libraries(falcon_drives PUBLIC CURL::libcurl)
        target_compile_definitions(falcon_drives PRIVATE FALCON_ENABLE_CLOUD_STORAGE)
    endif()
endif()

if(FALCON_ENABLE_RESOURCE_SEARCH)
    if(NOT CURL_FOUND OR NOT nlohmann_json_FOUND)
        message(WARNING "Resource search requires libcurl and nlohmann_json; disabling FALCON_ENABLE_RESOURCE_SEARCH")
        set(FALCON_ENABLE_RESOURCE_SEARCH OFF CACHE BOOL "Enable resource search functionality" FORCE)
    else()
        target_sources(falcon_drives PRIVATE src/resource_search.cpp)
        target_link_libraries(falcon_drives PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_drives PRIVATE FALCON_ENABLE_RESOURCE_SEARCH)
    endif()
endif()

if(FALCON_ENABLE_CONFIG_MANAGER)
    if(NOT SQLite3_FOUND OR NOT OPENSSL_FOUND OR NOT TARGET OpenSSL::Crypto OR NOT TARGET OpenSSL::SSL OR NOT nlohmann_json_FOUND)
        message(WARNING "Config manager requires SQLite3, OpenSSL, and nlohmann_json; disabling FALCON_ENABLE_CONFIG_MANAGER")
        set(FALCON_ENABLE_CONFIG_MANAGER OFF CACHE BOOL "Enable encrypted config manager (SQLite3)" FORCE)
    else()
        target_sources(falcon_drives PRIVATE src/config_manager.cpp)

        if(TARGET SQLite::SQLite3)
            target_link_libraries(falcon_drives PUBLIC SQLite::SQLite3)
        elseif(TARGET SQLite3::SQLite3)
            target_link_libraries(falcon_drives PUBLIC SQLite3::SQLite3)
        else()
            target_include_directories(falcon_drives PRIVATE ${SQLite3_INCLUDE_DIRS})
            target_link_libraries(falcon_drives PUBLIC ${SQLite3_LIBRARIES})
        endif()

        target_link_libraries(falcon_drives PUBLIC OpenSSL::Crypto OpenSSL::SSL nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_drives PUBLIC FALCON_ENABLE_CONFIG_MANAGER)
    endif()
endif()

# ============================================================================
# Compatibility aggregate target
# ============================================================================

add_library(falcon INTERFACE)
add_library(Falcon::falcon ALIAS falcon)

target_link_libraries(falcon
    INTERFACE
        falcon_core
        falcon_protocols
        falcon_storage
        falcon_drives
)

target_include_directories(falcon
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

set(FALCON_INTERNAL_EXPORT_TARGETS)
if(TARGET falcon_plugin_ftp)
    list(APPEND FALCON_INTERNAL_EXPORT_TARGETS falcon_plugin_ftp)
endif()
if(TARGET falcon_plugin_bittorrent)
    list(APPEND FALCON_INTERNAL_EXPORT_TARGETS falcon_plugin_bittorrent)
endif()
if(TARGET falcon_plugin_sftp)
    list(APPEND FALCON_INTERNAL_EXPORT_TARGETS falcon_plugin_sftp)
endif()
if(TARGET falcon_plugin_metalink)
    list(APPEND FALCON_INTERNAL_EXPORT_TARGETS falcon_plugin_metalink)
endif()

install(TARGETS
    falcon_core
    falcon_protocols
    falcon_storage
    falcon_drives
    ${FALCON_INTERNAL_EXPORT_TARGETS}
    EXPORT FalconTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS falcon
    EXPORT FalconTargets
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
