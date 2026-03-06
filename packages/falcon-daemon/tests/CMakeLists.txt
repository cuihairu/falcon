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

# RPC Tests
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

# Task Storage Tests (requires SQLite3)
if(SQLite3_FOUND AND TARGET falcon_daemon_storage)
    add_executable(falcon_daemon_storage_tests
        task_storage_test.cpp
    )

    target_link_libraries(falcon_daemon_storage_tests
        PRIVATE
            falcon_daemon_storage
            GTest::gtest
            GTest::gtest_main
    )

    target_include_directories(falcon_daemon_storage_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_daemon_storage_tests PRIVATE cxx_std_17)
    target_compile_definitions(falcon_daemon_storage_tests PRIVATE FALCON_HAS_SQLITE3)

    gtest_discover_tests(falcon_daemon_storage_tests)
endif()
