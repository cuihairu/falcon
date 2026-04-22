#include <falcon/protocol_registry.hpp>

#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
#include "../plugins/http/http_handler.hpp"
#endif
#ifdef FALCON_ENABLE_FTP_PLUGIN
#include "../plugins/ftp/ftp_handler.hpp"
#endif

namespace falcon {

void register_builtin_protocol_handlers(ProtocolRegistry& registry) {
#if defined(FALCON_ENABLE_HTTP_PLUGIN) || defined(FALCON_ENABLE_HTTP)
    registry.register_handler(protocols::create_http_handler());
#endif

#ifdef FALCON_ENABLE_FTP_PLUGIN
    registry.register_handler(protocols::create_ftp_handler());
#endif
}

} // namespace falcon
