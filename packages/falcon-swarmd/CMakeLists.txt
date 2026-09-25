cmake_minimum_required(VERSION 3.15)

# ============================================================================
# falcon-swarmd：P2SP swarm 目录服务器 + 节点侧客户端库
# （docs/p2sp_network_design.md §12 阶段 0）
#
# 守卫链：缺基础 target（daemon 包拆出的 falcon_ws_protocol /
# falcon_daemon_core）或缺 OpenSSL 时整包优雅跳过——线协议签名
# （Ed25519，§8.1）是协议存在前提，无降级形态。
# ============================================================================

# 基础 target 来自 falcon-daemon 包（依赖 FALCON_BUILD_DAEMON=ON）。
# 此文件仅在根 CMakeLists 的 FALCON_BUILD_DAEMON AND FALCON_BUILD_SWARMD
# 守卫内被 add_subdirectory，但双保险防御显式包含本目录的场景。
if(NOT TARGET falcon_ws_protocol OR NOT TARGET falcon_daemon_core)
    message(WARNING
        "falcon-swarmd requires targets falcon_ws_protocol / falcon_daemon_core "
        "from the falcon-daemon package (FALCON_BUILD_DAEMON=ON): skipped")
    return()
endif()

# Ed25519（EVP 便携 API）+ SHA-256：签名与指纹的硬依赖
find_package(OpenSSL QUIET)
if(NOT TARGET OpenSSL::Crypto)
    message(WARNING
        "OpenSSL not found: falcon-swarmd targets will be skipped "
        "(swarm wire protocol requires Ed25519 signing, no fallback)")
    return()
endif()

find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(WARNING
        "nlohmann_json not found: falcon-swarmd targets will be skipped")
    return()
endif()

message(STATUS "falcon-swarmd targets enabled")

# ============================================================================
# 公共层：线协议常量/编码定案 + Ed25519 密码学（§7-§9 单一事实源）
# ============================================================================
add_library(falcon_swarm_common STATIC
    src/common/swarm_crypto.cpp
    src/common/swarm_crypto.hpp
    src/common/swarm_protocol.cpp
    src/common/swarm_protocol.hpp
)

target_link_libraries(falcon_swarm_common
    PUBLIC
        Falcon::core
        nlohmann_json::nlohmann_json
        OpenSSL::Crypto
)

# NOMINMAX：windows.h 的 min/max 函数式宏污染 std::min/std::max/numeric_limits
# （批 2 CI Qt6 Windows 红面 C2589——main.cpp numeric_limits::max()）；PUBLIC
# 传递让同包消费 target 一并免疫。
target_compile_definitions(falcon_swarm_common PUBLIC NOMINMAX)

target_include_directories(falcon_swarm_common
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_swarm_common PRIVATE cxx_std_17)

# ============================================================================
# 服务端：状态/handlers/限频/传输层（阶段 0 server 全部面）
# ============================================================================
add_library(falcon_swarm_server STATIC
    src/server/swarm_rate_limiter.cpp
    src/server/swarm_rate_limiter.hpp
    src/server/swarm_rpc_handlers.cpp
    src/server/swarm_rpc_handlers.hpp
    src/server/swarm_rpc_server.cpp
    src/server/swarm_rpc_server.hpp
    src/server/swarm_server_state.cpp
    src/server/swarm_server_state.hpp
    # 配置加载编进 server 库（可执行已链 server）——单测可链接直达
    src/swarmd_config.cpp
    src/swarmd_config.hpp
)

# falcon_ws_protocol：RFC 6455 帧协议层（daemon 包拆出的共享库）
# falcon_daemon_core：swarmd_config 的 get_default_config_dir（PRIVATE——
# 仅实现符号依赖，不进公共头）
target_link_libraries(falcon_swarm_server
    PUBLIC
        falcon_swarm_common
        falcon_ws_protocol
    PRIVATE
        falcon_daemon_core
)

target_include_directories(falcon_swarm_server
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(falcon_swarm_server PRIVATE cxx_std_17)

# ============================================================================
# 客户端：节点侧 SwarmClient（注册/心跳/查询/退订 + WS 收订）
# HTTP 通道走 libcurl；CURL 缺席只跳过 client（server + 二进制照建）。
# ============================================================================
find_package(CURL QUIET)
if(TARGET CURL::libcurl)
    add_library(falcon_swarm_client STATIC
        src/client/swarm_client.cpp
        src/client/swarm_client.hpp
        src/client/swarm_http_client.cpp
        src/client/swarm_http_client.hpp
        src/client/swarm_key_store.cpp
        src/client/swarm_key_store.hpp
        src/client/swarm_ws_subscriber.cpp
        src/client/swarm_ws_subscriber.hpp
    )

    target_link_libraries(falcon_swarm_client
        PUBLIC
            falcon_swarm_common
        PRIVATE
            falcon_ws_protocol
            CURL::libcurl
    )

    target_compile_features(falcon_swarm_client PRIVATE cxx_std_17)
else()
    message(STATUS "CURL not found: falcon_swarm_client skipped "
                   "(server + falcon-swarmd binary still built)")
endif()

# ============================================================================
# 可执行：falcon-swarmd（配置 + main；链 server 与 daemon 包拆出的 core）
# ============================================================================
add_executable(falcon-swarmd
    src/main.cpp
)

target_link_libraries(falcon-swarmd
    PRIVATE
        falcon_swarm_server
        falcon_daemon_core
)

target_compile_features(falcon-swarmd PRIVATE cxx_std_17)

# ============================================================================
# 安装（daemon 包同形制：GNUInstallDirs BINDIR）
# ============================================================================
include(GNUInstallDirs)

install(TARGETS falcon-swarmd
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# ============================================================================
# Tests（GTest 在位才编；与 daemon 包同守卫形制）
# ============================================================================
if(FALCON_BUILD_TESTS)
    add_subdirectory(tests)
endif()
