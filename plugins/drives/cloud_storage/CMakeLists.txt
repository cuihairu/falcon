cmake_minimum_required(VERSION 3.15)

# cloud_storage 插件：nlohmann_json 只在本目标内链接；HTTP 由宿主服务表提供
find_package(nlohmann_json REQUIRED)

if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    add_library(falcon_core ALIAS Falcon::core)
endif()

add_library(falcon_drive_cloud_storage MODULE
    src/cloud_storage_plugin.cpp
    src/spi_entry.cpp
    # 与门面共享的纯链接识别逻辑（各自编译一份，插件不反向依赖门面库）
    ${CMAKE_CURRENT_LIST_DIR}/../../../libs/drives/src/cloud_link_detector.cpp
)

target_include_directories(falcon_drive_cloud_storage
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../../libs/drives/include
)

target_compile_features(falcon_drive_cloud_storage PRIVATE cxx_std_17)
target_compile_definitions(falcon_drive_cloud_storage PRIVATE
    HAVE_NLOHMANN_JSON=1
    FALCON_ENABLE_CLOUD_STORAGE=1
)
if(FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS)
    target_compile_definitions(falcon_drive_cloud_storage PRIVATE FALCON_FAILURE_INJECTION=1)
endif()
if(MSVC)
    target_compile_options(falcon_drive_cloud_storage PRIVATE /utf-8)
    set_target_properties(falcon_drive_cloud_storage PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

target_link_libraries(falcon_drive_cloud_storage
    PRIVATE
        falcon_core
        nlohmann_json::nlohmann_json
)

set_target_properties(falcon_drive_cloud_storage PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}"
)

include(GNUInstallDirs)
install(TARGETS falcon_drive_cloud_storage
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/falcon/plugins
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/falcon/plugins
)
