# CMakeLists.txt for Falcon Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_tests
    VERSION 0.1.0
    DESCRIPTION "Falcon library unit and integration tests"
    LANGUAGES CXX
)

# ============================================================================
# 查找依赖库
# ============================================================================

find_package(nlohmann_json QUIET)
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

enable_testing()

# ============================================================================
# 通用编译选项
# ============================================================================
function(add_test_compile_options target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_17)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
        )
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O2>
        )
    endif()
endfunction()

# 通用 include 目录设置
function(add_test_includes target_name)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
endfunction()

# ============================================================================
# Core 单元测试（仅链接 falcon_core）
# ============================================================================
add_executable(falcon_core_tests
    unit/download_task_test.cpp
    unit/task_manager_test.cpp
    unit/task_manager_persistence_test.cpp
    unit/thread_pool_test.cpp
    unit/plugin_manager_test.cpp
    unit/event_dispatcher_test.cpp
    unit/download_engine_test.cpp
    unit/version_test.cpp
    unit/password_manager_test.cpp
    unit/bug_fix_test.cpp
    unit/url_utils_test.cpp
    unit/common_utils_test.cpp
    unit/core_isolation_test.cpp
    unit/download_engine_api_test.cpp
    unit/task_manager_api_test.cpp
)

target_link_libraries(falcon_core_tests
    PRIVATE
        falcon_core
        GTest::gtest_main
)

add_test_compile_options(falcon_core_tests)
add_test_includes(falcon_core_tests)

# ============================================================================
# Protocols 单元测试（链接 falcon_core + falcon_protocols）
# ============================================================================
add_executable(falcon_protocols_tests
    unit/download_engine_v2_test.cpp
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

# 条件编译的协议插件测试
if(FALCON_ENABLE_BITTORRENT)
    target_sources(falcon_protocols_tests PRIVATE
        unit/bittorrent_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_ED2K)
    target_sources(falcon_protocols_tests PRIVATE
        unit/ed2k_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_FLASHGET)
    target_sources(falcon_protocols_tests PRIVATE
        unit/flashget_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_HLS)
    target_sources(falcon_protocols_tests PRIVATE
        unit/hls_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_THUNDER)
    target_sources(falcon_protocols_tests PRIVATE
        unit/thunder_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_QQDL)
    target_sources(falcon_protocols_tests PRIVATE
        unit/qqdl_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_METALINK)
    target_sources(falcon_protocols_tests PRIVATE
        unit/metalink_plugin_test.cpp
    )
endif()

if(FALCON_ENABLE_SFTP)
    target_sources(falcon_protocols_tests PRIVATE
        unit/sftp_plugin_test.cpp
    )
endif()

target_link_libraries(falcon_protocols_tests
    PRIVATE
        falcon_core
        falcon_protocols
        GTest::gtest_main
)

add_test_compile_options(falcon_protocols_tests)
add_test_includes(falcon_protocols_tests)

# Protocols tests may need storage headers (cloud_url_protocols.hpp)
target_include_directories(falcon_protocols_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libfalcon-storage/include
)

# ============================================================================
# HTTP 插件测试（单独可执行文件，需要 CURL）
# ============================================================================
add_executable(falcon_http_tests
    unit/http_handler_test.cpp
)

target_link_libraries(falcon_http_tests
    PRIVATE
        falcon_core
        falcon_protocols
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

# HTTP test needs plugin header paths
target_include_directories(falcon_http_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../libfalcon-protocols/plugins/http
)

# ============================================================================
# FTP 插件测试（条件编译）
# ============================================================================
if(FALCON_ENABLE_FTP)
    add_executable(falcon_ftp_tests
        unit/ftp_handler_test.cpp
    )

    target_link_libraries(falcon_ftp_tests
        PRIVATE
            falcon_core
            falcon_protocols
            GTest::gtest_main
    )

    add_test_compile_options(falcon_ftp_tests)
    add_test_includes(falcon_ftp_tests)
endif()

# ============================================================================
# Storage 单元测试（链接 falcon_core + falcon_storage）
# ============================================================================
if(FALCON_ENABLE_RESOURCE_BROWSER)
    find_package(OpenSSL QUIET)

    add_executable(falcon_storage_tests
        unit/resource_browser_utils_test.cpp
        unit/resource_browsers_test.cpp
        unit/storage_isolation_test.cpp
    )

    target_link_libraries(falcon_storage_tests
        PRIVATE
            falcon_core
            falcon_storage
            GTest::gtest_main
    )

    if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto)
        target_sources(falcon_storage_tests PRIVATE
            unit/resource_cloud_browsers_test.cpp
        )
    endif()

    add_test_compile_options(falcon_storage_tests)
    add_test_includes(falcon_storage_tests)
endif()

# ============================================================================
# Drives 单元测试（链接 falcon_core + falcon_drives）
# ============================================================================
set(FALCON_DRIVES_TEST_SOURCES)

if(FALCON_ENABLE_CLOUD_STORAGE)
    list(APPEND FALCON_DRIVES_TEST_SOURCES
        unit/cloud_storage_test.cpp
    )
endif()

if(FALCON_ENABLE_RESOURCE_SEARCH)
    list(APPEND FALCON_DRIVES_TEST_SOURCES
        unit/search_basic_test.cpp
        unit/resource_search_test.cpp
    )
endif()

if(FALCON_ENABLE_CONFIG_MANAGER)
    list(APPEND FALCON_DRIVES_TEST_SOURCES
        unit/config_manager_test.cpp
    )
endif()

if(FALCON_DRIVES_TEST_SOURCES)
    add_executable(falcon_drives_tests
        ${FALCON_DRIVES_TEST_SOURCES}
        unit/drives_isolation_test.cpp
    )

    target_link_libraries(falcon_drives_tests
        PRIVATE
            falcon_core
            falcon_drives
            GTest::gtest_main
    )

    add_test_compile_options(falcon_drives_tests)
    add_test_includes(falcon_drives_tests)
endif()

# ============================================================================
# 集成测试
# ============================================================================

# 下载集成测试
add_executable(falcon_integration_tests
    integration/download_integration_test.cpp
)

target_link_libraries(falcon_integration_tests
    PRIVATE
        falcon_core
        falcon_protocols
        GTest::gtest_main
)

add_test_compile_options(falcon_integration_tests)
add_test_includes(falcon_integration_tests)

if(CURL_FOUND)
    target_link_libraries(falcon_integration_tests PRIVATE CURL::libcurl)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_TEST_CURL_AVAILABLE)
