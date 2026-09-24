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
