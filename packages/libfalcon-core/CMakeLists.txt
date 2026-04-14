cmake_minimum_required(VERSION 3.15)

project(libfalcon-core
    VERSION 0.1.0
    DESCRIPTION "Falcon core download engine library"
    LANGUAGES CXX
)

# ============================================================================
# Dependencies
# ============================================================================

find_package(Threads REQUIRED)
find_package(spdlog QUIET)
find_package(nlohmann_json QUIET)
find_package(OpenSSL QUIET)

# ============================================================================
# Core library
# ============================================================================

add_library(falcon_core
    src/download_engine.cpp
    src/download_task.cpp
    src/task_manager.cpp
    src/thread_pool.cpp
    src/plugin_manager.cpp
    src/event_dispatcher.cpp
    src/password_manager.cpp
    src/version.cpp
)

add_library(Falcon::core ALIAS falcon_core)

target_include_directories(falcon_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(falcon_core PUBLIC cxx_std_17)

if(BUILD_SHARED_LIBS)
    target_compile_definitions(falcon_core
        PRIVATE FALCON_EXPORTS
        INTERFACE FALCON_IMPORTS
    )
endif()

target_link_libraries(falcon_core
    PUBLIC
        Threads::Threads
)

if(WIN32)
    target_link_libraries(falcon_core PUBLIC ws2_32)
endif()

if(spdlog_FOUND)
    target_link_libraries(falcon_core PUBLIC spdlog::spdlog)
    target_compile_definitions(falcon_core PUBLIC FALCON_USE_SPDLOG)
endif()

if(OPENSSL_FOUND AND TARGET OpenSSL::Crypto AND TARGET OpenSSL::SSL)
    target_link_libraries(falcon_core PUBLIC OpenSSL::Crypto OpenSSL::SSL)
    target_compile_definitions(falcon_core PRIVATE FALCON_USE_OPENSSL)
elseif(OPENSSL_FOUND)
    message(WARNING "OpenSSL found without imported CMake targets; OpenSSL-dependent features will remain disabled")
endif()

# ============================================================================
# Installation rules
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon_core
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

    if(FALCON_ENABLE_COVERAGE)
        include(cmake/CodeCoverage.cmake OPTIONAL)
        if(COMMAND setup_coverage_target)
            setup_coverage_target(falcon_coverage
                "*/libfalcon-core/tests/*"
                "*/build-*/*"
            )
        endif()
    endif()
endif()
