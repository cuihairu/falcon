cmake_minimum_required(VERSION 3.15)

project(falcon-daemon
    VERSION 0.1.0
    DESCRIPTION "Falcon download daemon service"
    LANGUAGES CXX
)

# ============================================================================
# Dependencies
# ============================================================================

include(FetchContent)

# JSON (needed for aria2-compatible JSON-RPC)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found: fetching via FetchContent")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.11.3
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ============================================================================
# 可执行文件
# ============================================================================

add_library(falcon_daemon_rpc STATIC
    src/rpc/json_rpc_server.cpp
    src/rpc/json_rpc_server.hpp
)

target_link_libraries(falcon_daemon_rpc
    PUBLIC
        Falcon::falcon
        nlohmann_json::nlohmann_json
        # gRPC::grpc++  # 待添加
        # SQLite::SQLite3  # 待添加
)

target_include_directories(falcon_daemon_rpc
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_daemon_rpc PRIVATE cxx_std_17)

add_executable(falcon-daemon
    src/main.cpp
)

target_link_libraries(falcon-daemon
    PRIVATE
        falcon_daemon_rpc
)

# Optional: spdlog for logging
find_package(spdlog QUIET)
if(spdlog_FOUND)
    target_link_libraries(falcon-daemon PRIVATE spdlog::spdlog)
    target_compile_definitions(falcon-daemon PRIVATE FALCON_USE_SPDLOG)
endif()

target_compile_features(falcon-daemon PRIVATE cxx_std_17)

# ============================================================================
# 安装
# ============================================================================

install(TARGETS falcon-daemon
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ============================================================================
# 测试
# ============================================================================

if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
