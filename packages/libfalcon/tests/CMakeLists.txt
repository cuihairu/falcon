# CMakeLists.txt for Falcon Tests - Simplified Version
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
# 为测试目标添加通用编译选项的函数
# ============================================================================
function(add_test_compile_options target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_17)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wno-unused-parameter
        )
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O2>
        )
    endif()
endfunction()

# ============================================================================
# 核心单元测试
# ============================================================================
add_executable(falcon_core_tests
    # 核心模块测试
    unit/download_task_test.cpp
    unit/task_manager_test.cpp
    unit/url_utils_test.cpp
    unit/bug_fix_test.cpp
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

add_test_compile_options(falcon_core_tests)

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

add_test_compile_options(falcon_http_tests)

# ============================================================================
# 集成测试
# ============================================================================
add_executable(falcon_integration_tests
    integration/download_integration_test.cpp
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

add_test_compile_options(falcon_integration_tests)

# 查找并链接 libcurl
find_package(CURL QUIET)
if(CURL_FOUND)
    target_link_libraries(falcon_integration_tests PRIVATE CURL::libcurl)
    target_compile_definitions(falcon_integration_tests PRIVATE FALCON_TEST_CURL_AVAILABLE)
endif()

# ============================================================================
# 注册测试到 CTest
# ============================================================================

# 基础测试发现
include(GoogleTest)

# 注册核心测试
gtest_discover_tests(falcon_core_tests)

# 注册 HTTP 测试（如果存在）
if(TARGET falcon_http_tests)
    gtest_discover_tests(falcon_http_tests)
endif()

# 注册集成测试
gtest_discover_tests(falcon_integration_tests)