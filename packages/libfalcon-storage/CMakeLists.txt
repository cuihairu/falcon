cmake_minimum_required(VERSION 3.15)

project(libfalcon-storage
    VERSION 0.1.0
    DESCRIPTION "Falcon storage and resource browser library"
    LANGUAGES CXX
)

# ============================================================================
# Storage library
# ============================================================================

add_library(falcon_storage
    src/resource_browser.cpp
    src/resource_browser_utils.cpp
)

add_library(Falcon::storage ALIAS falcon_storage)

target_include_directories(falcon_storage
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_storage PUBLIC cxx_std_17)

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
        )
        target_link_libraries(falcon_storage PUBLIC CURL::libcurl nlohmann_json::nlohmann_json)
        target_compile_definitions(falcon_storage PRIVATE FALCON_ENABLE_RESOURCE_BROWSER)

        if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto)
            target_sources(falcon_storage PRIVATE
                plugins/oss/oss_browser.cpp
                plugins/cos/cos_browser.cpp
                plugins/kodo/kodo_browser.cpp
                plugins/upyun/upyun_browser.cpp
            )
            target_link_libraries(falcon_storage PUBLIC OpenSSL::Crypto)
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
