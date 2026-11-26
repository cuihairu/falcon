cmake_minimum_required(VERSION 3.15)

# config_storage 插件：SQLite3 + OpenSSL + nlohmann_json 只在本目标内链接
find_package(SQLite3 REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)

if(NOT TARGET falcon_core)
    find_package(FalconCore CONFIG REQUIRED)
    add_library(falcon_core ALIAS Falcon::core)
endif()

add_library(falcon_drive_config_storage MODULE
    src/config_manager.cpp
    src/spi_entry.cpp
)

target_include_directories(falcon_drive_config_storage
    PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/../../../libs/drives/include
        ${SQLite3_INCLUDE_DIRS}
)

target_compile_features(falcon_drive_config_storage PRIVATE cxx_std_17)
target_compile_definitions(falcon_drive_config_storage PRIVATE
    FALCON_ENABLE_CONFIG_MANAGER=1
)
if(FALCON_ENABLE_FAILURE_INJECTION OR FALCON_BUILD_TESTS)
    target_compile_definitions(falcon_drive_config_storage PRIVATE FALCON_FAILURE_INJECTION=1)
endif()
if(MSVC)
    target_compile_options(falcon_drive_config_storage PRIVATE /utf-8)
    set_target_properties(falcon_drive_config_storage PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(TARGET SQLite::SQLite3)
    set(_falcon_sqlite_target SQLite::SQLite3)
elseif(TARGET SQLite3::SQLite3)
    set(_falcon_sqlite_target SQLite3::SQLite3)
else()
    set(_falcon_sqlite_target ${SQLite3_LIBRARIES})
endif()

target_link_libraries(falcon_drive_config_storage
    PRIVATE
        falcon_core
        ${_falcon_sqlite_target}
        OpenSSL::Crypto
        OpenSSL::SSL
        nlohmann_json::nlohmann_json
)

set_target_properties(falcon_drive_config_storage PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${FALCON_DRIVES_PLUGIN_OUTPUT_DIR}"
)

include(GNUInstallDirs)
install(TARGETS falcon_drive_config_storage
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/falcon/plugins
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/falcon/plugins
)
