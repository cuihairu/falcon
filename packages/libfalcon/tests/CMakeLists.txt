# CMakeLists.txt for Falcon Tests - Enhanced Version
cmake_minimum_required(VERSION 3.15)

project(falcon_tests
    VERSION 0.1.0
    DESCRIPTION "Falcon library unit and integration tests"
    LANGUAGES CXX
)

# ============================================================================
# 查找依赖库
# ============================================================================

# 查找 nlohmann/json
find_package(nlohmann_json QUIET)

# 查找 GoogleTest
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    # 如果未找到，尝试通过 FetchContent 获取
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.14.0
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

# 启用测试
enable_testing()

# ============================================================================
# 编译选项
# ============================================================================

# 测试编译选项
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        $<$<CONFIG:Debug>:-g -O0>
        $<$<CONFIG:Release>:-O2>
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter  # 测试代码中的未使用参数很常见
    )
endif()

# ============================================================================
# 核心单元测试
# ============================================================================

add_executable(falcon_core_tests
    # 核心模块测试
    unit/download_task_test.cpp
    unit/task_manager_test.cpp
    unit/url_utils_test.cpp
    unit/thread_pool_test.cpp
)

target_link_libraries(falcon_core_tests
    PRIVATE
        falcon
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_core_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_core_tests PRIVATE cxx_std_17)

# ============================================================================
# 协议插件单元测试
# ============================================================================

# HTTP 插件测试（始终编译）
add_executable(falcon_http_tests
    unit/http_plugin_test.cpp
)

target_link_libraries(falcon_http_tests
    PRIVATE
        falcon
        GTest::gtest
        GTest::gtest_main
)

# 查找并链接 libcurl
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(falcon_http_tests PRIVATE CURL::libcurl)
    target_compile_definitions(falcon_http_tests PRIVATE FALCON_TEST_CURL_AVAILABLE)
else()
    message(WARNING "libcurl not found, HTTP tests may not work properly")
endif()

target_include_directories(falcon_http_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_http_tests PRIVATE cxx_std_17)

# 私有协议插件测试集合
set(PRIVATE_PROTOCOL_TEST_SOURCES)
set(PRIVATE_PROTOCOL_TEST_TARGETS)

# ============================================================================
# 资源搜索和网盘测试
# ============================================================================

# 基础搜索测试（不依赖JSON）
add_executable(falcon_search_basic_tests
    unit/search_basic_test.cpp
)

target_link_libraries(falcon_search_basic_tests
    PRIVATE
        falcon
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_search_basic_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_search_basic_tests PRIVATE cxx_std_17)

# 资源搜索测试（需要nlohmann/json）
if(nlohmann_json_FOUND)
    add_executable(falcon_resource_search_tests
        unit/resource_search_test.cpp
    )

    target_link_libraries(falcon_resource_search_tests
        PRIVATE
            falcon
            GTest::gtest
            GTest::gtest_main
    )

    target_include_directories(falcon_resource_search_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_resource_search_tests PRIVATE cxx_std_17)

    if(nlohmann_json_FOUND)
        target_link_libraries(falcon_resource_search_tests PRIVATE nlohmann_json::nlohmann_json)
    endif()
endif()

# 网盘存储测试
add_executable(falcon_cloud_storage_tests
    unit/cloud_storage_test.cpp
)

target_link_libraries(falcon_cloud_storage_tests
    PRIVATE
        falcon
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_cloud_storage_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_cloud_storage_tests PRIVATE cxx_std_17)

# 条件编译私有协议测试
if(FALCON_ENABLE_THUNDER)
    list(APPEND PRIVATE_PROTOCOL_TEST_SOURCES unit/thunder_plugin_test.cpp)
endif()

if(FALCON_ENABLE_QQDL)
    list(APPEND PRIVATE_PROTOCOL_TEST_SOURCES unit/qqdl_plugin_test.cpp)
endif()

if(FALCON_ENABLE_FLASHGET)
    list(APPEND PRIVATE_PROTOCOL_TEST_SOURCES unit/flashget_plugin_test.cpp)
