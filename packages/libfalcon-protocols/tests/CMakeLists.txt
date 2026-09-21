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
    unit/http_commands_coverage_test.cpp
    unit/event_poll_poll_test.cpp
    unit/download_engine_v2_run_test.cpp
    unit/download_engine_v2_retry_test.cpp
    unit/download_engine_v2_speed_limit_test.cpp
    unit/download_engine_v2_resume_test.cpp
    unit/download_engine_v2_pause_test.cpp
    unit/download_engine_v2_multi_source_test.cpp
    unit/http_commands_chunked_test.cpp
    unit/http_commands_redirect_test.cpp
    unit/http_commands_tls_test.cpp
    unit/http_commands_ipv6_test.cpp
    unit/http_commands_injection_test.cpp
    unit/http_commands_proxy_test.cpp
    unit/http_commands_edges_test.cpp
    unit/coverage_supplement_test.cpp
)

# download_engine_v2_test：pipe 已用回环 TCP 在 Windows 等价实现，三平台统一编译
target_sources(falcon_protocols_tests PRIVATE unit/download_engine_v2_test.cpp)

# Conditional plugin tests
if(FALCON_ENABLE_BITTORRENT)
    target_sources(falcon_protocols_tests PRIVATE unit/bittorrent_plugin_test.cpp)
    target_sources(falcon_protocols_tests PRIVATE unit/bittorrent_parse_test.cpp)
    target_sources(falcon_protocols_tests PRIVATE unit/dht_node_test.cpp)
    target_sources(falcon_protocols_tests PRIVATE unit/pex_protocol_test.cpp)
    target_sources(falcon_protocols_tests PRIVATE unit/bencode_edge_test.cpp)
    target_sources(falcon_protocols_tests PRIVATE unit/seed_policy_test.cpp)
    # libtorrent session 数据面做种 e2e（非 libtorrent 构建下编译为 skip 占位）
    target_sources(falcon_protocols_tests PRIVATE unit/bittorrent_seeding_test.cpp)
    # 插件符号（BencodeValue、create_bittorrent_handler 等）在插件静态库中
    target_link_libraries(falcon_protocols_tests PRIVATE falcon_plugin_bittorrent)
    # 与库一致的编译定义，供配置相关的测试条件使用
    target_compile_definitions(falcon_protocols_tests PRIVATE FALCON_ENABLE_BT_PLUGIN)
    # dht_node.hpp 是插件私有头（不在公共 include 目录），直接引用插件源目录
    target_include_directories(falcon_protocols_tests
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/bittorrent)
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

# Metalink 解析单测(handler e2e 挂到下方 falcon_http_tests 定义之后)
if(FALCON_ENABLE_METALINK)
    target_sources(falcon_protocols_tests PRIVATE
        unit/mini_xml_parser_test.cpp
        unit/metalink_parse_test.cpp
    )
    target_compile_definitions(falcon_protocols_tests PRIVATE FALCON_ENABLE_METALINK_PLUGIN)
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
    unit/http_handler_edges_test.cpp
    unit/global_speed_limit_test.cpp
    unit/range_integrity_test.cpp
    unit/v2_http_adapter_test.cpp
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

# Metalink handler e2e:复用共享可编程 HTTP 服务器基建与 HttpHandler
if(FALCON_ENABLE_METALINK)
    target_sources(falcon_http_tests PRIVATE unit/metalink_handler_test.cpp)
    target_include_directories(falcon_http_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../plugins/metalink
    )
endif()

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
