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

message(STATUS "falcon-swarmd targets (batch 2/3): falcon_swarm_common, "
               "falcon_swarm_server, falcon_swarm_client, falcon-swarmd")
