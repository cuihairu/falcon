/**
 * @file host_api.hpp
 * @brief 协议层提供的宿主服务表（给第三方网盘插件使用）
 *
 * falcon_protocols 已经 PUBLIC 链接 curl；这里把同步 HTTP 能力封装成
 * 稳定的 falcon_host_api，供 PluginLoader 在加载插件前注入。
 *
 * 生命周期：返回的指针指向函数内 static 对象，进程级有效。
 */
#pragma once

#include <falcon/abi/drive_api.h>

namespace falcon {
namespace protocols {

/// 返回协议层宿主服务表的只读指针（进程级静态，永不为空）。
const falcon_host_api* host_api();

} // namespace protocols
} // namespace falcon