endif()

if(FALCON_ENABLE_ED2K)
    list(APPEND PRIVATE_PROTOCOL_TEST_SOURCES unit/ed2k_plugin_test.cpp)
endif()

if(FALCON_ENABLE_HLS)
    list(APPEND PRIVATE_PROTOCOL_TEST_SOURCES unit/hls_plugin_test.cpp)
endif()

if(FALCON_ENABLE_BITTORRENT)
    list(APPEND PRIVATE_PROTOCOL_TEST_SOURCES unit/bittorrent_plugin_test.cpp)

    # 查找 libtorrent
    find_package(PkgConfig QUIET)
    pkg_check_modules(LIBTORRENT-Rasterbar QUIET libtorrent-rasterbar)
    if(LIBTORRENT-Rasterbar_FOUND)
        set_target_properties(falcon_private_protocol_tests PROPERTIES
            INCLUDE_DIRECTORIES ${LIBTORRENT-Rasterbar_INCLUDE_DIRS}
            COMPILE_DEFINITIONS FALCON_USE_LIBTORRENT
        )
    endif()
endif()

# 如果有私有协议测试，创建测试可执行文件
if(PRIVATE_PROTOCOL_TEST_SOURCES)
    add_executable(falcon_private_protocol_tests
        ${PRIVATE_PROTOCOL_TEST_SOURCES}
    )

    target_link_libraries(falcon_private_protocol_tests
        PRIVATE
            falcon
            GTest::gtest
            GTest::gtest_main
    )

    target_include_directories(falcon_private_protocol_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_private_protocol_tests PRIVATE cxx_std_17)

    # 如果找到 libtorrent，链接它
    if(LIBTORRENT-Rasterbar_FOUND)
        target_link_libraries(falcon_private_protocol_tests PRIVATE ${LIBTORRENT-Rasterbar_LIBRARIES})
    endif()
endif()

# ============================================================================
# 集成测试
# ============================================================================

add_executable(falcon_integration_tests
    integration/all_protocols_integration_test.cpp
)

target_link_libraries(falcon_integration_tests
    PRIVATE
        falcon
        GTest::gtest
        GTest::gtest_main
)

