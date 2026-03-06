cmake_minimum_required(VERSION 3.15)

project(falcon-cli
    VERSION 0.1.0
    DESCRIPTION "Falcon command-line download tool"
    LANGUAGES CXX
)

# ============================================================================
# Executable
# ============================================================================

add_executable(falcon-cli
    src/main.cpp
    src/config_loader.cpp
    src/config_loader.hpp
)

target_link_libraries(falcon-cli
    PRIVATE
        Falcon::falcon
)

# Link nlohmann_json if available
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(falcon-cli PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon-cli PRIVATE FALCON_USE_JSON)
endif()

target_compile_features(falcon-cli PRIVATE cxx_std_17)

# ============================================================================
# Optional dependencies
# ============================================================================

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_link_libraries(falcon-cli PRIVATE spdlog::spdlog)
endif()

# ============================================================================
# Installation
# ============================================================================

include(GNUInstallDirs)

install(TARGETS falcon-cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ============================================================================
# Tests
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
