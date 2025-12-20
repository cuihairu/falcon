cmake_minimum_required(VERSION 3.15)

project(Falcon
    VERSION 0.1.0
    DESCRIPTION "Modern multi-protocol downloader library and tools"
    LANGUAGES CXX
)

# ============================================================================
# 全局设置
# ============================================================================

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 使 IDE 支持文件夹组织
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# ============================================================================
# 用户可配置选项
# ============================================================================

# 构建选项
option(FALCON_BUILD_TESTS "Build tests" ON)
option(FALCON_BUILD_EXAMPLES "Build examples" ON)
option(FALCON_BUILD_CLI "Build CLI tool" ON)
option(FALCON_BUILD_DAEMON "Build daemon service" ON)

# 插件开关
option(FALCON_ENABLE_HTTP "Enable HTTP/HTTPS plugin" ON)
option(FALCON_ENABLE_FTP "Enable FTP plugin" ON)
option(FALCON_ENABLE_BITTORRENT "Enable BitTorrent plugin" OFF)

# 依赖管理
option(FALCON_USE_SYSTEM_LIBS "Use system libraries instead of vcpkg" OFF)
option(FALCON_USE_STATIC_LIBS "Link dependencies statically" OFF)

# 开发选项
option(FALCON_ENABLE_COVERAGE "Enable code coverage" OFF)
option(FALCON_ENABLE_SANITIZERS "Enable sanitizers (ASan, UBSan)" OFF)

# ============================================================================
# 编译器警告与标准
# ============================================================================

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Wall -Wextra -Wpedantic
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wformat=2
    )

    if(FALCON_ENABLE_SANITIZERS)
        add_compile_options(-fsanitize=address,undefined)
        add_link_options(-fsanitize=address,undefined)
    endif()
elseif(MSVC)
    add_compile_options(
        /W4
        /permissive-
        /Zc:__cplusplus
    )
endif()

# ============================================================================
# 依赖查找
# ============================================================================

include(cmake/Dependencies.cmake)

# ============================================================================
# 子项目
# ============================================================================

# 核心库（必选）
add_subdirectory(packages/libfalcon)

# CLI 工具（可选）
if(FALCON_BUILD_CLI)
    add_subdirectory(packages/falcon-cli)
endif()

# Daemon 服务（可选）
if(FALCON_BUILD_DAEMON)
    add_subdirectory(packages/falcon-daemon)
endif()

# 示例代码（可选）
if(FALCON_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# 测试
# ============================================================================

if(FALCON_BUILD_TESTS)
    enable_testing()
    # 测试由各子项目自行添加
endif()

# ============================================================================
# 安装配置
# ============================================================================

include(GNUInstallDirs)

# 导出目标配置
install(EXPORT FalconTargets
    FILE FalconTargets.cmake
    NAMESPACE Falcon::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Falcon
)

# 生成配置文件
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FalconConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/FalconConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Falcon
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/FalconConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/FalconConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/FalconConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Falcon
)

# ============================================================================
# 项目总结
# ============================================================================

message(STATUS "========================================")
message(STATUS "Falcon Downloader - Configuration Summary")
message(STATUS "========================================")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "")
message(STATUS "Build Options:")
message(STATUS "  Build Tests: ${FALCON_BUILD_TESTS}")
message(STATUS "  Build Examples: ${FALCON_BUILD_EXAMPLES}")
message(STATUS "  Build CLI: ${FALCON_BUILD_CLI}")
message(STATUS "  Build Daemon: ${FALCON_BUILD_DAEMON}")
message(STATUS "")
message(STATUS "Plugin Options:")
message(STATUS "  HTTP/HTTPS: ${FALCON_ENABLE_HTTP}")
message(STATUS "  FTP: ${FALCON_ENABLE_FTP}")
message(STATUS "  BitTorrent: ${FALCON_ENABLE_BITTORRENT}")
message(STATUS "")
message(STATUS "Install Prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "========================================")
