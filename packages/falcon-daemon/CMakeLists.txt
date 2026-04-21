cmake_minimum_required(VERSION 3.15)

project(falcon-daemon
    VERSION 0.1.0
    DESCRIPTION "Falcon download daemon service"
    LANGUAGES CXX
)

# ============================================================================
# Dependencies
# ============================================================================

# JSON (required for aria2-compatible JSON-RPC daemon)
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(WARNING "nlohmann_json not found: falcon-daemon targets will be skipped")
endif()

# SQLite3 for task persistence
find_package(SQLite3 QUIET)
if(SQLite3_FOUND)
    message(STATUS "SQLite3 found: ${SQLite3_VERSION}")
else()
    message(STATUS "SQLite3 not found: task persistence will be disabled")
endif()

if(nlohmann_json_FOUND)
    # ============================================================================
    # 可执行文件
    # ============================================================================

    add_library(falcon_daemon_rpc STATIC
        src/rpc/json_rpc_server.cpp
        src/rpc/json_rpc_server.hpp
        src/daemon/daemon.cpp
        src/daemon/daemon.hpp
    )

    # Task storage library (requires SQLite3)
    if(SQLite3_FOUND)
        add_library(falcon_daemon_storage STATIC
            src/storage/task_storage.cpp
            src/storage/task_storage.hpp
        )

        target_link_libraries(falcon_daemon_storage
            PUBLIC
                Falcon::core
                nlohmann_json::nlohmann_json
        )

        if(TARGET SQLite::SQLite3)
            target_link_libraries(falcon_daemon_storage PUBLIC SQLite::SQLite3)
        elseif(TARGET SQLite3::SQLite3)
            target_link_libraries(falcon_daemon_storage PUBLIC SQLite3::SQLite3)
        else()
            target_include_directories(falcon_daemon_storage PUBLIC ${SQLite3_INCLUDE_DIRS})
            target_link_libraries(falcon_daemon_storage PUBLIC ${SQLite3_LIBRARIES})
        endif()

        target_include_directories(falcon_daemon_storage
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        target_compile_features(falcon_daemon_storage PRIVATE cxx_std_17)
        target_compile_definitions(falcon_daemon_storage PUBLIC FALCON_HAS_SQLITE3)
    endif()

    target_link_libraries(falcon_daemon_rpc
        PUBLIC
            Falcon::protocols
            nlohmann_json::nlohmann_json
            # gRPC::grpc++  # 待添加
    )

    # Link storage to RPC if available
    if(TARGET falcon_daemon_storage)
        target_link_libraries(falcon_daemon_rpc PUBLIC falcon_daemon_storage)
    endif()

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

    # Link task storage if available
    if(TARGET falcon_daemon_storage)
        target_link_libraries(falcon-daemon PRIVATE falcon_daemon_storage)
        target_compile_definitions(falcon-daemon PRIVATE FALCON_HAS_SQLITE3)
    endif()

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

    include(GNUInstallDirs)

    install(TARGETS falcon-daemon
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    # ============================================================================
    # 测试
    # ============================================================================

    if(FALCON_BUILD_TESTS)
        add_subdirectory(tests)
    endif()
endif()