target_include_directories(falcon_integration_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_compile_features(falcon_integration_tests PRIVATE cxx_std_17)

# ============================================================================
# 性能测试
# ============================================================================

# 如果需要性能测试
option(FALCON_BUILD_PERFORMANCE_TESTS "Build performance tests" OFF)

if(FALCON_BUILD_PERFORMANCE_TESTS)
    add_executable(falcon_performance_tests
        performance/http_download_benchmark.cpp
        performance/protocol_comparison_benchmark.cpp
    )

    target_link_libraries(falcon_performance_tests
        PRIVATE
            falcon
            GTest::gtest
            GTest::gtest_main
            pthread  # 性能测试需要多线程
    )

    target_include_directories(falcon_performance_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_compile_features(falcon_performance_tests PRIVATE cxx_std_17)
endif()

# ============================================================================
# 注册测试到 CTest
# ============================================================================

# 基础测试发现
include(GoogleTest)

# 注册核心测试
gtest_discover_tests(falcon_core_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TEST_PREFIX "core_"
)

# 注册基础搜索测试
gtest_discover_tests(falcon_search_basic_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TEST_PREFIX "search_basic_"
)

# 注册资源搜索测试
if(nlohmann_json_FOUND AND TARGET falcon_resource_search_tests)
    gtest_discover_tests(falcon_resource_search_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TEST_PREFIX "search_"
)
endif()

# 注册网盘存储测试
gtest_discover_tests(falcon_cloud_storage_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TEST_PREFIX "cloud_"
)

# 注册 HTTP 测试
gtest_discover_tests(falcon_http_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TEST_PREFIX "http_"
)

# 注册私有协议测试（如果存在）
if(PRIVATE_PROTOCOL_TEST_SOURCES)
    gtest_discover_tests(falcon_private_protocol_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TEST_PREFIX "private_"
    )
endif()

# 注册集成测试
gtest_discover_tests(falcon_integration_tests
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    TEST_PREFIX "integration_"
)

# 设置测试属性
set_tests_properties(falcon_core_tests
    PROPERTIES
    LABELS "core;unit"
    TIMEOUT 60
)

set_tests_properties(falcon_http_tests
    PROPERTIES
    LABELS "http;plugin;unit"
    TIMEOUT 120
)

if(PRIVATE_PROTOCOL_TEST_SOURCES)
    set_tests_properties(falcon_private_protocol_tests
        PROPERTIES
        LABELS "private;plugin;unit"
        TIMEOUT 60
    )
endif()

set_tests_properties(falcon_integration_tests
    PROPERTIES
    LABELS "integration"
    TIMEOUT 300
)

if(FALCON_BUILD_PERFORMANCE_TESTS)
    gtest_discover_tests(falcon_performance_tests
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        TEST_PREFIX "perf_"
    )

    set_tests_properties(falcon_performance_tests
        PROPERTIES
        LABELS "performance"
        TIMEOUT 600
    )
endif()

# ============================================================================
# 自定义测试目标
# ============================================================================

# 运行单元测试
add_custom_target(run_unit_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --label "unit"
    DEPENDS falcon_core_tests falcon_http_tests falcon_private_protocol_tests
    COMMENT "Running all unit tests"
)

# 运行集成测试
add_custom_target(run_integration_tests
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --label "integration"
    DEPENDS falcon_integration_tests
    COMMENT "Running integration tests"
)

# 运行所有测试
add_custom_target(test
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    COMMENT "Running all tests"
)

# 并行运行测试（如果有多个测试套件）
add_custom_target(test_parallel
    COMMAND ${CMAKE_CTEST_COMMAND} --parallel ${PROCESSOR_COUNT} --output-on-failure
    COMMENT "Running tests in parallel"
)

# 运行性能测试
if(FALCON_BUILD_PERFORMANCE_TESTS)
    add_custom_target(run_performance_tests
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure --label "performance"
        DEPENDS falcon_performance_tests
        COMMENT "Running performance tests"
    )
endif()

# ============================================================================
# 代码覆盖率（Debug 模式）
# ============================================================================

option(FALCON_ENABLE_COVERAGE "Enable code coverage reporting" OFF)

if(FALCON_ENABLE_COVERAGE AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    # 如果启用了覆盖率，添加编译选项
    target_compile_options(falcon_core_tests PRIVATE --coverage)
    target_compile_options(falcon_http_tests PRIVATE --coverage)
    if(PRIVATE_PROTOCOL_TEST_SOURCES)
        target_compile_options(falcon_private_protocol_tests PRIVATE --coverage)
    endif()
    target_compile_options(falcon_integration_tests PRIVATE --coverage)

    # 生成覆盖率报告
    find_program(LCOV_EXECUTABLE lcov)
    find_program(GENHTML_EXECUTABLE genhtml)

    if(LCOV_EXECUTABLE AND GENHTML_EXECUTABLE)
        add_custom_target(coverage
            COMMAND ${LCOV_EXECUTABLE} --capture --directory ${CMAKE_SOURCE_DIR}/../src --output-file coverage.info
            COMMAND ${GENHTML_EXECUTABLE} -o coverage_html coverage.info
            DEPENDS falcon_core_tests falcon_http_tests falcon_private_protocol_tests falcon_integration_tests
            COMMENT "Generating code coverage report"
        )
    endif()
endif()

# ============================================================================
# 测试报告
# ============================================================================

# 测试输出目录
set(TEST_OUTPUT_DIR ${CMAKE_BINARY_DIR}/test_output)

# 创建测试输出目录
file(MAKE_DIRECTORY ${TEST_OUTPUT_DIR})

# 配置测试输出
set_property(DIRECTORY PROPERTY ADDITIONAL_MAKE_CLEAN_FILES ${TEST_OUTPUT_DIR})