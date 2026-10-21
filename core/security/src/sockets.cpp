#include "ss/security/sockets.hpp"

namespace ss::security {

std::vector<DeclaredSocket> defaultSocketDeclaration() {
    using D = SocketDirection;
    return {
        {"lighting.artnet.out", "udp", D::Outbound, BindScope::None, 6454, false, "Art-Net DMX output to the configured node (opt-in, armed)"},
        {"lighting.sacn.out", "udp", D::Outbound, BindScope::None, 5568, false, "sACN DMX output to the configured destination (opt-in, armed)"},
        {"broadcast.rtmps.out", "tcp", D::Outbound, BindScope::None, 443, false, "RTMPS publish to the configured endpoint (opt-in, visible Live state)"},
        {"broadcast.srt.out", "udp", D::Outbound, BindScope::None, 0, false, "SRT publish to the configured receiver (opt-in, visible Live state)"},
    };
}

std::vector<std::string> auditSockets(const std::vector<ObservedSocket>& observed, bool defaultConfiguration) {
    std::vector<std::string> v;
    for (const auto& s : observed) {
        if (!s.listening) continue;
        const std::string what = s.transport + " listener on port " + std::to_string(s.localPort);
        if (s.scope == BindScope::AnyInterface) v.push_back(what + " is bound to all interfaces");
        else if (defaultConfiguration) v.push_back(what + " is not declared for the default configuration");
    }
    return v;
}

}  // namespace ss::security
