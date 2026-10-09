cmake_minimum_required(VERSION 3.15)

# 本包 project VERSION 是 falcon-mcp --version 的单一事实源（沿
# falcon-cli 先例：版本注入前同步升所属包 project VERSION，防自报
# 版本与发布版静默漂移）
project(falcon-daemon
    VERSION 0.2.0
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
        src/rpc/mcp_server.cpp
        src/rpc/mcp_server.hpp
        src/rpc/json_rpc_client.cpp
        src/rpc/json_rpc_client.hpp
        src/rpc/aria2_snapshots.cpp
        src/rpc/aria2_snapshots.hpp
        src/rpc/websocket_rpc_client.cpp
        src/rpc/websocket_rpc_client.hpp
    )

    # MCP initialize serverInfo.version 单一事实源 = 本包 project(VERSION)：
    # 构建期注入（沿 falcon-cli FALCON_CLI_VERSION / falcon-mcp
    # FALCON_MCP_VERSION 先例；选注入而非生成头文件——同款机制仓内已有
    # 两处，免新增 configure_file 模板与生成目录 include 路径）。经
    # mcp_server_version() 访问器暴露，测试引用同一访问器做一致性钉子。
    target_compile_definitions(falcon_daemon_rpc PRIVATE
        FALCON_DAEMON_VERSION="${PROJECT_VERSION}"
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

    # swarm_announcer 挂可执行目标而非 falcon_daemon_core——真实网关依赖
    # falcon_swarm_client（CURL），core 库保持零 CURL 依赖；falcon_swarm_client
    # 可用时由根 CMakeLists（swarmd add_subdirectory 之后）补链 + 定义
    # FALCON_HAS_SWARM（编译顺序：本目录先于 swarmd，target 届时尚不存在）
    # swarm_data_service（P2SP 阶段 2 增量 1，§10.3 入站只读 HTTP 数据服务）
    # 同挂可执行目标：零外部依赖（仅 POSIX/Winsock socket），随 daemon 启动；
    # 与 swarm_announcer 一起构成 P2SP 数据面（公告 + 服务）。
    add_executable(falcon-daemon
        src/main.cpp
        src/daemon/config.cpp
        src/daemon/swarm_announcer.cpp
        src/daemon/swarm_announcer.hpp
        src/daemon/swarm_data_service.cpp
        src/daemon/swarm_data_service.hpp
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
    # MCP stdio 薄壳（falcon-mcp，阶段 2 增量 2）
    # ============================================================================

    # 桥层：stdin 行协议 ↔ daemon /mcp。仅依赖 nlohmann json + CURL，
    # 不链 libfalcon-core（设计文档 §4：避免第二个进程内引擎形态）。
    # 传输经 HttpPostFn 注入——curl 实现（mcp_curl_post）与单测 fake
    # 走同一桥逻辑。
    if(CURL_FOUND)
        add_library(falcon_daemon_mcp_stdio STATIC
            tools/mcp_stdio.cpp
            tools/mcp_stdio.hpp
            tools/mcp_curl_post.cpp
            tools/mcp_curl_post.hpp
        )

        target_link_libraries(falcon_daemon_mcp_stdio
            PUBLIC
                nlohmann_json::nlohmann_json
                CURL::libcurl
        )

        target_include_directories(falcon_daemon_mcp_stdio
            PUBLIC
                ${CMAKE_CURRENT_SOURCE_DIR}/tools
        )

        target_compile_features(falcon_daemon_mcp_stdio PRIVATE cxx_std_17)

        add_executable(falcon-mcp tools/falcon-mcp.cpp)
        target_link_libraries(falcon-mcp PRIVATE falcon_daemon_mcp_stdio)
        target_compile_features(falcon-mcp PRIVATE cxx_std_17)
        target_compile_definitions(falcon-mcp
            PRIVATE FALCON_MCP_VERSION="${PROJECT_VERSION}")
    endif()

    # ============================================================================
    # 安装
    # ============================================================================

    include(GNUInstallDirs)

    install(TARGETS falcon-daemon
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )

    if(TARGET falcon-mcp)
        install(TARGETS falcon-mcp
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()

    # ============================================================================
    # 测试
    # ============================================================================

    if(FALCON_BUILD_TESTS)
        add_subdirectory(tests)
    endif()
endif()
