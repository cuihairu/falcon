cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_webdav
    VERSION 0.1.0
    DESCRIPTION "Falcon WebDAV protocol plugin (libcurl)"
    LANGUAGES CXX
)

add_library(falcon_plugin_webdav
    webdav_plugin.cpp
    webdav_handler.hpp
)

target_include_directories(falcon_plugin_webdav
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

target_link_libraries(falcon_plugin_webdav
    PRIVATE
        falcon_core
        CURL::libcurl
)

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_link_libraries(falcon_plugin_webdav PRIVATE spdlog::spdlog)
    target_compile_definitions(falcon_plugin_webdav PRIVATE FALCON_USE_SPDLOG)
endif()

target_compile_features(falcon_plugin_webdav PRIVATE cxx_std_17)
