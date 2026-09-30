# Falcon Desktop 测试
#
# - backend tests：纯 C++，不依赖 Qt。覆盖 IDownloadBackend 两个实现：
#   - DaemonRpcBackend × 真实 JsonRpcServer 回环
#   - InProcessBackend 基础行为
# - url tests：UrlDetector 回归（依赖 Qt6::Core 的 QString/QRegularExpression；
#   网盘识别规则已迁移到 drives 层 CloudLinkDetector，这里钉住 UI 侧行为）
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
    trash_store_test.cpp
    search_engine_catalog_test.cpp
    ../src/services/download_backend.cpp
    ../src/services/trash_store.cpp
    ../src/services/search_engine_catalog.cpp
)

# 测试 harness 直接使用真实 JsonRpcServer：其符号在 falcon_daemon_rpc 中
# （含客户端库同源文件，静态库按需拉入成员，无重复定义冲突）
target_link_libraries(falcon_desktop_backend_tests
    PRIVATE
        falcon_daemon_rpc
        falcon_daemon_rpc_client
        Falcon::drives
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

# UrlDetector 测试：网盘识别规则唯一事实源在 drives 层 CloudLinkDetector，
# 本 target 钉住迁移后的 UI 侧行为（识别/优先级/文件名格式/展示名目录）。
add_executable(falcon_desktop_url_tests
    url_detector_test.cpp
    ../src/utils/url_detector.cpp
)

target_link_libraries(falcon_desktop_url_tests
    PRIVATE
        Qt6::Core
        Falcon::drives
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_desktop_url_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_url_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_url_tests
    DISCOVERY_TIMEOUT 30
)

# 任务手动排序算法测试（纯 C++，无 Qt 依赖）：拖拽排序的核心语义
# （可见子集换位/未知任务排尾/序列化容错）全部在这层钉住。
add_executable(falcon_desktop_order_tests
    task_order_test.cpp
    ../src/services/task_order.cpp
)

target_link_libraries(falcon_desktop_order_tests
    PRIVATE
        Falcon::core
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_desktop_order_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_order_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_order_tests
    DISCOVERY_TIMEOUT 30
)
