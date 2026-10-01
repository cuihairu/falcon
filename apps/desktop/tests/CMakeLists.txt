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

# 任务两视图可见性判定测试（纯 C++ 头文件内联函数，无 Qt 依赖）：
# Failed 归入下载中视图的语义（失败任务可重试，不得凭空消失）在这层钉住。
add_executable(falcon_desktop_visibility_tests
    task_visibility_test.cpp
)

target_link_libraries(falcon_desktop_visibility_tests
    PRIVATE
        Falcon::core
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_desktop_visibility_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_visibility_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_visibility_tests
    DISCOVERY_TIMEOUT 30
)

# 检查更新版本比较测试（静态纯函数，零网络）：check_for_updates() 本体
# 访问真实 GitHub API（外网红线），本 target 只钉可离线测试面
# is_newer_tag_version 全语义 + current_version 单一事实源。
add_executable(falcon_desktop_update_tests
    update_checker_test.cpp
    ../src/services/update_checker.cpp
)

target_link_libraries(falcon_desktop_update_tests
    PRIVATE
        Qt6::Core
        Qt6::Network
        Falcon::core
        GTest::gtest
        GTest::gtest_main
        Threads::Threads
)

target_include_directories(falcon_desktop_update_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_update_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_update_tests
    DISCOVERY_TIMEOUT 30
)

# IPC 服务器回环测试（真实 QTcpServer/QTcpSocket 全链）：自动化 09-19 批次
# 的离屏 + curl 手工冒烟，并把「/v1/add 先应答再派发」修复钉成回归用例。
# 自定义 main（QCoreApplication 事件循环驱动，无 gtest_main）。
add_executable(falcon_desktop_ipc_tests
    ipc_server_test.cpp
    ../src/ipc/http_server.cpp
)

target_link_libraries(falcon_desktop_ipc_tests
    PRIVATE
        Qt6::Core
        Qt6::Network
        GTest::gtest
        Threads::Threads
)

target_include_directories(falcon_desktop_ipc_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_ipc_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_ipc_tests
    DISCOVERY_TIMEOUT 30
)

# ClipboardMonitor 测试（QApplication 级测试基建首发）：QClipboard 需要平台层
# 支撑，验证以 xvfb-run（xcb）为准；无显示环境 main 内自动回落 offscreen
# （进程内剪贴板往返 + changed 信号在 offscreen 均可用——探针实证）。
# 自定义 main（QApplication + 平台选择，无 gtest_main）。
add_executable(falcon_desktop_clipboard_tests
    clipboard_monitor_test.cpp
    ../src/utils/clipboard_monitor.cpp
    ../src/utils/url_detector.cpp
)

target_link_libraries(falcon_desktop_clipboard_tests
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Widgets
        Falcon::drives
        GTest::gtest
        Threads::Threads
)

target_include_directories(falcon_desktop_clipboard_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_desktop_clipboard_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_clipboard_tests
    DISCOVERY_TIMEOUT 30
)

# StorageService 测试（QCoreApplication 级）：网络成功路径全走回环
# MockHttpServer（storage 包共享基建），失败路径 127.0.0.1:1 回环连接拒绝
# ——不出网。自定义 main（QSettings 隔离到临时目录 + QCoreApplication）。
add_executable(falcon_desktop_storage_tests
    storage_service_test.cpp
    ../src/services/storage_service.cpp
)

target_link_libraries(falcon_desktop_storage_tests
    PRIVATE
        Qt6::Core
        Qt6::Concurrent
        Falcon::storage
        GTest::gtest
        Threads::Threads
)

target_include_directories(falcon_desktop_storage_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../packages/libfalcon-storage/tests/unit
)

target_compile_features(falcon_desktop_storage_tests PRIVATE cxx_std_17)

gtest_discover_tests(falcon_desktop_storage_tests
    DISCOVERY_TIMEOUT 30
)
