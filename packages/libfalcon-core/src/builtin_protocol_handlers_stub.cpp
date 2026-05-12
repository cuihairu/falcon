#include <falcon/protocol_registry.hpp>

namespace falcon {

// Weak stub: overridden by the strong definition in
// libfalcon-protocols/src/builtin_protocol_handlers.cpp when
// falcon_protocols is linked into the final binary.
//
// GCC/Clang support weak symbols via __attribute__((weak))
// MSVC does not support weak symbols in the same way.
#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
void register_builtin_protocol_handlers(ProtocolRegistry&) {
    // Empty stub - will be overridden by the real implementation
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((weak))
#endif
std::vector<BuiltinProtocolInfo> describe_builtin_protocols() {
    return {};
}

} // namespace falcon
