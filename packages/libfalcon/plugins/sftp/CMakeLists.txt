cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_sftp
    VERSION 0.1.0
    DESCRIPTION "Falcon SFTP protocol plugin"
    LANGUAGES CXX
)

# 选项控制是否启用 libssh
option(FALCON_USE_LIBSSH "Use libssh for SFTP support" ON)

if(FALCON_USE_LIBSSH)
    # 查找 libssh
    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBSSH libssh QUIET)
    endif()

    if(NOT LIBSSH_FOUND)
        find_package(Libssh 0.9 QUIET)
        if(Libssh_FOUND)
            set(LIBSSH_FOUND TRUE)
            set(LIBSSH_LIBRARIES Libssh::Libssh)
            set(LIBSSH_INCLUDE_DIRS "")
        endif()
    endif()

    if(LIBSSH_FOUND)
        message(STATUS "SFTP plugin: Using libssh")
        add_compile_definitions(FALCON_USE_LIBSSH)
    else()
        message(WARNING "libssh not found, SFTP support will be disabled")
    endif()
endif()

add_library(falcon_plugin_sftp
    sftp_plugin.cpp
)

target_include_directories(falcon_plugin_sftp
    PRIVATE
        ${CMAKE_SOURCE_DIR}/packages/libfalcon/include
        ${LIBSSH_INCLUDE_DIRS}
)

target_link_libraries(falcon_plugin_sftp
    PRIVATE
        ${LIBSSH_LIBRARIES}
        spdlog::spdlog
)

target_compile_features(falcon_plugin_sftp PRIVATE cxx_std_17)

# 平台特定设置
if(UNIX AND NOT APPLE)
    target_link_libraries(falcon_plugin_sftp PRIVATE pthread)
endif()

# 测试
if(FALCON_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
