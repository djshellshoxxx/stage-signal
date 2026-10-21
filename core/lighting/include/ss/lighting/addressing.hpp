// Universe numbering and endpoints. Art-Net and sACN universes are DIFFERENT TYPES on purpose:
// they have different ranges and different zero/one-based conventions and are never interchangeable.
//
//   Art-Net Port-Address : 15 bit, wire value 0..32767, = Net(7) << 8 | SubNet(4) << 4 | Universe(4).
//                          Wire numbering is ZERO based. "User one-based" N maps to wire N-1.
//   sACN universe        : 16 bit, 1..63999, ONE based on the wire. There is no zero-based form.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "ss/base/error.hpp"
#include "ss/lighting/limits.hpp"

namespace ss::lighting {

struct ArtNetPortAddress {
    std::uint16_t wire = 0;                       // 0..32767, exactly what goes on the wire (SubUni | Net << 8)
    std::uint8_t net() const { return static_cast<std::uint8_t>((wire >> 8) & 0x7F); }
    std::uint8_t subNet() const { return static_cast<std::uint8_t>((wire >> 4) & 0x0F); }
    std::uint8_t universe() const { return static_cast<std::uint8_t>(wire & 0x0F); }
    std::uint8_t subUni() const { return static_cast<std::uint8_t>(wire & 0xFF); }
    friend bool operator==(ArtNetPortAddress a, ArtNetPortAddress b) { return a.wire == b.wire; }
    friend bool operator!=(ArtNetPortAddress a, ArtNetPortAddress b) { return a.wire != b.wire; }
};
struct SacnUniverse {
    std::uint16_t value = 1;                      // 1..63999, as on the wire
    friend bool operator==(SacnUniverse a, SacnUniverse b) { return a.value == b.value; }
    friend bool operator!=(SacnUniverse a, SacnUniverse b) { return a.value != b.value; }
};

inline Result<ArtNetPortAddress> artnetFromWire(std::uint32_t wire) {
    if (wire > kArtNetMaxPortAddress)
        return makeError(ErrorCategory::InvalidArgument, "Art-Net Port-Address out of range 0..32767");
    return ArtNetPortAddress{static_cast<std::uint16_t>(wire)};
}
inline Result<ArtNetPortAddress> artnetFromNetSubUni(unsigned net, unsigned sub, unsigned uni) {
    if (net > 127 || sub > 15 || uni > 15)
        return makeError(ErrorCategory::InvalidArgument, "Art-Net Net 0..127, SubNet 0..15, Universe 0..15");
    return ArtNetPortAddress{static_cast<std::uint16_t>((net << 8) | (sub << 4) | uni)};
}
// Operator-facing "universe 1" style numbering. Explicit conversion; wire = userOneBased - 1.
inline Result<ArtNetPortAddress> artnetFromUserOneBased(std::uint32_t userOneBased) {
    if (userOneBased < 1 || userOneBased > 32768u)
        return makeError(ErrorCategory::InvalidArgument, "Art-Net one-based universe out of range 1..32768");
    return ArtNetPortAddress{static_cast<std::uint16_t>(userOneBased - 1)};
}
inline std::uint32_t artnetToUserOneBased(ArtNetPortAddress a) { return static_cast<std::uint32_t>(a.wire) + 1u; }

inline Result<SacnUniverse> sacnFromNumber(std::uint32_t n) {
    if (n < kSacnMinUniverse || n > kSacnMaxUniverse)
        return makeError(ErrorCategory::InvalidArgument, "sACN universe out of range 1..63999");
    return SacnUniverse{static_cast<std::uint16_t>(n)};
}

// Label that always states the convention, for UI and diagnostics.
inline std::string artnetLabel(ArtNetPortAddress a) {
    return "Art-Net " + std::to_string(a.net()) + ":" + std::to_string(a.subNet()) + ":" + std::to_string(a.universe()) +
           " (wire Port-Address " + std::to_string(a.wire) + ", zero-based; one-based " +
           std::to_string(artnetToUserOneBased(a)) + ")";
}
inline std::string sacnLabel(SacnUniverse u) {
    return "sACN universe " + std::to_string(u.value) + " (wire value " + std::to_string(u.value) + ", one-based)";
}

// ---- Endpoints -------------------------------------------------------------------------------
struct Ipv4 {
    std::array<std::uint8_t, 4> b{{0, 0, 0, 0}};
    bool isZero() const { return b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0; }
    friend bool operator==(const Ipv4& x, const Ipv4& y) { return x.b == y.b; }
    friend bool operator!=(const Ipv4& x, const Ipv4& y) { return x.b != y.b; }
    std::string toString() const {
        return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." + std::to_string(b[3]);
    }
};
// Strict dotted-quad parse (4 decimal parts, 0..255, no leading junk). Empty string -> error.
inline Result<Ipv4> parseIpv4(const std::string& s) {
    Ipv4 out; std::size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        if (i >= s.size() || s[i] < '0' || s[i] > '9') return makeError(ErrorCategory::InvalidArgument, "bad IPv4 address");
        unsigned v = 0; int digits = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') { v = v * 10 + static_cast<unsigned>(s[i] - '0'); ++i; if (++digits > 3) break; }
        if (digits > 3 || v > 255) return makeError(ErrorCategory::InvalidArgument, "bad IPv4 address");
        out.b[static_cast<std::size_t>(part)] = static_cast<std::uint8_t>(v);
        if (part < 3) { if (i >= s.size() || s[i] != '.') return makeError(ErrorCategory::InvalidArgument, "bad IPv4 address"); ++i; }
    }
    if (i != s.size()) return makeError(ErrorCategory::InvalidArgument, "bad IPv4 address");
    return out;
}
struct Endpoint {
    Ipv4 ip;
    std::uint16_t port = 0;
    friend bool operator==(const Endpoint& x, const Endpoint& y) { return x.ip == y.ip && x.port == y.port; }
};

// E1.31 multicast group for a universe: 239.255.<hi>.<lo>, UDP 5568.
inline Endpoint sacnMulticastEndpoint(SacnUniverse u) {
    Endpoint e; e.ip.b = {{239, 255, static_cast<std::uint8_t>(u.value >> 8), static_cast<std::uint8_t>(u.value & 0xFF)}};
    e.port = kSacnPort; return e;
}

}  // namespace ss::lighting
