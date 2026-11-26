cmake_minimum_required(VERSION 3.15)

project(libfalcon-core
    VERSION 0.1.0
    DESCRIPTION "Falcon core download engine library"
    LANGUAGES CXX
)

# MSVC：源码/执行字符集统一 UTF-8。全库注释与字符串字面量携带 UTF-8
# 中文，无 BOM 时 MSVC 按 locale 代码页解码（zh-CN CP936 双字节配对
# 可吞换行/引号 → C4819/C2001 类红面），/utf-8 消除 locale 依赖。
# 独立 configure（vcpkg 端口形态）不经根 CMakeLists，必须自带
if(MSVC)
    add_compile_options(/utf-8)
endif()

# ============================================================================
# Shared-build wiring（BUILD_SHARED_LIBS=ON，vcpkg 动态 triplet 形态）
#
# PIC 开关只挂共享构建——静态构建整块跳过（零行为变化）。共享形态下
# 库对象必须位置无关；静态构建保持既有非 PIC 编译面不变
# ============================================================================
if(BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ============================================================================
# Dependencies
# ============================================================================

find_package(Threads REQUIRED)
find_package(spdlog QUIET)
find_package(nlohmann_json QUIET)
find_package(OpenSSL QUIET)

# ============================================================================
# Core library
# ============================================================================

add_library(falcon_core
    src/download_engine.cpp
    src/logger.cpp
    src/download_task.cpp
    src/task_manager.cpp
    src/thread_pool.cpp
    src/protocol_registry.cpp
    src/builtin_protocol_handlers_stub.cpp
    src/event_dispatcher.cpp
    src/password_manager.cpp
    src/version.cpp
)

add_library(Falcon::core ALIAS falcon_core)

# 导出名与文档/别名一致（Falcon::core）——默认导出名为真实 target 名
# （Falcon::falcon_core），外部消费方按别名找不到目标
set_target_properties(falcon_core PROPERTIES EXPORT_NAME core)

target_include_directories(falcon_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(falcon_core PUBLIC cxx_std_17)

if(BUILD_SHARED_LIBS)
    # MSVC 共享库导出接线：FALCON_EXPORTS/FALCON_IMPORTS 宏全库零消费
    # （无任何 __declspec 接线），共享构建导出表为空 = 坏 DLL。CMake 在
    # MSVC 构建期枚举符号生成 .def 自动导出（非 MSVC 平台忽略此属性）
    set_target_properties(falcon_core PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_link_libraries(falcon_core
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(falcon_core PUBLIC ws2_32)
endif()

if(spdlog_FOUND)
    target_link_libraries(falcon_core PUBLIC spdlog::spdlog)
    target_compile_definitions(falcon_core PUBLIC FALCON_USE_SPDLOG)
endif()

if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
    target_link_libraries(falcon_core PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(falcon_core PRIVATE FALCON_USE_OPENSSL)
elseif(OPENSSL_FOUND)
    message(WARNING "OpenSSL found without imported CMake targets; OpenSSL-dependent features will remain disabled")
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

# 独立 configure（vcpkg 端口形态，本包为顶层 project）时根目录的
# FalconTargets 导出集无人 install——本包自带 FalconCoreTargets 导出与
# FalconCoreConfig；monorepo 树内沿用 FalconTargets（#12 验收过的单体
# find_package(Falcon) 消费路径零变化）。判定锚 = 顶层 configure（core
# 无上游包，TARGET 锚不适用于本包）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(FALCON_CORE_EXPORT_SET FalconCoreTargets)
else()
    set(FALCON_CORE_EXPORT_SET FalconTargets)
endif()

install(TARGETS falcon_core
    EXPORT ${FALCON_CORE_EXPORT_SET}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
)

# 独立形态专属：每包 Config（monorepo 树内根目录负责 FalconConfig）
if(FALCON_CORE_EXPORT_SET STREQUAL "FalconCoreTargets")
    # 消费侧闭包打标：与上方链接点同条件（target 存在性，非 FOUND 布尔
    # ——构建侧要求 targets 齐备才链接，导出闭包只在链接时才引用它们）
    set(FALCON_CORE_PACKAGE_NEEDS_SPDLOG OFF)
    if(TARGET spdlog::spdlog)
        set(FALCON_CORE_PACKAGE_NEEDS_SPDLOG ON)
    endif()
    set(FALCON_CORE_PACKAGE_NEEDS_OPENSSL OFF)
    if(TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
        set(FALCON_CORE_PACKAGE_NEEDS_OPENSSL ON)
    endif()

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FalconCoreConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/FalconCoreConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconCore
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/FalconCoreConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT FalconCoreTargets
        FILE FalconCoreTargets.cmake
        NAMESPACE Falcon::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconCore
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FalconCoreConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FalconCoreConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconCore
    )
endif()

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)

    if(FALCON_ENABLE_COVERAGE)
        include(cmake/CodeCoverage.cmake OPTIONAL)
        if(COMMAND setup_coverage_target)
            setup_coverage_target(falcon_coverage
                "*/libfalcon-core/tests/*"
                "*/build-*/*"
            )
        endif()
    endif()
endif()
