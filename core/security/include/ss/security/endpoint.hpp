// StageSignal security: protocol / endpoint allowlist (spec section 16: "Decoder allowlist excludes network
// protocols for local content; stream adapter separately owns enabled network transports").
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::security {

enum class Context : std::uint8_t {
    LocalContent,   // decoding show media: no network protocol is ever allowed
    Stream          // broadcast adapter: rtmps and srt (plain rtmp only when explicitly enabled)
};

struct EndpointPolicy {
    bool allowPlainRtmp = false;   // plaintext RTMP must be an explicit, visible opt-in
    bool allowLoopback = true;     // local test receivers
};

struct Endpoint {
    std::string scheme;   // lower-case: "rtmps", "rtmp", "srt"
    std::string host;     // lower-case host name, IPv4 literal or bracketed IPv6
    std::uint16_t port = 0;
    std::string app;      // first path segment for rtmp(s) ("live2"), empty otherwise
    // Stable, secret-free label for UI and logs, e.g. "rtmps://host:443/live2".
    std::string label() const;
};

bool isProtocolAllowed(Context ctx, const std::string& scheme, const EndpointPolicy& policy = EndpointPolicy{});

// Parses and validates a broadcast endpoint. Rejects: other schemes, userinfo, query, fragment, stream keys in the
// path (keys must be secret references), invalid hosts, ports outside 1..65535, control characters and over-long input.
// LocalContent always fails: show media may not supply a network URL.
Result<Endpoint> parseEndpoint(const std::string& url, Context ctx, const EndpointPolicy& policy = EndpointPolicy{});

// True when the string carries a URL scheme ("x://"), a data: URI or a network-path prefix. Used to refuse
// media references that try to smuggle a URL.
bool looksLikeUrlOrUri(const std::string& s);

// Lighting destination: strict dotted-quad IPv4, not 0.0.0.0, not the limited broadcast address.
Status validateLightingDestination(const std::string& ip);

// Protocol allowlist exported for the dependency / protocol inventory (section 16).
std::vector<std::string> enabledProtocolList(const EndpointPolicy& policy = EndpointPolicy{});

}  // namespace ss::security
