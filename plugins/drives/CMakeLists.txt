cmake_minimum_required(VERSION 3.15)

project(falcon-drives-plugins
    VERSION 0.1.0
    DESCRIPTION "Drives SPI plugins (cloud storage / resource search / config storage)"
    LANGUAGES CXX
)

# ============================================================================
# 统一构建产物目录：构建树内 falcon/plugins/
# 门面 PluginLoader 会从可执行文件同级的 falcon/plugins 或环境变量发现它们。
# ============================================================================
set(FALCON_DRIVES_PLUGIN_OUTPUT_DIR "${CMAKE_BINARY_DIR}/falcon/plugins"
    CACHE PATH "Output directory for Falcon drives plugins")
file(MAKE_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}")

if(BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

if(FALCON_ENABLE_CLOUD_STORAGE)
    add_subdirectory(cloud_storage)
endif()

if(FALCON_ENABLE_RESOURCE_SEARCH)
    add_subdirectory(resource_search)
endif()

if(FALCON_ENABLE_CONFIG_MANAGER)
    add_subdirectory(config_storage)
endif()
