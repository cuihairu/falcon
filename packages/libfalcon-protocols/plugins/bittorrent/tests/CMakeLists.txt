cmake_minimum_required(VERSION 3.15)

# BitTorrent 插件测试
add_executable(bittorrent_plugin_test
    bittorrent_plugin_test.cpp
)

target_link_libraries(bittorrent_plugin_test
    PRIVATE
        falcon_plugin_bittorrent
        falcon_core
        gtest_main
        gmock
)

include(GoogleTest)
gtest_discover_tests(bittorrent_plugin_test)
