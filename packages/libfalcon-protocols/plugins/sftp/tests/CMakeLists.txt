cmake_minimum_required(VERSION 3.15)

# SFTP 插件测试
add_executable(sftp_plugin_test
    sftp_plugin_test.cpp
)

target_link_libraries(sftp_plugin_test
    PRIVATE
        falcon_plugin_sftp
        falcon_core
        gtest_main
        gmock
)

include(GoogleTest)
gtest_discover_tests(sftp_plugin_test)
