# CMakeLists.txt for libfalcon-storage Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_storage_tests_project
    VERSION 0.1.0
    DESCRIPTION "Falcon storage library tests"
    LANGUAGES CXX
)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping libfalcon-storage tests")
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
# Storage unit tests
# ============================================================================
if(FALCON_ENABLE_RESOURCE_BROWSER)
    find_package(OpenSSL QUIET)

    add_executable(falcon_storage_tests
        unit/resource_browser_utils_test.cpp
        unit/resource_browsers_test.cpp
        unit/resource_browser_edges_test.cpp
        unit/webdav_browser_mock_test.cpp
        unit/ftp_browser_mock_test.cpp
        unit/storage_presets_test.cpp
        unit/storage_isolation_test.cpp
    )
    if(FALCON_ENABLE_STORAGE_S3)
        target_sources(falcon_storage_tests PRIVATE unit/s3_browser_mock_test.cpp)
    endif()

    target_link_libraries(falcon_storage_tests
        PRIVATE
            falcon_core
            falcon_storage
            GTest::gtest_main
    )

    # per-provider 宏与库侧同值（§5.2：provider option 维度透传给测试
    # TU；OpenSSL 维度由下方与库同款的 OPENSSL_FOUND 条件承载，
    # FALCON_ENABLE_CRYPTO_STORAGE_BROWSERS 保持对测试 TU 不可见）
    foreach(_provider S3 OSS COS KODO UPYUN)
        if(FALCON_ENABLE_STORAGE_${_provider})
            target_compile_definitions(falcon_storage_tests
                PRIVATE FALCON_ENABLE_STORAGE_${_provider})
        endif()
    endforeach()

    if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto)
        if(FALCON_ENABLE_STORAGE_S3)
            target_sources(falcon_storage_tests PRIVATE unit/s3_browser_auth_test.cpp)
        endif()
        if(FALCON_ENABLE_STORAGE_OSS)
            target_sources(falcon_storage_tests PRIVATE unit/oss_browser_mock_test.cpp)
        endif()
        if(FALCON_ENABLE_STORAGE_COS)
            target_sources(falcon_storage_tests PRIVATE unit/cos_browser_mock_test.cpp)
        endif()
        if(FALCON_ENABLE_STORAGE_KODO)
            target_sources(falcon_storage_tests PRIVATE unit/kodo_browser_mock_test.cpp)
        endif()
        if(FALCON_ENABLE_STORAGE_UPYUN)
            target_sources(falcon_storage_tests PRIVATE unit/upyun_browser_mock_test.cpp)
        endif()
        if(FALCON_ENABLE_STORAGE_OSS
                OR FALCON_ENABLE_STORAGE_COS
                OR FALCON_ENABLE_STORAGE_KODO
                OR FALCON_ENABLE_STORAGE_UPYUN)
            target_sources(falcon_storage_tests PRIVATE unit/resource_cloud_browsers_test.cpp)
        endif()
        if(FALCON_ENABLE_STORAGE_S3
                OR FALCON_ENABLE_STORAGE_OSS
                OR FALCON_ENABLE_STORAGE_COS
                OR FALCON_ENABLE_STORAGE_KODO
                OR FALCON_ENABLE_STORAGE_UPYUN)
            target_sources(falcon_storage_tests PRIVATE unit/storage_injection_test.cpp)
        endif()
    endif()

    add_test_compile_options(falcon_storage_tests)
    add_test_includes(falcon_storage_tests)

    include(GoogleTest)
    gtest_discover_tests(falcon_storage_tests)
endif()
