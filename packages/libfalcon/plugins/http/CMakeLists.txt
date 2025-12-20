cmake_minimum_required(VERSION 3.15)

project(falcon_plugin_http
    VERSION 0.1.0
    DESCRIPTION "Falcon HTTP/HTTPS protocol plugin"
    LANGUAGES CXX
)

# ============================================================================
# Plugin library (OBJECT library for embedding in main falcon library)
# ============================================================================

add_library(falcon_plugin_http OBJECT
    http_plugin.cpp
)

target_include_directories(falcon_plugin_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
)

# Required for OBJECT libraries used in shared libraries
set_property(TARGET falcon_plugin_http PROPERTY POSITION_INDEPENDENT_CODE ON)

target_compile_features(falcon_plugin_http PRIVATE cxx_std_17)

# ============================================================================
# Compile definitions (linking handled by parent falcon library)
# ============================================================================

# Optional: libcurl for actual HTTP downloads
find_package(CURL QUIET)
if(CURL_FOUND)
    target_compile_definitions(falcon_plugin_http PRIVATE FALCON_USE_CURL)
    # Include directories for CURL (linking done in parent CMakeLists.txt)
    target_include_directories(falcon_plugin_http PRIVATE ${CURL_INCLUDE_DIRS})
    message(STATUS "HTTP plugin: libcurl found, full functionality enabled")
else()
    message(STATUS "HTTP plugin: libcurl not found, limited functionality")
endif()

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_compile_definitions(falcon_plugin_http PRIVATE FALCON_USE_SPDLOG)
endif()
