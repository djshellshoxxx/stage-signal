#include "ss/broadcast/ffmpeg_io.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include "ss/broadcast/redact.hpp"

namespace ss::broadcast {

const char* failureName(FailureKind k) {
    switch (k) {
        case FailureKind::None: return "none";
        case FailureKind::Certificate: return "certificate";
        case FailureKind::Auth: return "authentication";
        case FailureKind::InvalidConfig: return "invalid configuration";
        case FailureKind::Dns: return "host not found";
        case FailureKind::Refused: return "connection refused";
        case FailureKind::Timeout: return "timeout";
        case FailureKind::ConnectionLost: return "connection lost";
        case FailureKind::Stalled: return "stalled";
        case FailureKind::Unknown: return "unknown error";
    }
    return "?";
}
bool isTerminal(FailureKind k) {
    return k == FailureKind::Certificate || k == FailureKind::Auth || k == FailureKind::InvalidConfig;
}

namespace {
std::string lower(const std::string& s) {
    std::string o = s;
    std::transform(o.begin(), o.end(), o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return o;
}
bool has(const std::string& t, std::initializer_list<const char*> needles) {
    for (const char* n : needles) if (t.find(n) != std::string::npos) return true;
    return false;
}
std::string firstInterestingLine(const std::string& redacted) {
    std::size_t pos = 0;
    std::string best;
    while (pos < redacted.size()) {
        std::size_t e = redacted.find('\n', pos);
        if (e == std::string::npos) e = redacted.size();
        std::string line = redacted.substr(pos, e - pos);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) { if (best.empty()) best = line; const std::string l = lower(line); if (l.find("error") != std::string::npos || l.find("fail") != std::string::npos || l.find("refused") != std::string::npos) return line.substr(0, 300); }
        pos = e + 1;
    }
    return best.substr(0, 300);
}
}  // namespace

Failure classifyStderr(const std::string& text) {
    Failure f;
    if (text.empty()) return f;
    const std::string t = lower(text);
    f.message = firstInterestingLine(redactText(text));
    if (has(t, {"peer certificate failed verification", "certificate verify failed", "certificate has expired", "hostname mismatch",
                "unable to get local issuer", "self signed certificate", "self-signed certificate", "certificate is not yet valid"}))
        f.kind = FailureKind::Certificate;
    else if (has(t, {"unrecognized option", "option not found", "unknown encoder", "encoder not found", "not compatible with flv",
                     "not compatible with mpegts", "error splitting the argument list", "unable to find a suitable output format",
                     "protocol not found", "invalid passphrase", "incorrect codec parameters", "invalid argument"}))
        f.kind = FailureKind::InvalidConfig;
    else if (has(t, {"netstream.publish.badname", "netstream.publish.denied", "401 unauthorized", "403 forbidden", "server returned 401",
                     "server returned 403", "authentication failed", "not authorized", "unauthorized", "access denied", "invalid stream key",
                     "rejected the stream key"}))
        f.kind = FailureKind::Auth;
    else if (has(t, {"failed to resolve hostname", "name or service not known", "temporary failure in name resolution", "no address associated"}))
        f.kind = FailureKind::Dns;
    else if (has(t, {"connection refused"})) f.kind = FailureKind::Refused;
    else if (has(t, {"timed out", "timeout"})) f.kind = FailureKind::Timeout;
    else if (has(t, {"broken pipe", "connection reset", "end of file", "input/output error", "cannot read rtmp handshake", "handshake",
                     "cannot open connection", "error writing trailer", "error muxing a packet"}))
        f.kind = FailureKind::ConnectionLost;
    else if (has(t, {"error", "failed"})) f.kind = FailureKind::Unknown;
    return f;
}

Failure classifyExit(int code, const std::string& text) {
    Failure f = classifyStderr(text);
    if (f.kind != FailureKind::None) return f;
    if (code == 8) { f.kind = FailureKind::InvalidConfig; f.message = "ffmpeg rejected an option (exit code 8)"; return f; }
    if (code != 0) { f.kind = FailureKind::Unknown; f.message = "ffmpeg exited with code " + std::to_string(code); }
    return f;
}

std::vector<ProgressSample> ProgressParser::feed(const std::string& chunk) {
    std::vector<ProgressSample> done;
    pending_ += chunk;
    if (pending_.size() > 65536) pending_.erase(0, pending_.size() - 65536);   // bounded even for a garbage stream
    std::size_t pos = 0, nl;
    while ((nl = pending_.find('\n', pos)) != std::string::npos) {
        std::string line = pending_.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq);
        std::string v = line.substr(eq + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        auto u64 = [&](std::uint64_t& out) { if (!v.empty() && std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); })) out = std::strtoull(v.c_str(), nullptr, 10); };
        if (k == "frame") u64(cur_.frame);
        else if (k == "total_size") u64(cur_.totalSize);
        else if (k == "drop_frames") u64(cur_.dropFrames);
        else if (k == "dup_frames") u64(cur_.dupFrames);
        else if (k == "out_time_us") {
            if (!v.empty() && std::all_of(v.begin(), v.end(), [](unsigned char c) { return std::isdigit(c); })) cur_.outTime = static_cast<Ns>(std::strtoull(v.c_str(), nullptr, 10)) * 1000;
        } else if (k == "bitrate") {
            if (v != "N/A" && !v.empty()) cur_.bitrateKbps = std::atof(v.c_str());
        } else if (k == "progress") {
            cur_.end = (v == "end");
            done.push_back(cur_);
            cur_ = ProgressSample{};
        }
    }
    pending_.erase(0, pos);
    return done;
}

}  // namespace ss::broadcast
