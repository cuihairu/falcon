/**
 * @file abi_host_setup.cpp
 * @brief 测试进程启动时注入稳定 C ABI 宿主服务表
 *
 * 生产由桌面端 main 注入；测试用 gtest_main 无自定义入口，
 * 故用静态初始化完成同样的注入，保证插件能拿到 HTTP 与分配器。
 */
#include <falcon/drives/plugin_loader.hpp>
#include <falcon/protocols/host_api.hpp>

namespace {

const bool kHostApiInstalled = [] {
    falcon::drives::PluginLoader::set_host_api(falcon::protocols::host_api());
    return true;
}();

} // namespace
