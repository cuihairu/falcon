# Falcon Desktop backend-layer tests（纯 C++，不依赖 Qt）
#
# 覆盖 IDownloadBackend 两个实现：
# - DaemonRpcBackend × 真实 JsonRpcServer 回环
# - InProcessBackend 基础行为
#
# Qt 服务层（DownloadService）随桌面应用在 CI 的 Qt6 job 中编译验证。

find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping falcon-desktop backend tests")
    return()
endif()

find_package(Threads REQUIRED)

enable_testing()

add_executable(falcon_desktop_backend_tests
    download_backend_test.cpp
    ../src/services/download_backend.cpp
)

# 测试 harness 直接使用真实 JsonRpcServer：其符号在 falcon_daemon_rpc 中
# （含客户端库同源文件，静态库按需拉入成员，无重复定义冲突）
target_link_libraries(falcon_desktop_backend_tests
    PRIVATE
        falcon_daemon_rpc
        falcon_daemon_rpc_client
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_desktop_backend_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_backend_tests PRIVATE cxx_std_17)

include(GoogleTest)
gtest_discover_tests(falcon_desktop_backend_tests
    DISCOVERY_TIMEOUT 30
)
