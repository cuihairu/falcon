cmake_minimum_required(VERSION 3.15)

project(falcon-cli-tests)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found, fetching from GitHub...")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    # For Windows: Prevent overriding the parent project's compiler/linker settings
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# ============================================================================
# Unit Tests
# ============================================================================

add_executable(falcon_cli_tests
    unit/arg_parser_test.cpp
)

target_link_libraries(falcon_cli_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_cli_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_cli_tests PRIVATE cxx_std_17)

# Register with CTest
include(GoogleTest)
gtest_discover_tests(falcon_cli_tests)
