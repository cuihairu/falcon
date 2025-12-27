# Falcon Project Dependencies
# ============================================================================
# This file manages all external dependencies for the Falcon project.
# Dependencies are optional where possible to allow minimal builds.
# ============================================================================

# ============================================================================
# Threads (Required)
# ============================================================================
find_package(Threads REQUIRED)

# ============================================================================
# spdlog - Logging (Optional)
# ============================================================================
find_package(spdlog QUIET)
if(spdlog_FOUND)
    message(STATUS "Found spdlog: logging enabled")
else()
    message(STATUS "spdlog not found: using basic logging")
endif()

# ============================================================================
# nlohmann_json - JSON parsing (Optional)
# ============================================================================
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "Found nlohmann_json: JSON config enabled")
else()
    message(STATUS "nlohmann_json not found: JSON config disabled")
endif()

# ============================================================================
# libcurl - HTTP/FTP support (Optional, for plugins)
# ============================================================================
if(FALCON_ENABLE_HTTP OR FALCON_ENABLE_FTP)
    find_package(CURL QUIET)
    if(CURL_FOUND)
        message(STATUS "Found libcurl: HTTP/FTP plugins enabled")
    else()
        message(STATUS "libcurl not found: HTTP/FTP plugins will have limited functionality")
    endif()
endif()

# ============================================================================
# libtorrent-rasterbar - BitTorrent support (Optional)
# ============================================================================
if(FALCON_ENABLE_BITTORRENT)
    find_package(LibtorrentRasterbar QUIET)
    if(LibtorrentRasterbar_FOUND)
        message(STATUS "Found libtorrent: BitTorrent plugin enabled")
    else()
        message(WARNING "libtorrent not found: BitTorrent plugin will have limited functionality")
    endif()
endif()

# ============================================================================
# libssh - SFTP support (Optional)
# ============================================================================
if(FALCON_ENABLE_SFTP)
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBSSH libssh QUIET)
    endif()

    if(NOT LIBSSH_FOUND)
        find_package(Libssh 0.9 QUIET)
        if(Libssh_FOUND)
            set(LIBSSH_FOUND TRUE)
            set(LIBSSH_LIBRARIES Libssh::Libssh)
        endif()
    endif()

    if(LIBSSH_FOUND)
        message(STATUS "Found libssh: SFTP plugin enabled")
        add_compile_definitions(FALCON_USE_LIBSSH)
    else()
        message(WARNING "libssh not found: SFTP plugin will have limited functionality")
    endif()
endif()

# ============================================================================
# gRPC - RPC framework (Optional, for daemon)
# ============================================================================
if(FALCON_BUILD_DAEMON)
    find_package(gRPC CONFIG QUIET)
    if(gRPC_FOUND)
        message(STATUS "Found gRPC: RPC interface enabled")
    else()
        message(STATUS "gRPC not found: Daemon will use REST API")
    endif()
endif()

# ============================================================================
# Google Test - Testing framework (Optional)
# ============================================================================
if(FALCON_BUILD_TESTS)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        include(GoogleTest)
        message(STATUS "Found GTest: tests enabled")
    else()
        message(STATUS "GTest not found: will fetch via FetchContent")
    endif()
endif()

# ============================================================================
# Code coverage (Optional)
# ============================================================================
if(FALCON_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(--coverage -fprofile-arcs -ftest-coverage)
        add_link_options(--coverage)
        message(STATUS "Code coverage enabled")
    else()
        message(WARNING "Code coverage only supported on GCC/Clang")
    endif()
endif()
