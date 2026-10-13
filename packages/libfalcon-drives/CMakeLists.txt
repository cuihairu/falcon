cmake_minimum_required(VERSION 3.15)

project(libfalcon-drives
    VERSION 0.1.0
    DESCRIPTION "Falcon cloud drives and share link library"
    LANGUAGES CXX
)

# ============================================================================
# Export set anchor（先于一切 install 计算）
#
# monorepo 树内（根已建 falcon_core）沿用 FalconTargets 单体导出（#12
# 消费路径零变化）；独立 configure（vcpkg 端口形态，本包为顶层 project）
# 自带 FalconDrivesTargets 每包导出集。判定锚 = TARGET falcon_core
# （根必然已建该目标，端口树内必然没有——不引入第二个真值来源）。
# 与 protocols/storage 的 *_EXPORT_SET 同构
# ============================================================================

if(TARGET falcon_core)
    set(FALCON_DRIVES_EXPORT_SET FalconTargets)
    set(FALCON_DRIVES_STANDALONE OFF)
else()
    set(FALCON_DRIVES_EXPORT_SET FalconDrivesTargets)
    set(FALCON_DRIVES_STANDALONE ON)
endif()

# ============================================================================
# 独立 configure 自足块（vcpkg 端口形态）
#
# monorepo 树内根已做过全部 find_package，整块跳过（零行为变化）；
# 独立形态下补齐本包消费的 finder 与上游包。option 只在未定义时兜底
# （与根 CMakeLists 的 option 同值）。OpenSSL / SQLite3 的查找在下方
# 既有位置（monorepo/独立两形态同走，不在此重复）
# ============================================================================

if(NOT DEFINED FALCON_ENABLE_CLOUD_STORAGE)
    set(FALCON_ENABLE_CLOUD_STORAGE ON)
endif()
if(NOT DEFINED FALCON_ENABLE_RESOURCE_SEARCH)
    set(FALCON_ENABLE_RESOURCE_SEARCH ON)
endif()
if(NOT DEFINED FALCON_ENABLE_CONFIG_MANAGER)
    set(FALCON_ENABLE_CONFIG_MANAGER ON)
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
    # 查找门与链接点同条件（CLOUD_STORAGE 与 RESOURCE_SEARCH 是本包仅有的
    # 两个 curl 消费方；config_manager 不走网络）
    if(FALCON_ENABLE_CLOUD_STORAGE OR FALCON_ENABLE_RESOURCE_SEARCH)
        find_package(CURL QUIET)
    endif()
endif()

# 故障注入宏与根目录同款触发条件，且必须先于本目录一切 add_library
# （injection.hpp 被 resource_search.cpp / config_manager.cpp 与
# config_manager_test 消费——放晚了会导致库与测试 TU 的内联布局分叉，
# ODR 红线）。monorepo 下根目录已在 add_subdirectory 前全局定义，此处
# 跳过以免重复 -D 令本包全部 TU 重编
if(NOT TARGET falcon_core
        AND (FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS))
    add_compile_definitions(FALCON_FAILURE_INJECTION=1)
endif()

# ============================================================================
# Drives library
# ============================================================================

add_library(falcon_drives
    src/dummy.cpp
)

add_library(Falcon::drives ALIAS falcon_drives)

# 导出名与文档/别名一致（Falcon::drives），理由同 falcon_core
set_target_properties(falcon_drives PROPERTIES EXPORT_NAME drives)

target_include_directories(falcon_drives
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_drives PUBLIC cxx_std_17)

# Prevent Windows.h from defining min/max macros
if(WIN32)
    target_compile_definitions(falcon_drives PUBLIC NOMINMAX)
endif()

if(BUILD_SHARED_LIBS)
    target_compile_definitions(falcon_drives
        PRIVATE FALCON_EXPORTS
        INTERFACE FALCON_IMPORTS
    )
endif()

find_package(OpenSSL QUIET)
find_package(SQLite3 QUIET)

target_link_libraries(falcon_drives
    PUBLIC
        falcon_core
)

if(nlohmann_json_FOUND)
    target_link_libraries(falcon_drives PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon_drives PUBLIC FALCON_USE_JSON)
    target_compile_definitions(falcon_drives PRIVATE HAVE_NLOHMANN_JSON)
endif()

