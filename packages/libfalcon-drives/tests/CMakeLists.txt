# CMakeLists.txt for libfalcon-drives Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_drives_tests_project
    VERSION 0.1.0
    DESCRIPTION "Falcon drives library tests"
    LANGUAGES CXX
)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping libfalcon-drives tests")
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
# Drives unit tests
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

    include(GoogleTest)
    gtest_discover_tests(falcon_drives_tests)
endif()
