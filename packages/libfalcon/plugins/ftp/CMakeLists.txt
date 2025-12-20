cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_ftp
    VERSION 0.1.0
    DESCRIPTION "Falcon FTP/FTPS protocol plugin"
    LANGUAGES CXX
)

add_library(falcon_plugin_ftp
    ftp_plugin.cpp
)

target_link_libraries(falcon_plugin_ftp
    PRIVATE
        CURL::libcurl
        spdlog::spdlog
)

target_compile_features(falcon_plugin_ftp PRIVATE cxx_std_17)
