# CMakeLists.txt for libfalcon-core Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_core_tests_project
    VERSION 0.1.0
    DESCRIPTION "Falcon core library tests"
    LANGUAGES CXX
)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping libfalcon-core tests")
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
# Core unit tests (links only falcon_core)
# ============================================================================
add_executable(falcon_core_tests
    unit/download_task_test.cpp
    unit/task_manager_test.cpp
    unit/task_manager_persistence_test.cpp
    unit/thread_pool_test.cpp
    unit/plugin_manager_test.cpp
    unit/event_dispatcher_test.cpp
    unit/download_engine_test.cpp
    unit/download_engine_api_test.cpp
    unit/task_manager_api_test.cpp
    unit/version_test.cpp
    unit/password_manager_test.cpp
    unit/bug_fix_test.cpp
    unit/url_utils_test.cpp
    unit/common_utils_test.cpp
    unit/core_isolation_test.cpp
)

target_link_libraries(falcon_core_tests
    PRIVATE
        falcon_core
        GTest::gtest_main
)

add_test_compile_options(falcon_core_tests)
add_test_includes(falcon_core_tests)

# ============================================================================
# Library Split architecture constraint test (links all libraries)
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

# ============================================================================
# Register with CTest
# ============================================================================
include(GoogleTest)
gtest_discover_tests(falcon_core_tests)
gtest_discover_tests(falcon_split_tests)
