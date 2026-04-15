cmake_minimum_required(VERSION 3.15)

project(libfalcon-drives
    VERSION 0.1.0
    DESCRIPTION "Falcon cloud drives and share link library"
    LANGUAGES CXX
)

# ============================================================================
# Drives library
# ============================================================================

add_library(falcon_drives
    src/dummy.cpp
)

add_library(Falcon::drives ALIAS falcon_drives)

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

        target_link_libraries(falcon_drives PUBLIC OpenSSL::Crypto OpenSSL::SSL nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_drives PUBLIC FALCON_ENABLE_CONFIG_MANAGER)
    endif()
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon_drives
    EXPORT FalconTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/falcon
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp"
)

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
