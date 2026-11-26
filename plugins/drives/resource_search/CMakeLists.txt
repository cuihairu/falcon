cmake_minimum_required(VERSION 3.15)

# resource_search 插件：nlohmann_json 只在本目标内链接；HTTP 走宿主服务表
find_package(nlohmann_json REQUIRED)

if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    add_library(falcon_core ALIAS Falcon::core)
endif()

add_library(falcon_drive_resource_search MODULE
    src/resource_search.cpp
    src/spi_entry.cpp
    # 与门面共享的纯解析逻辑（各自编译一份）
    ${CMAKE_CURRENT_LIST_DIR}/../../../libs/drives/src/search_detail.cpp
)

target_include_directories(falcon_drive_resource_search
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../../libs/drives/include
)

target_compile_features(falcon_drive_resource_search PRIVATE cxx_std_17)
target_compile_definitions(falcon_drive_resource_search PRIVATE
    HAVE_NLOHMANN_JSON=1
    FALCON_ENABLE_RESOURCE_SEARCH=1
)
if(FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS)
    target_compile_definitions(falcon_drive_resource_search PRIVATE FALCON_FAILURE_INJECTION=1)
endif()
if(MSVC)
    target_compile_options(falcon_drive_resource_search PRIVATE /utf-8)
    set_target_properties(falcon_drive_resource_search PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_link_libraries(falcon_drive_resource_search
    PRIVATE
        falcon_core
        nlohmann_json::nlohmann_json
)

set_target_properties(falcon_drive_resource_search PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}"
)

include(GNUInstallDirs)
install(TARGETS falcon_drive_resource_search
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/falcon/plugins
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/falcon/plugins
)
