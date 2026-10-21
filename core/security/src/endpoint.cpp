#include "ss/security/endpoint.hpp"

#include <cctype>

namespace ss::security {
namespace {

Error bad(const char* why) { return makeError(ErrorCategory::InvalidArgument, std::string("endpoint rejected: ") + why); }

std::string lowerAscii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool validHost(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (h.front() == '[') {   // bracketed IPv6 literal
        if (h.back() != ']' || h.size() < 4) return false;
        for (std::size_t i = 1; i + 1 < h.size(); ++i) {
            const char c = h[i];
            if (!(std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.')) return false;
        }
        return true;
    }
    std::size_t labelLen = 0;
    char prev = '.';
    for (char c : h) {
        if (c == '.') {
            if (labelLen == 0) return false;
            labelLen = 0; prev = c; continue;
        }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) return false;
        if (prev == '.' && c == '-') return false;
        if (++labelLen > 63) return false;
        prev = c;
    }
    return labelLen > 0 && prev != '-';
}

bool parseIPv4(const std::string& s, unsigned out[4]) {
    std::size_t i = 0;
    for (int part = 0; part < 4; ++part) {
        if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) return false;
        unsigned v = 0; std::size_t digits = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            v = v * 10 + static_cast<unsigned>(s[i] - '0');
            if (++digits > 3 || v > 255) return false;
            ++i;
        }
        if (digits > 1 && s[i - digits] == '0') return false;   // no leading zeros (octal ambiguity)
        out[part] = v;
        if (part < 3) { if (i >= s.size() || s[i] != '.') return false; ++i; }
    }
    return i == s.size();
}

bool isLoopbackHost(const std::string& h) {
    if (h == "localhost" || h == "[::1]") return true;
    unsigned o[4];
    return parseIPv4(h, o) && o[0] == 127;
}

}  // namespace

std::string Endpoint::label() const {
    std::string s = scheme + "://" + host + ":" + std::to_string(port);
    if (!app.empty()) s += "/" + app;
    return s;
}

bool isProtocolAllowed(Context ctx, const std::string& scheme, const EndpointPolicy& policy) {
    if (ctx == Context::LocalContent) return false;
    const std::string s = lowerAscii(scheme);
    return s == "rtmps" || s == "srt" || (s == "rtmp" && policy.allowPlainRtmp);
}

bool looksLikeUrlOrUri(const std::string& s) {
    if (s.find("://") != std::string::npos) return true;
    if (s.compare(0, 2, "//") == 0 || s.compare(0, 2, "\\\\") == 0) return true;
    // Any colon beyond a single drive-letter prefix ("data:", "concat:", "subfile,,start,0:") is an ffmpeg-style pseudo-URL.
    const std::size_t colon = s.find(':');
    if (colon != std::string::npos && colon > 1) return true;
    return false;
}

Result<Endpoint> parseEndpoint(const std::string& url, Context ctx, const EndpointPolicy& policy) {
    if (ctx == Context::LocalContent) return bad("network protocols are not allowed for local content");
    if (url.empty() || url.size() > 512) return bad("empty or too long");
    for (char c : url) if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F) return bad("illegal character");
    const std::size_t sep = url.find("://");
    if (sep == std::string::npos || sep == 0) return bad("missing scheme");
    Endpoint ep;
    ep.scheme = lowerAscii(url.substr(0, sep));
    if (!isProtocolAllowed(ctx, ep.scheme, policy)) return bad("protocol not allowed");
    std::string rest = url.substr(sep + 3);
    if (rest.find_first_of("?#") != std::string::npos) return bad("query or fragment not allowed; use a secret reference");
    const std::size_t aEnd = rest.find('/');
    std::string authority = rest.substr(0, aEnd);
    const std::string path = aEnd == std::string::npos ? std::string() : rest.substr(aEnd + 1);
    if (authority.find('@') != std::string::npos) return bad("userinfo not allowed; use a secret reference");
    std::string host = authority;
    unsigned long port = 0;
    bool havePort = false;
    std::size_t colon = std::string::npos;
    if (!host.empty() && host.front() == '[') {
        const std::size_t close = host.find(']');
        if (close == std::string::npos) return bad("bad host");
        if (close + 1 < host.size()) { if (host[close + 1] != ':') return bad("bad host"); colon = close + 1; }
    } else {
        colon = host.rfind(':');
    }
    if (colon != std::string::npos) {
        const std::string ps = host.substr(colon + 1);
        host = host.substr(0, colon);
        if (ps.empty() || ps.size() > 5) return bad("bad port");
        for (char c : ps) if (!std::isdigit(static_cast<unsigned char>(c))) return bad("bad port");
        port = std::stoul(ps);
        havePort = true;
    }
    host = lowerAscii(host);
    if (!validHost(host)) return bad("bad host");
    if (!policy.allowLoopback && isLoopbackHost(host)) return bad("loopback not allowed");
    if (!havePort) {
        if (ep.scheme == "rtmps") port = 443;
        else if (ep.scheme == "rtmp") port = 1935;
        else return bad("port required");
    }
    if (port < 1 || port > 65535) return bad("bad port");
    ep.host = host;
    ep.port = static_cast<std::uint16_t>(port);
    if (ep.scheme == "srt") {
        if (!path.empty()) return bad("path not allowed for srt");
    } else {
        std::string app = path;
        if (!app.empty() && app.back() == '/') app.pop_back();
        if (app.find('/') != std::string::npos) return bad("stream key in path; use a secret reference");
        for (char c : app) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return bad("bad app name");
        if (app.size() > 64) return bad("bad app name");
        ep.app = app;
    }
    return ep;
}

Status validateLightingDestination(const std::string& ip) {
    unsigned o[4];
    if (!parseIPv4(ip, o)) return makeError(ErrorCategory::InvalidArgument, "lighting destination must be an IPv4 address");
    if (o[0] == 0) return makeError(ErrorCategory::InvalidArgument, "lighting destination is unspecified");
    if (o[0] == 255 && o[1] == 255 && o[2] == 255 && o[3] == 255)
        return makeError(ErrorCategory::InvalidArgument, "limited broadcast is not a unicast destination");
    return okStatus();
}

std::vector<std::string> enabledProtocolList(const EndpointPolicy& policy) {
    std::vector<std::string> v{"rtmps", "srt", "udp (Art-Net, sACN; lighting transport)"};
    if (policy.allowPlainRtmp) v.insert(v.begin() + 1, "rtmp");
    return v;
}

}  // namespace ss::security
