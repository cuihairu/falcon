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

# ============================================================================
# 基础库（零外部依赖 / 仅 Falcon::core，不进 nlohmann_json 守卫——
# 缺 nlohmann 不应连坐无依赖目标；falcon-swarmd 亦链接这两个 target）
# ============================================================================

# RFC 6455 WebSocket 帧协议层：SHA1/base64 自实现、无 socket 依赖。
# 原先在 falcon_daemon_rpc_client 与 falcon_daemon_rpc 重复编译同源
# 文件，拆出独立静态库后 RPC 服务器/客户端与 falcon-swarmd 共享单一
# 事实源。
add_library(falcon_ws_protocol STATIC
    src/rpc/websocket_frame.cpp
    src/rpc/websocket_frame.hpp
)

target_include_directories(falcon_ws_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_ws_protocol PRIVATE cxx_std_17)

# DaemonManager（守护化/信号/pid 文件/停机排水）：仅依赖 Falcon::core
# （logger/injection）。原编在 falcon_daemon_rpc 内，拆出供 falcon-daemon
# 可执行与 falcon-swarmd（复用 daemon 配置节与生命周期）共同链接。
add_library(falcon_daemon_core STATIC
    src/daemon/daemon.cpp
    src/daemon/daemon.hpp
)

target_link_libraries(falcon_daemon_core
    PUBLIC
        Falcon::core
)

target_include_directories(falcon_daemon_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_daemon_core PRIVATE cxx_std_17)

if(nlohmann_json_FOUND)
    # ============================================================================
    # 可执行文件
    # ============================================================================

    # RPC 客户端 + aria2 快照转换 + WebSocket 事件流客户端：无服务端依赖，
    # desktop 可直接链接（websocket_frame 经 falcon_ws_protocol 共享）
    if(CURL_FOUND)
        add_library(falcon_daemon_rpc_client STATIC
            src/rpc/json_rpc_client.cpp
            src/rpc/json_rpc_client.hpp
            src/rpc/aria2_snapshots.cpp
            src/rpc/aria2_snapshots.hpp
            src/rpc/websocket_rpc_client.cpp
            src/rpc/websocket_rpc_client.hpp
        )

        target_link_libraries(falcon_daemon_rpc_client
            PUBLIC
                Falcon::core
                nlohmann_json::nlohmann_json
                CURL::libcurl
                falcon_ws_protocol
        )

        target_include_directories(falcon_daemon_rpc_client
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/src
        )

        target_compile_features(falcon_daemon_rpc_client PRIVATE cxx_std_17)
    endif()

    add_library(falcon_daemon_rpc STATIC
        src/rpc/json_rpc_server.cpp
        src/rpc/json_rpc_server.hpp
        src/rpc/json_rpc_client.cpp
        src/rpc/json_rpc_client.hpp
        src/rpc/aria2_snapshots.cpp
        src/rpc/aria2_snapshots.hpp
        src/rpc/websocket_rpc_client.cpp
        src/rpc/websocket_rpc_client.hpp
    )

    # Task storage library (requires SQLite3)
    if(SQLite3_FOUND)
        add_library(falcon_daemon_storage STATIC
            src/storage/task_storage.cpp
            src/storage/task_storage.hpp
            src/storage/task_storage_listener.cpp
            src/storage/task_storage_listener.hpp
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
            Falcon::core
            Falcon::protocols
            nlohmann_json::nlohmann_json
            falcon_ws_protocol
            falcon_daemon_core
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
        src/daemon/config.cpp
    )

    target_link_libraries(falcon-daemon
        PRIVATE
            falcon_daemon_rpc
            # daemon.cpp 拆出后不再经 falcon_daemon_rpc 携带，显式链接
            falcon_daemon_core
            # 强制纳入内置协议强符号定义（见 libfalcon-protocols/CMakeLists.txt 锚点说明）
            Falcon::builtin_protocol_handlers
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
