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

# RPC Tests（含 WebSocket 事件流订阅 + MCP 端点 + MCP stdio 薄壳）
add_executable(falcon_daemon_rpc_tests
    json_rpc_server_test.cpp
    websocket_test.cpp
    mcp_server_test.cpp
)

# MCP stdio 薄壳（桥层 fake 单测 + 真 curl 回环 e2e）：
# 薄壳库仅在 CURL 可用时存在
if(TARGET falcon_daemon_mcp_stdio)
    target_sources(falcon_daemon_rpc_tests PRIVATE mcp_stdio_test.cpp)
    target_link_libraries(falcon_daemon_rpc_tests PRIVATE falcon_daemon_mcp_stdio)
endif()

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

# daemon.json 配置文件加载（config.cpp 直接编进测试目标）
add_executable(falcon_daemon_config_tests
    config_test.cpp
    ../src/daemon/config.cpp
)

target_link_libraries(falcon_daemon_config_tests
    PRIVATE
        falcon_daemon_rpc
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_daemon_config_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_daemon_config_tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(falcon_daemon_rpc_tests)
gtest_discover_tests(falcon_daemon_config_tests)

# P2SP 公告组件（§16.4）：swarm_announcer.cpp 直接编进测试目标（真实
# 网关经 SwarmAnnouncerGateway 接缝替换为 fake，无需 CURL/swarmd client）
add_executable(falcon_daemon_swarm_announcer_tests
    swarm_announcer_test.cpp
    ../src/daemon/swarm_announcer.cpp
)

target_link_libraries(falcon_daemon_swarm_announcer_tests
    PRIVATE
        falcon_daemon_rpc
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_daemon_swarm_announcer_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_daemon_swarm_announcer_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_daemon_swarm_announcer_tests
    DISCOVERY_TIMEOUT 30
)

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

# JSON-RPC 客户端 × 真实服务端回环集成（HTTP + WebSocket 事件流）
add_executable(falcon_daemon_rpc_client_tests
    json_rpc_client_test.cpp
    aria2_snapshots_test.cpp
    websocket_rpc_client_test.cpp
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

    # 直连 task db 预置恢复记录的用例需要 sqlite3（缺失时该用例跳过）
    if(SQLite3_FOUND)
        target_link_libraries(falcon_daemon_main_tests PRIVATE SQLite::SQLite3)
        target_compile_definitions(falcon_daemon_main_tests PRIVATE FALCON_HAS_SQLITE3)
    endif()

    target_compile_features(falcon_daemon_main_tests PRIVATE cxx_std_17)

    target_compile_definitions(falcon_daemon_main_tests PRIVATE
        FALCON_DAEMON_BIN="$<TARGET_FILE:falcon-daemon>"
    )

    gtest_discover_tests(falcon_daemon_main_tests
        DISCOVERY_TIMEOUT 30
    )

    # Swarm daemon e2e（§16.6 铁律真二进制验收，POSIX-only）：本 target 在此
    # 无条件注册（daemon tests 子目录先于 swarmd 子目录处理，届时
    # falcon_swarm_client 尚不存在）；链接与 FALCON_SWARMD_BIN/FALCON_DAEMON_BIN/
    # FALCON_SWARM_E2E_ENABLED 定义由根 CMakeLists 在 swarmd 之后事后补注入
    # （与 FALCON_HAS_SWARM 补链同款形态）。宏缺席时编译为单 skip 占位。
    add_executable(falcon_daemon_swarm_e2e_tests
        swarm_daemon_e2e_test.cpp
    )

    target_link_libraries(falcon_daemon_swarm_e2e_tests
        PRIVATE
            GTest::gtest
            GTest::gtest_main
            Threads::Threads
    )

    target_compile_features(falcon_daemon_swarm_e2e_tests PRIVATE cxx_std_17)

    gtest_discover_tests(falcon_daemon_swarm_e2e_tests
        DISCOVERY_TIMEOUT 30
    )
endif()
