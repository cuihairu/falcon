# Falcon Daemon Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_daemon_tests
    VERSION 0.1.0
    DESCRIPTION "Falcon daemon tests"
    LANGUAGES CXX
)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping falcon-daemon tests")
    return()
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
        task_storage_listener_test.cpp
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

    # JSON-RPC × TaskStorage 集成：历史任务经 RPC 可见、删除联动、批量控制落库
    find_package(Threads REQUIRED)
    add_executable(falcon_daemon_rpc_storage_tests
        json_rpc_storage_test.cpp
    )

    target_link_libraries(falcon_daemon_rpc_storage_tests
        PRIVATE
            falcon_daemon_rpc
            GTest::gtest
            GTest::gtest_main
            Threads::Threads
    )

    target_include_directories(falcon_daemon_rpc_storage_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_daemon_rpc_storage_tests PRIVATE cxx_std_17)

    gtest_discover_tests(falcon_daemon_rpc_storage_tests
        DISCOVERY_TIMEOUT 30
    )
endif()

# JSON-RPC 客户端 × 真实服务端回环集成
add_executable(falcon_daemon_rpc_client_tests
    json_rpc_client_test.cpp
)

target_link_libraries(falcon_daemon_rpc_client_tests
    PRIVATE
        falcon_daemon_rpc
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_daemon_rpc_client_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_daemon_rpc_client_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_daemon_rpc_client_tests
    DISCOVERY_TIMEOUT 30
)

# ============================================================================
# Additional coverage tests (appended; targets above are unchanged)
# ============================================================================

find_package(Threads REQUIRED)

# Extended JSON-RPC server coverage: HTTP layer + every RPC method.
add_executable(falcon_daemon_rpc_coverage_tests
    json_rpc_server_coverage_test.cpp
)

target_link_libraries(falcon_daemon_rpc_coverage_tests
    PRIVATE
        falcon_daemon_rpc
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_daemon_rpc_coverage_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_daemon_rpc_coverage_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_daemon_rpc_coverage_tests
    DISCOVERY_TIMEOUT 30
)

if(NOT WIN32)
    # DaemonManager lifecycle: run loop, pid files, daemonize() and signal
    # handling in forked children (POSIX only).
    add_executable(falcon_daemon_lifecycle_tests
        daemon_lifecycle_test.cpp
    )

    target_link_libraries(falcon_daemon_lifecycle_tests
        PRIVATE
            falcon_daemon_rpc
            GTest::gtest
            GTest::gtest_main
            Threads::Threads
    )

    target_include_directories(falcon_daemon_lifecycle_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_daemon_lifecycle_tests PRIVATE cxx_std_17)

    gtest_discover_tests(falcon_daemon_lifecycle_tests
        DISCOVERY_TIMEOUT 30
    )

    # main.cpp integration: run the real falcon-daemon binary with various
    # argument sets and assert exit codes / output (POSIX only).
    add_executable(falcon_daemon_main_tests
        main_integration_test.cpp
    )

    target_link_libraries(falcon_daemon_main_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            Threads::Threads
    )

    target_compile_features(falcon_daemon_main_tests PRIVATE cxx_std_17)

    target_compile_definitions(falcon_daemon_main_tests PRIVATE
        FALCON_DAEMON_BIN="$<TARGET_FILE:falcon-daemon>"
    )

    gtest_discover_tests(falcon_daemon_main_tests
        DISCOVERY_TIMEOUT 30
    )
endif()
