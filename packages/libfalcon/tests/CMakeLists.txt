cmake_minimum_required(VERSION 3.15)

project(falcon_tests
    VERSION 0.1.0
    DESCRIPTION "Falcon library unit tests"
    LANGUAGES CXX
)

# ============================================================================
# Find GoogleTest
# ============================================================================

find_package(GTest QUIET)

if(NOT GTest_FOUND)
    # Try to fetch GoogleTest if not found
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

enable_testing()

# ============================================================================
# Unit tests
# ============================================================================

add_executable(falcon_unit_tests
    unit/download_task_test.cpp
    unit/task_manager_test.cpp
    unit/url_utils_test.cpp
    unit/thread_pool_test.cpp
)

target_link_libraries(falcon_unit_tests
    PRIVATE
        falcon
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_unit_tests PRIVATE cxx_std_17)

# Register tests with CTest
include(GoogleTest)
gtest_discover_tests(falcon_unit_tests)