endif()

# 协议集成测试
# NOTE: all_protocols_integration_test.cpp uses outdated API names
# (loadAllPlugins, startDownload, output_path, etc.) and needs updating
# before it can compile. Disabled until those are fixed.
# if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/integration/all_protocols_integration_test.cpp")
#     add_executable(falcon_all_protocols_integration_tests ...)
# endif()

# 搜索集成测试
# NOTE: search_integration_test.cpp has pre-existing compilation errors.
# Disabled until fixed.
# if(FALCON_ENABLE_RESOURCE_SEARCH AND EXISTS "...")
#     add_executable(falcon_search_integration_tests ...)
# endif()

if(0)
    add_executable(falcon_search_integration_tests
        integration/search_integration_test.cpp
    )

    target_link_libraries(falcon_search_integration_tests
        PRIVATE
            falcon_core
            falcon_drives
            GTest::gtest_main
    )

    add_test_compile_options(falcon_search_integration_tests)
    add_test_includes(falcon_search_integration_tests)
endif()

# ============================================================================
# 注册测试到 CTest
# ============================================================================
include(GoogleTest)

gtest_discover_tests(falcon_core_tests)
gtest_discover_tests(falcon_protocols_tests)
gtest_discover_tests(falcon_http_tests)

if(TARGET falcon_ftp_tests)
    gtest_discover_tests(falcon_ftp_tests)
endif()

if(TARGET falcon_storage_tests)
    gtest_discover_tests(falcon_storage_tests)
endif()

if(TARGET falcon_drives_tests)
    gtest_discover_tests(falcon_drives_tests)
endif()

gtest_discover_tests(falcon_integration_tests)

# ============================================================================
# Library Split 架构约束测试（链接所有库，验证兼容性）
# ============================================================================
add_executable(falcon_split_tests
    unit/library_split_test.cpp
)

target_link_libraries(falcon_split_tests
    PRIVATE
        falcon_core
        falcon_protocols
        falcon_storage
        falcon_drives
        GTest::gtest_main
)

add_test_compile_options(falcon_split_tests)
add_test_includes(falcon_split_tests)

gtest_discover_tests(falcon_split_tests)

if(TARGET falcon_all_protocols_integration_tests)
    gtest_discover_tests(falcon_all_protocols_integration_tests)
endif()

if(TARGET falcon_search_integration_tests)
    gtest_discover_tests(falcon_search_integration_tests)
endif()
