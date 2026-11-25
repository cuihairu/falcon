cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_sftp
    VERSION 0.1.0
    DESCRIPTION "Falcon SFTP protocol plugin (libssh2)"
    LANGUAGES CXX
)

# 恒静态：插件是 falcon_protocols 的 PRIVATE 实现细节，对象恒被吸收进
# 该库。共享形态下随 BUILD_SHARED_LIBS 提升为独立 DSO 会让直连
# builtin_protocol_handlers 目标对象的可执行文件链接失败
# （DSO missing from command line）
add_library(falcon_plugin_sftp STATIC
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
