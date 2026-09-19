cmake_minimum_required(VERSION 3.15)

project(falcon-cli
    VERSION 0.2.0
    DESCRIPTION "Falcon command-line download tool"
    LANGUAGES CXX
)

# ============================================================================
# Executable
# ============================================================================

add_executable(falcon-cli
    src/arg_parser.cpp
    src/main.cpp
    src/config_loader.cpp
)

target_include_directories(falcon-cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(falcon-cli
    PRIVATE
        Falcon::core
        Falcon::protocols
        # 强制纳入内置协议强符号定义（见 libfalcon-protocols/CMakeLists.txt 锚点说明）
        Falcon::builtin_protocol_handlers
)

# Link nlohmann_json if available
find_package(nlohmann_json QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(falcon-cli PRIVATE nlohmann_json::nlohmann_json)
    target_compile_definitions(falcon-cli PRIVATE FALCON_USE_JSON)
endif()

target_compile_features(falcon-cli PRIVATE cxx_std_17)

# 版本号单一事实源 = project(VERSION)；CLI 内不得再硬编码版本字符串
target_compile_definitions(falcon-cli PRIVATE
    FALCON_CLI_VERSION="${PROJECT_VERSION}"
)

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
