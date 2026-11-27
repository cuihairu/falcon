# CMakeLists.txt for libfalcon-drives Tests
cmake_minimum_required(VERSION 3.15)

project(falcon_drives_tests_project
    VERSION 0.1.0
    DESCRIPTION "Falcon drives library tests"
    LANGUAGES CXX
)

# ============================================================================
# Find GoogleTest
# ============================================================================
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(WARNING "GTest not found: skipping libfalcon-drives tests")
    return()
endif()

enable_testing()

# ============================================================================
# Helper functions
# ============================================================================
function(add_test_compile_options target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_17)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic -Wno-unused-parameter
            $<$<CONFIG:Debug>:-g -O0>
            $<$<CONFIG:Release>:-O2>
        )
    endif()
endfunction()

function(add_test_includes target_name)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../src
            ${CMAKE_CURRENT_SOURCE_DIR}/..
    )
endfunction()

# ============================================================================
# Drives unit tests
# ============================================================================
set(FALCON_DRIVES_TEST_SOURCES)

if(FALCON_ENABLE_CLOUD_STORAGE)
    list(APPEND FALCON_DRIVES_TEST_SOURCES
        unit/cloud_storage_test.cpp
        unit/cloud_storage_coverage_test.cpp
        unit/cloud_storage_coverage_test_part2.cpp
    )
endif()

if(FALCON_ENABLE_RESOURCE_SEARCH)
    list(APPEND FALCON_DRIVES_TEST_SOURCES
        unit/search_basic_test.cpp
        unit/resource_search_test.cpp
        unit/resource_search_coverage_test.cpp
        unit/resource_search_coverage_test_part2.cpp
    )
endif()

if(FALCON_ENABLE_CONFIG_MANAGER)
    list(APPEND FALCON_DRIVES_TEST_SOURCES
        unit/config_manager_test.cpp
        unit/config_manager_test_part2.cpp
    )
endif()

if(FALCON_DRIVES_TEST_SOURCES)
    add_executable(falcon_drives_tests
        ${FALCON_DRIVES_TEST_SOURCES}
        unit/abi_host_setup.cpp
        unit/drives_isolation_test.cpp
    )

    target_link_libraries(falcon_drives_tests
        PRIVATE
            falcon_core
            falcon_drives
            falcon_protocols
            GTest::gtest_main
    )

    # 门面已不链接第三方库；测试自身直接使用这些头文件/API，显式补齐。
    find_package(nlohmann_json QUIET)
    if(nlohmann_json_FOUND)
        target_link_libraries(falcon_drives_tests PRIVATE nlohmann_json::nlohmann_json)
    endif()

    if(FALCON_ENABLE_CONFIG_MANAGER)
        find_package(SQLite3 REQUIRED)
        find_package(OpenSSL REQUIRED)
        target_include_directories(falcon_drives_tests PRIVATE ${SQLite3_INCLUDE_DIRS})
        if(TARGET SQLite::SQLite3)
            target_link_libraries(falcon_drives_tests PRIVATE SQLite::SQLite3)
        elseif(TARGET SQLite3::SQLite3)
            target_link_libraries(falcon_drives_tests PRIVATE SQLite3::SQLite3)
        else()
            target_link_libraries(falcon_drives_tests PRIVATE ${SQLite3_LIBRARIES})
        endif()
        target_link_libraries(falcon_drives_tests PRIVATE OpenSSL::Crypto OpenSSL::SSL)
    endif()

    add_test_compile_options(falcon_drives_tests)
    add_test_includes(falcon_drives_tests)

    include(GoogleTest)
    gtest_discover_tests(falcon_drives_tests)
endif()
