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
)

target_link_libraries(falcon-cli
    PRIVATE
        Falcon::falcon
)

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
