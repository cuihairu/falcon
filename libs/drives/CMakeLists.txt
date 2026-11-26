cmake_minimum_required(VERSION 3.15)

project(libfalcon-drives
    VERSION 0.1.0
    DESCRIPTION "Falcon cloud drives facade (SPI host, no third-party dependencies)"
    LANGUAGES CXX
)

# MSVC：源码/执行字符集统一 UTF-8（注释与字符串字面量含中文）
if(MSVC)
    add_compile_options(/utf-8)
endif()

if(BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ============================================================================
# Export set anchor
# ============================================================================
if(TARGET falcon_core)
    set(FALCON_DRIVES_EXPORT_SET FalconTargets)
    set(FALCON_DRIVES_STANDALONE OFF)
else()
    set(FALCON_DRIVES_EXPORT_SET FalconDrivesTargets)
    set(FALCON_DRIVES_STANDALONE ON)
endif()

# 插件功能开关在此仍保留（兼容既有 configure 参数）。开关的消费方已移至
# plugins/drives，门面自身恒定编译为无第三方依赖的 SPI host。
foreach(opt FALCON_ENABLE_CLOUD_STORAGE FALCON_ENABLE_RESOURCE_SEARCH
            FALCON_ENABLE_CONFIG_MANAGER FALCON_ENABLE_FAILURE_INJECTION)
    if(NOT DEFINED ${opt})
        if(${opt} STREQUAL "FALCON_ENABLE_FAILURE_INJECTION")
            set(${opt} OFF)
        else()
            set(${opt} ON)
        endif()
    endif()
endforeach()

if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    add_library(falcon_core ALIAS Falcon::core)
endif()

# 故障注入宏（插件侧也需同值，见 plugins/drives 各 CMakeLists）
if(NOT TARGET falcon_core
        AND (FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS))
    add_compile_definitions(FALCON_FAILURE_INJECTION=1)
endif()

# ============================================================================
# Facade library：仅 STL + falcon_core，不链接 curl/SQLite/OpenSSL/json
# ============================================================================
add_library(falcon_drives
    src/dummy.cpp
    src/plugin_loader.cpp
    src/cloud_link_detector.cpp
    src/search_detail.cpp
    src/cloud_storage_plugin.cpp
    src/resource_search.cpp
    src/config_manager.cpp
)

add_library(Falcon::drives ALIAS falcon_drives)
set_target_properties(falcon_drives PROPERTIES EXPORT_NAME drives)

target_include_directories(falcon_drives
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_drives PUBLIC cxx_std_17)

if(WIN32)
    target_compile_definitions(falcon_drives PUBLIC NOMINMAX)
endif()

if(BUILD_SHARED_LIBS)
    set_target_properties(falcon_drives PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_link_libraries(falcon_drives PUBLIC falcon_core)

# 特性可用性与旧版保持一致：宏表示「该能力在构建中启用」（现由插件提供），
# 消费方与测试据此决定是否编译对应调用路径。
if(FALCON_ENABLE_CLOUD_STORAGE)
    target_compile_definitions(falcon_drives PUBLIC FALCON_ENABLE_CLOUD_STORAGE=1)
endif()
if(FALCON_ENABLE_RESOURCE_SEARCH)
    target_compile_definitions(falcon_drives PUBLIC FALCON_ENABLE_RESOURCE_SEARCH=1)
endif()
if(FALCON_ENABLE_CONFIG_MANAGER)
    target_compile_definitions(falcon_drives PUBLIC FALCON_ENABLE_CONFIG_MANAGER=1)
endif()

include(GNUInstallDirs)

# 安装态插件搜索目录（可由 FALCON_PLUGIN_DIR 环境变量覆盖）
if(NOT DEFINED FALCON_PLUGIN_INSTALL_DIR)
    set(FALCON_PLUGIN_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/falcon/plugins")
endif()
target_compile_definitions(falcon_drives
    PRIVATE FALCON_PLUGIN_DEFAULT_DIR="${FALCON_PLUGIN_INSTALL_DIR}")

# ============================================================================
# Installation rules
# ============================================================================
install(TARGETS falcon_drives
    EXPORT ${FALCON_DRIVES_EXPORT_SET}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

if(FALCON_DRIVES_STANDALONE)
    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FalconDrivesConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/FalconDrivesConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconDrives
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/FalconDrivesConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT FalconDrivesTargets
        FILE FalconDrivesTargets.cmake
        NAMESPACE Falcon::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconDrives
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FalconDrivesConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FalconDrivesConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconDrives
    )
endif()

# ============================================================================
# Tests（门面层测试）
# ============================================================================
if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
