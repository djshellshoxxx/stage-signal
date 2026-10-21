// StageSignal security: declared sockets (REQ-X04). In the default configuration StageSignal opens no listening
// socket and no outbound connection. Everything the product can ever open is listed here; the T-SEC port-scan
// test compares the process's observed sockets against this declaration.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ss::security {

enum class SocketDirection : std::uint8_t { Outbound, InboundListener };
enum class BindScope : std::uint8_t { None, Loopback, AnyInterface };

struct DeclaredSocket {
    std::string id;
    std::string transport;          // "udp" | "tcp"
    SocketDirection direction = SocketDirection::Outbound;
    BindScope scope = BindScope::None;
    std::uint16_t port = 0;         // 0 = chosen by user configuration
    bool enabledByDefault = false;
    std::string purpose;
};

struct ObservedSocket {
    std::string transport;
    std::uint16_t localPort = 0;
    bool listening = false;
    BindScope scope = BindScope::None;
};

// All sockets the product may open. None is enabled by default and none is an inbound listener.
std::vector<DeclaredSocket> defaultSocketDeclaration();

// Returns human-readable violations (empty = pass). Default-configuration audit: any listening socket is a
// violation; a listener bound to AnyInterface is a violation in every configuration.
std::vector<std::string> auditSockets(const std::vector<ObservedSocket>& observed, bool defaultConfiguration = true);

}  // namespace ss::security
