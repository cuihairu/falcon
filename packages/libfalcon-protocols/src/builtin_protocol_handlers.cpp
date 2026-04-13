#include <falcon/plugin_manager.hpp>

#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
#include "../plugins/http/http_handler.hpp"
#endif
#ifdef FALCON_ENABLE_FTP_PLUGIN
#include "../plugins/ftp/ftp_handler.hpp"
#endif

namespace falcon {

void register_builtin_protocol_handlers(PluginManager& manager) {
#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
    manager.registerPlugin(plugins::create_http_handler());
#endif

#ifdef FALCON_ENABLE_FTP_PLUGIN
    manager.registerPlugin(plugins::create_ftp_handler());
#endif
}

} // namespace falcon
