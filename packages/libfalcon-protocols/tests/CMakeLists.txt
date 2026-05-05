# CMakeLists.txt for libfalcon-protocols Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_protocols_tests_project
    VERSION 0.1.0
    DESCRIPTION "Falcon protocols library tests"
    LANGUAGES CXX
)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping libfalcon-protocols tests")
    return()
endif()

enable_testing()

# ============================================================================
# Helper functions
# ============================================================================
function(add_test_compile_options target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_17)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O2>
        )
    endif()
endfunction()

function(add_test_includes target_name)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
endfunction()

# ============================================================================
# Protocols unit tests
# ============================================================================
add_executable(falcon_protocols_tests
    unit/segment_downloader_test.cpp
    unit/incremental_download_test.cpp
    unit/request_group_test.cpp
    unit/file_hash_test.cpp
    unit/command_test.cpp
    unit/event_poll_test.cpp
    unit/socket_pool_test.cpp
    unit/cloud_protocols_test.cpp
    unit/builtin_protocol_handlers_test.cpp
)

# download_engine_v2_test uses POSIX pipe/close, skip on Windows
if(NOT WIN32)
    target_sources(falcon_protocols_tests PRIVATE unit/download_engine_v2_test.cpp)
endif()

# Conditional plugin tests
if(FALCON_ENABLE_BITTORRENT)
    target_sources(falcon_protocols_tests PRIVATE unit/bittorrent_plugin_test.cpp)
endif()

if(FALCON_ENABLE_ED2K)
    target_sources(falcon_protocols_tests PRIVATE unit/ed2k_plugin_test.cpp)
endif()

if(FALCON_ENABLE_FLASHGET)
    target_sources(falcon_protocols_tests PRIVATE unit/flashget_plugin_test.cpp)
endif()

if(FALCON_ENABLE_HLS)
    target_sources(falcon_protocols_tests PRIVATE unit/hls_plugin_test.cpp)
endif()

if(FALCON_ENABLE_THUNDER)
    target_sources(falcon_protocols_tests PRIVATE unit/thunder_plugin_test.cpp)
endif()

if(FALCON_ENABLE_QQDL)
    target_sources(falcon_protocols_tests PRIVATE unit/qqdl_plugin_test.cpp)
endif()

target_link_libraries(falcon_protocols_tests
    PRIVATE
        falcon_protocols
        falcon_core
        GTest::gtest_main
)

add_test_compile_options(falcon_protocols_tests)
add_test_includes(falcon_protocols_tests)

if(FALCON_ENABLE_HTTP)
    target_compile_definitions(falcon_protocols_tests PRIVATE FALCON_ENABLE_HTTP)
endif()

if(FALCON_ENABLE_FTP)
    target_compile_definitions(falcon_protocols_tests PRIVATE FALCON_ENABLE_FTP_PLUGIN)
endif()

# Pass OpenSSL availability to test code
if(OpenSSL_FOUND)
    target_compile_definitions(falcon_protocols_tests PRIVATE FALCON_ENABLE_OPENSSL)
elseif(OPENSSL_FOUND)
    target_compile_definitions(falcon_protocols_tests PRIVATE FALCON_ENABLE_OPENSSL)
endif()

# Protocols tests may need storage headers (cloud_url_protocols.hpp)
target_include_directories(falcon_protocols_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libfalcon-storage/include
)

# ============================================================================
# HTTP plugin tests (separate executable, requires CURL)
# ============================================================================
add_executable(falcon_http_tests
    unit/http_handler_test.cpp
)

target_link_libraries(falcon_http_tests
    PRIVATE
        falcon_protocols
        falcon_core
        GTest::gtest_main
)

find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(falcon_http_tests PRIVATE CURL::libcurl)
    target_compile_definitions(falcon_http_tests PRIVATE FALCON_TEST_CURL_AVAILABLE)
else()
    message(WARNING "libcurl not found, HTTP tests may not work properly")
endif()

add_test_compile_options(falcon_http_tests)
add_test_includes(falcon_http_tests)

if(FALCON_ENABLE_HTTP)
    target_compile_definitions(falcon_http_tests PRIVATE FALCON_ENABLE_HTTP)
endif()

# Pass OpenSSL availability to test code
if(OpenSSL_FOUND)
    target_compile_definitions(falcon_http_tests PRIVATE FALCON_ENABLE_OPENSSL)
elseif(OPENSSL_FOUND)
    target_compile_definitions(falcon_http_tests PRIVATE FALCON_ENABLE_OPENSSL)
endif()

target_include_directories(falcon_http_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/http
)

# ============================================================================
# FTP plugin tests (conditional)
# ============================================================================
if(FALCON_ENABLE_FTP)
    add_executable(falcon_ftp_tests
        unit/ftp_handler_test.cpp
    )

    target_link_libraries(falcon_ftp_tests
        PRIVATE
            falcon_protocols
            falcon_core
            GTest::gtest_main
    )

    add_test_compile_options(falcon_ftp_tests)
    add_test_includes(falcon_ftp_tests)
    target_compile_definitions(falcon_ftp_tests PRIVATE FALCON_ENABLE_FTP_PLUGIN)
endif()

# ============================================================================
# Integration tests
# ============================================================================
add_executable(falcon_integration_tests
    integration/download_integration_test.cpp
)

target_link_libraries(falcon_integration_tests
    PRIVATE
        falcon_protocols
        falcon_core
        GTest::gtest_main
)

add_test_compile_options(falcon_integration_tests)
add_test_includes(falcon_integration_tests)

if(FALCON_ENABLE_HTTP)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_ENABLE_HTTP)
endif()

if(FALCON_ENABLE_FTP)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_ENABLE_FTP_PLUGIN)
endif()

if(CURL_FOUND)
    target_link_libraries(falcon_integration_tests PRIVATE CURL::libcurl)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_TEST_CURL_AVAILABLE)
endif()

# Pass OpenSSL availability to test code
if(OpenSSL_FOUND)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_ENABLE_OPENSSL)
elseif(OPENSSL_FOUND)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_ENABLE_OPENSSL)
endif()

# ============================================================================
# Register with CTest
# ============================================================================
include(GoogleTest)
gtest_discover_tests(falcon_protocols_tests)
gtest_discover_tests(falcon_http_tests)

if(TARGET falcon_ftp_tests)
    gtest_discover_tests(falcon_ftp_tests)
endif()

gtest_discover_tests(falcon_integration_tests)
