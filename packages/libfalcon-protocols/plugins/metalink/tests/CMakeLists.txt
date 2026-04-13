cmake_minimum_required(VERSION 3.15)

# Metalink 插件测试
add_executable(metalink_plugin_test
    metalink_plugin_test.cpp
)

target_link_libraries(metalink_plugin_test
    PRIVATE
        falcon_plugin_metalink
        gtest_main
        gmock
)

target_include_directories(metalink_plugin_test
    PRIVATE
        ${CMAKE_SOURCE_DIR}/packages/libfalcon/include
)

include(GoogleTest)
gtest_discover_tests(metalink_plugin_test)
