cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_sftp
    VERSION 0.1.0
    DESCRIPTION "Falcon SFTP protocol plugin (libssh2)"
    LANGUAGES CXX
)

add_library(falcon_plugin_sftp
    sftp_plugin.cpp
    sftp_handler.hpp
)

target_include_directories(falcon_plugin_sftp
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
)

# LIBSSH2_TARGET 由父目录（protocols CMakeLists）解析后传入
target_link_libraries(falcon_plugin_sftp
    PRIVATE
        falcon_core
        ${LIBSSH2_TARGET}
)

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_link_libraries(falcon_plugin_sftp PRIVATE spdlog::spdlog)
    target_compile_definitions(falcon_plugin_sftp PRIVATE FALCON_USE_SPDLOG)
endif()

target_compile_features(falcon_plugin_sftp PRIVATE cxx_std_17)