if(FALCON_ENABLE_CLOUD_STORAGE)
    if(NOT CURL_FOUND)
        message(WARNING "Cloud storage plugins require libcurl; disabling FALCON_ENABLE_CLOUD_STORAGE")
        set(FALCON_ENABLE_CLOUD_STORAGE OFF CACHE BOOL "Enable cloud storage plugins" FORCE)
    else()
        target_sources(falcon_drives PRIVATE src/cloud_storage_plugin.cpp)
        target_link_libraries(falcon_drives PUBLIC CURL::libcurl)
        target_compile_definitions(falcon_drives PRIVATE FALCON_ENABLE_CLOUD_STORAGE)
    endif()
endif()

if(FALCON_ENABLE_RESOURCE_SEARCH)
    if(NOT CURL_FOUND OR NOT nlohmann_json_FOUND)
        message(WARNING "Resource search requires libcurl and nlohmann_json; disabling FALCON_ENABLE_RESOURCE_SEARCH")
        set(FALCON_ENABLE_RESOURCE_SEARCH OFF CACHE BOOL "Enable resource search functionality" FORCE)
    else()
        target_sources(falcon_drives PRIVATE src/resource_search.cpp)
        target_link_libraries(falcon_drives PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_drives PRIVATE FALCON_ENABLE_RESOURCE_SEARCH)
    endif()
endif()

if(FALCON_ENABLE_CONFIG_MANAGER)
    if(NOT SQLite3_FOUND OR NOT OPENSSL_FOUND OR NOT TARGET OpenSSL::Crypto OR NOT TARGET OpenSSL::SSL OR NOT nlohmann_json_FOUND)
        message(WARNING "Config manager requires SQLite3, OpenSSL, and nlohmann_json; disabling FALCON_ENABLE_CONFIG_MANAGER")
        set(FALCON_ENABLE_CONFIG_MANAGER OFF CACHE BOOL "Enable encrypted config manager (SQLite3)" FORCE)
    else()
        target_sources(falcon_drives PRIVATE src/config_manager.cpp)

        if(TARGET SQLite::SQLite3)
            target_link_libraries(falcon_drives PUBLIC SQLite::SQLite3)
        elseif(TARGET SQLite3::SQLite3)
            target_link_libraries(falcon_drives PUBLIC SQLite3::SQLite3)
        else()
            target_include_directories(falcon_drives PRIVATE ${SQLite3_INCLUDE_DIRS})
            target_link_libraries(falcon_drives PUBLIC ${SQLite3_LIBRARIES})
        endif()

        # 导出闭包引用 SQLite 的 imported target，消费方由 FalconConfig 的
        # find_dependency(SQLite3) 重建（同一查找器定义同一目标名，无需别名）；
        # 本目录 find_package 的结果根目录不可见，只能在这里判定。
        # 裸 ${SQLite3_LIBRARIES} 兜底形态闭包是绝对路径，不做导出支持
        if(TARGET SQLite::SQLite3 OR TARGET SQLite3::SQLite3)
            set(FALCON_PACKAGE_NEEDS_SQLITE3 ON CACHE INTERNAL "consumer needs sqlite3" FORCE)
        endif()

        target_link_libraries(falcon_drives PUBLIC OpenSSL::Crypto OpenSSL::SSL nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_drives PUBLIC FALCON_ENABLE_CONFIG_MANAGER)
    endif()
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

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

# 独立形态专属：每包 Config（monorepo 树内根目录负责 FalconConfig）
if(FALCON_DRIVES_STANDALONE)
    # 消费侧闭包打标：与上方链接点同条件（target 存在性，非 FOUND 布尔
    # ——构建侧要求 targets 齐备才链接，导出闭包只在链接时才引用它们）。
    # SQLITE3 复用 config_manager 链接点的 CACHE INTERNAL 标记
    # （FALCON_PACKAGE_NEEDS_SQLITE3，单一事实源收敛）
    set(FALCON_DRIVES_PACKAGE_NEEDS_NLOHMANN_JSON OFF)
    if(TARGET nlohmann_json::nlohmann_json)
        set(FALCON_DRIVES_PACKAGE_NEEDS_NLOHMANN_JSON ON)
    endif()
    set(FALCON_DRIVES_PACKAGE_NEEDS_CURL OFF)
    if(TARGET CURL::libcurl)
        set(FALCON_DRIVES_PACKAGE_NEEDS_CURL ON)
    endif()
    set(FALCON_DRIVES_PACKAGE_NEEDS_OPENSSL OFF)
    if(TARGET OpenSSL::Crypto)
        set(FALCON_DRIVES_PACKAGE_NEEDS_OPENSSL ON)
    endif()

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
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
