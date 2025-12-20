cmake_minimum_required(VERSION 3.15)

project(libfalcon
    VERSION 0.1.0
    DESCRIPTION "Falcon core download engine library"
    LANGUAGES CXX
)

# ============================================================================
# Library target
# ============================================================================

add_library(falcon
    # Core implementation
    src/download_engine.cpp
    src/download_task.cpp
    src/task_manager.cpp
    src/thread_pool.cpp
)

# Alias for consistent usage
add_library(Falcon::falcon ALIAS falcon)

# ============================================================================
# Include directories
# ============================================================================

target_include_directories(falcon
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# ============================================================================
# Compile features
# ============================================================================

target_compile_features(falcon PUBLIC cxx_std_17)

# ============================================================================
# Link dependencies
# ============================================================================

find_package(Threads REQUIRED)

target_link_libraries(falcon
    PUBLIC
        Threads::Threads
)

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_link_libraries(falcon PUBLIC spdlog::spdlog)
    target_compile_definitions(falcon PUBLIC FALCON_USE_SPDLOG)
endif()

# Optional: nlohmann_json for config
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(falcon PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon PUBLIC FALCON_USE_JSON)
endif()

# ============================================================================
# Export symbols (Windows DLL)
# ============================================================================

if(BUILD_SHARED_LIBS)
    target_compile_definitions(falcon
        PRIVATE FALCON_EXPORTS
        INTERFACE FALCON_IMPORTS
    )
endif()

# ============================================================================
# Protocol plugins
# ============================================================================

# HTTP/HTTPS plugin
if(FALCON_ENABLE_HTTP)
    add_subdirectory(plugins/http)
    target_sources(falcon PRIVATE
        $<TARGET_OBJECTS:falcon_plugin_http>
    )
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_HTTP_PLUGIN)
    # Link curl to falcon directly if http plugin uses it
    find_package(CURL QUIET)
    if(CURL_FOUND)
        target_link_libraries(falcon PRIVATE CURL::libcurl)
    endif()
endif()

# FTP plugin
if(FALCON_ENABLE_FTP)
    add_subdirectory(plugins/ftp)
    target_link_libraries(falcon PRIVATE falcon_plugin_ftp)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_FTP_PLUGIN)
endif()

# BitTorrent plugin
if(FALCON_ENABLE_BITTORRENT)
    add_subdirectory(plugins/bittorrent)
    target_link_libraries(falcon PRIVATE falcon_plugin_bittorrent)
    target_compile_definitions(falcon PRIVATE FALCON_ENABLE_BT_PLUGIN)
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon
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
