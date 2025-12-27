# Falcon Daemon Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_daemon_tests
    VERSION 0.1.0
    DESCRIPTION "Falcon daemon tests"
    LANGUAGES CXX
)

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

add_executable(falcon_daemon_rpc_tests
    json_rpc_server_test.cpp
)

target_link_libraries(falcon_daemon_rpc_tests
    PRIVATE
        falcon_daemon_rpc
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_daemon_rpc_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_daemon_rpc_tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(falcon_daemon_rpc_tests)
