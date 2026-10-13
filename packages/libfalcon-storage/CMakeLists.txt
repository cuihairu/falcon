cmake_minimum_required(VERSION 3.15)

project(libfalcon-storage
    VERSION 0.1.0
    DESCRIPTION "Falcon storage and resource browser library"
    LANGUAGES CXX
)

# ============================================================================
# Export set anchor（先于一切 install 计算）
#
# monorepo 树内（根已建 falcon_core）沿用 FalconTargets 单体导出（#12
# 消费路径零变化）；独立 configure（vcpkg 端口形态，本包为顶层 project）
# 自带 FalconStorageTargets 每包导出集。判定锚 = TARGET falcon_core
# （根必然已建该目标，端口树内必然没有——不引入第二个真值来源）。
# 与 protocols 的 FALCON_PROTOCOLS_EXPORT_SET 同构
# ============================================================================

if(TARGET falcon_core)
    set(FALCON_STORAGE_EXPORT_SET FalconTargets)
    set(FALCON_STORAGE_STANDALONE OFF)
else()
    set(FALCON_STORAGE_EXPORT_SET FalconStorageTargets)
    set(FALCON_STORAGE_STANDALONE ON)
endif()

# ============================================================================
# 独立 configure 自足块（vcpkg 端口形态）
#
# monorepo 树内根已做过全部 find_package，整块跳过（零行为变化）；
# 独立形态下补齐本包消费的 finder 与上游包。option 只在未定义时兜底
# （与根 CMakeLists 的 option 同值）。OpenSSL 的查找在下方既有位置
# （monorepo/独立两形态同走，不在此重复）
# ============================================================================

if(NOT DEFINED FALCON_ENABLE_RESOURCE_BROWSER)
    set(FALCON_ENABLE_RESOURCE_BROWSER ON)
endif()
if(NOT DEFINED FALCON_ENABLE_FAILURE_INJECTION)
    set(FALCON_ENABLE_FAILURE_INJECTION OFF)
endif()

if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    # 本包既有链接点裸写 falcon_core——独立 configure 下以别名对齐
    # Falcon::core（目录级即可：消费方全在本目录子树内）
    add_library(falcon_core ALIAS Falcon::core)
    find_package(nlohmann_json QUIET)
    # 查找门与链接点同条件（RESOURCE_BROWSER 是本包唯一 curl 消费方；
    # 与根 cmake/Dependencies.cmake 的门不同——根按全包消费方求并集）
    if(FALCON_ENABLE_RESOURCE_BROWSER)
        find_package(CURL QUIET)
    endif()
endif()

# 故障注入宏与根目录同款触发条件，且必须先于本目录一切 add_library
# （injection.hpp 被 6 个 storage 浏览器插件与 storage_injection_test
# 消费——放晚了会导致库与测试 TU 的内联布局分叉，ODR 红线）。
# monorepo 下根目录已在 add_subdirectory 前全局定义，此处跳过以免
# 重复 -D 令本包全部 TU 重编
if(NOT TARGET falcon_core
        AND (FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS))
    add_compile_definitions(FALCON_FAILURE_INJECTION=1)
endif()

# ============================================================================
# Storage library
# ============================================================================

add_library(falcon_storage
    src/resource_browser.cpp
    src/resource_browser_utils.cpp
    src/storage_presets.cpp
)

add_library(Falcon::storage ALIAS falcon_storage)

# 导出名与文档/别名一致（Falcon::storage），理由同 falcon_core
set_target_properties(falcon_storage PROPERTIES EXPORT_NAME storage)

target_include_directories(falcon_storage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_storage PUBLIC cxx_std_17)

# Prevent Windows.h from defining min/max macros
if(WIN32)
    target_compile_definitions(falcon_storage PUBLIC NOMINMAX)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(falcon_storage
        PRIVATE FALCON_EXPORTS
        INTERFACE FALCON_IMPORTS
    )
endif()

find_package(OpenSSL QUIET)

target_link_libraries(falcon_storage
    PUBLIC
        falcon_core
)

if(FALCON_ENABLE_RESOURCE_BROWSER)
    if(NOT CURL_FOUND OR NOT nlohmann_json_FOUND)
        message(WARNING "Resource browser requires libcurl and nlohmann_json; disabling FALCON_ENABLE_RESOURCE_BROWSER")
        set(FALCON_ENABLE_RESOURCE_BROWSER OFF CACHE BOOL "Enable resource browser functionality" FORCE)
    else()
        target_sources(falcon_storage PRIVATE
            plugins/ftp_browser.cpp
            plugins/s3/s3_browser.cpp
            plugins/webdav/webdav_browser.cpp
        )
        target_link_libraries(falcon_storage PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_storage PRIVATE FALCON_ENABLE_RESOURCE_BROWSER)

        if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto)
            target_sources(falcon_storage PRIVATE
                plugins/s3/s3_authenticator.cpp
                plugins/oss/oss_browser.cpp
                plugins/cos/cos_browser.cpp
                plugins/kodo/kodo_browser.cpp
                plugins/upyun/upyun_browser.cpp
            )
            target_link_libraries(falcon_storage PUBLIC OpenSSL::Crypto)
            target_compile_definitions(falcon_storage PRIVATE FALCON_ENABLE_CRYPTO_STORAGE_BROWSERS)
        else()
            message(STATUS "OpenSSL not found: OSS/COS/Kodo/Upyun browsers will not be built")
        endif()
    endif()
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon_storage
    EXPORT ${FALCON_STORAGE_EXPORT_SET}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# 独立形态专属：每包 Config（monorepo 树内根目录负责 FalconConfig）
if(FALCON_STORAGE_STANDALONE)
    # 消费侧闭包打标：与上方链接点同条件（target 存在性，非 FOUND 布尔
    # ——构建侧要求 targets 齐备才链接，导出闭包只在链接时才引用它们）。
    # 三个标记全由 target 存在性派生，零二次判定
    set(FALCON_STORAGE_PACKAGE_NEEDS_CURL OFF)
    if(TARGET CURL::libcurl)
        set(FALCON_STORAGE_PACKAGE_NEEDS_CURL ON)
    endif()
    set(FALCON_STORAGE_PACKAGE_NEEDS_NLOHMANN_JSON OFF)
    if(TARGET nlohmann_json::nlohmann_json)
        set(FALCON_STORAGE_PACKAGE_NEEDS_NLOHMANN_JSON ON)
    endif()
    set(FALCON_STORAGE_PACKAGE_NEEDS_OPENSSL OFF)
    if(TARGET OpenSSL::Crypto)
        set(FALCON_STORAGE_PACKAGE_NEEDS_OPENSSL ON)
    endif()

    include(CMakePackageConfigHelpers)
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/FalconStorageConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/FalconStorageConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconStorage
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/FalconStorageConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(EXPORT FalconStorageTargets
        FILE FalconStorageTargets.cmake
        NAMESPACE Falcon::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconStorage
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/FalconStorageConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/FalconStorageConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/FalconStorage
    )
endif()

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
