cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_metalink
    VERSION 0.1.0
    DESCRIPTION "Falcon Metalink protocol plugin"
    LANGUAGES CXX
)

add_library(falcon_plugin_metalink
    metalink_plugin.cpp
)

target_link_libraries(falcon_plugin_metalink
    PRIVATE
        falcon_core
)

if(OPENSSL_FOUND AND TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    target_link_libraries(falcon_plugin_metalink
        PRIVATE
            OpenSSL::SSL
            OpenSSL::Crypto
    )
endif()

if(spdlog_FOUND)
    target_link_libraries(falcon_plugin_metalink PRIVATE spdlog::spdlog)
    target_compile_definitions(falcon_plugin_metalink PRIVATE FALCON_USE_SPDLOG)
endif()

target_compile_features(falcon_plugin_metalink PRIVATE cxx_std_17)

# 平台特定设置
if(UNIX AND NOT APPLE)
    target_link_libraries(falcon_plugin_metalink PRIVATE pthread)
endif()

# 测试
if(FALCON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
