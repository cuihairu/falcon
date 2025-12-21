cmake_minimum_required(VERSION 3.15)

project(falcon-daemon
    VERSION 0.1.0
    DESCRIPTION "Falcon download daemon service"
    LANGUAGES CXX
)

# ============================================================================
# 可执行文件
# ============================================================================

add_executable(falcon-daemon
    src/main.cpp
    # src/daemon.cpp
    # src/rpc_server.cpp
    # src/task_storage.cpp
)

target_link_libraries(falcon-daemon
    PRIVATE
        Falcon::falcon
        # gRPC::grpc++  # 待添加
        # SQLite::SQLite3  # 待添加
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
