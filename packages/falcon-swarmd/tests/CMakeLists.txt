# ============================================================================
# falcon-swarmd Tests
#
# 两个目标：
#   falcon_swarm_unit_tests     —— 线协议对拍（canonical/RFC 8032 向量）+
#                                  限频器（虚拟时钟）+ 服务器状态（注入 now）
#   falcon_swarm_loopback_tests —— 真 socket 回环：HTTP/WS/错误路径/限频
#
# 两者都链 falcon_swarm_server（SwarmServerState/SwarmRateLimiter 编在其中，
# PUBLIC 传递 falcon_swarm_common 与 falcon_ws_protocol 的头与库）。
# ============================================================================

cmake_minimum_required(VERSION 3.15)

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping falcon-swarmd tests")
    return()
endif()

find_package(Threads REQUIRED)

include(GoogleTest)
enable_testing()

add_executable(falcon_swarm_unit_tests
    swarm_unit_tests.cpp
)

target_link_libraries(falcon_swarm_unit_tests
    PRIVATE
        falcon_swarm_server
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_swarm_unit_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_swarm_unit_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_swarm_unit_tests
    DISCOVERY_TIMEOUT 30
)

add_executable(falcon_swarm_loopback_tests
    swarm_server_loopback_test.cpp
    swarm_server_harness.hpp
)

target_link_libraries(falcon_swarm_loopback_tests
    PRIVATE
        falcon_swarm_server
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_swarm_loopback_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_swarm_loopback_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_swarm_loopback_tests
    DISCOVERY_TIMEOUT 30
)

# ============================================================================
# SwarmClient × 回环 server（client 库在位才编——CURL 缺席时跳过）
# ============================================================================
if(TARGET falcon_swarm_client)
    add_executable(falcon_swarm_client_tests
        swarm_client_test.cpp
        swarm_server_harness.hpp
    )

    target_link_libraries(falcon_swarm_client_tests
        PRIVATE
            falcon_swarm_client
            falcon_swarm_server
            GTest::gtest
            GTest::gtest_main
            Threads::Threads
    )

    target_include_directories(falcon_swarm_client_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_swarm_client_tests PRIVATE cxx_std_17)

    gtest_discover_tests(falcon_swarm_client_tests
        DISCOVERY_TIMEOUT 30
    )
endif()

# ============================================================================
# 真二进制 e2e（fork+execv，POSIX-only；Windows 编译面由其余三目标覆盖）
# ============================================================================
if(TARGET falcon_swarm_client AND NOT WIN32)
    add_executable(falcon_swarm_e2e_tests
        swarm_e2e_test.cpp
        swarm_server_harness.hpp
    )

    target_link_libraries(falcon_swarm_e2e_tests
        PRIVATE
            falcon_swarm_client
            falcon_swarm_server
            GTest::gtest
            GTest::gtest_main
            Threads::Threads
    )

    target_include_directories(falcon_swarm_e2e_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_swarm_e2e_tests PRIVATE cxx_std_17)

    target_compile_definitions(falcon_swarm_e2e_tests PRIVATE
        FALCON_SWARMD_BIN="$<TARGET_FILE:falcon-swarmd>"
    )

    gtest_discover_tests(falcon_swarm_e2e_tests
        DISCOVERY_TIMEOUT 30
    )
endif()
