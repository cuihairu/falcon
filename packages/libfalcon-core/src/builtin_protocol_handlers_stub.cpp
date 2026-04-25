#include <falcon/protocol_registry.hpp>

namespace falcon {

// Weak stub: overridden by the strong definition in
// libfalcon-protocols/src/builtin_protocol_handlers.cpp when
// falcon_protocols is linked into the final binary.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void register_builtin_protocol_handlers(ProtocolRegistry&) {}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
std::vector<BuiltinProtocolInfo> describe_builtin_protocols() {
    return {};
}

} // namespace falcon
