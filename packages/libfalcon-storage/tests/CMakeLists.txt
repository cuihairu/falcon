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

    include(GoogleTest)
    gtest_discover_tests(falcon_storage_tests)
endif()
