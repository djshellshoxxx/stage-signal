#include "ss/broadcast/settings.hpp"
#include <algorithm>
#include <cctype>
#include "nlohmann/json.hpp"
#include "ss/base/sha256.hpp"

namespace ss::broadcast {
using json = nlohmann::json;

const char* profileName(Profile p) {
    switch (p) {
        case Profile::Rtmps: return "RTMPS";
        case Profile::Rtmp: return "RTMP";
        case Profile::SrtCaller: return "SRT (caller)";
        case Profile::Rtp: return "RTP";
        case Profile::WebRtc: return "WebRTC";
    }
    return "?";
}
Availability availability(Profile p) {
    switch (p) {
        case Profile::Rtmps: case Profile::SrtCaller: return Availability::Beta;
        case Profile::Rtmp: return Availability::Optional;
        case Profile::Rtp: return Availability::Later;
        case Profile::WebRtc: return Availability::Deferred;
    }
    return Availability::Deferred;
}
const char* availabilityNote(Profile p) {
    switch (p) {
        case Profile::Rtmps: return "Beta. H.264/AAC receiver profile; TLS certificate validation is mandatory.";
        case Profile::Rtmp: return "Optional. Unencrypted; only for an intentionally selected trusted or local link.";
        case Profile::SrtCaller: return "Beta. Caller mode with MPEG-TS; encryption is optional and always shown.";
        case Profile::Rtp: return "Later. Needs a receiver SDP/codec profile; RTP/RTCP without SRTP is unencrypted.";
        case Profile::WebRtc: return "Deferred. Signalling, NAT traversal and congestion control are out of scope for the beta.";
    }
    return "";
}

bool SecretRef::wellFormed() const {
    if (handle.size() < 5 || handle.size() > 100 || handle.compare(0, 4, "ref:") != 0) return false;
    return std::all_of(handle.begin() + 4, handle.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-'; });
}

BroadcastSettings defaultSettings() { return BroadcastSettings{}; }

std::uint32_t effectivePort(const BroadcastSettings& s) {
    if (s.port != 0) return s.port;
    return s.profile == Profile::Rtmps ? 443 : (s.profile == Profile::Rtmp ? 1935 : 0);
}

EncryptionState encryptionState(const BroadcastSettings& s) {
    switch (s.profile) {
        case Profile::Rtmps: return EncryptionState::TlsVerified;
        case Profile::SrtCaller: return s.srtPassphrase.empty() ? EncryptionState::Unencrypted : EncryptionState::SrtEncrypted;
        default: return EncryptionState::Unencrypted;
    }
}
std::string encryptionLabel(const BroadcastSettings& s) {
    switch (s.profile) {
        case Profile::Rtmps: return "RTMPS: encrypted (TLS, certificate and host name verified)";
        case Profile::Rtmp: return "RTMP: NOT encrypted (the stream key and media can be read on the network)";
        case Profile::SrtCaller:
            return s.srtPassphrase.empty() ? "SRT: NOT encrypted (no passphrase set)"
                                           : "SRT: encrypted (AES-" + std::to_string(s.srtKeyLenBytes * 8) + ")";
        case Profile::Rtp: return "RTP: NOT encrypted";
        case Profile::WebRtc: return "WebRTC: not available";
    }
    return "";
}

namespace {
bool validHost(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (h.front() == '[') {                                               // [IPv6]
        if (h.back() != ']' || h.size() < 4) return false;
        return std::all_of(h.begin() + 1, h.end() - 1, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) || c == ':' || c == '.'; });
    }
    if (h.front() == '.' || h.back() == '.' || h.front() == '-' || h.back() == '-') return false;
    std::size_t label = 0;
    for (char c : h) {
        if (c == '.') { if (label == 0) return false; label = 0; continue; }
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) return false;
        if (++label > 63) return false;
    }
    return label > 0;
}
bool validPath(const std::string& p) {
    if (p.empty()) return true;
    if (p.size() > 128 || p.front() == '/' || p.back() == '/') return false;
    std::size_t start = 0;
    while (start <= p.size()) {
        std::size_t e = p.find('/', start);
        if (e == std::string::npos) e = p.size();
        const std::string seg = p.substr(start, e - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        start = e + 1;
    }
    return std::all_of(p.begin(), p.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '~' || c == '-' || c == '/'; });
}
void add(std::vector<SettingIssue>& v, const char* f, std::string m, bool blocking = true) { v.push_back({f, std::move(m), blocking}); }
}  // namespace

bool hasBlocking(const std::vector<SettingIssue>& v) {
    return std::any_of(v.begin(), v.end(), [](const SettingIssue& i) { return i.blocking; });
}

std::vector<SettingIssue> validateSettings(const BroadcastSettings& s, bool baseTier) {
    std::vector<SettingIssue> v;
    if (availability(s.profile) == Availability::Later || availability(s.profile) == Availability::Deferred)
        add(v, "profile", std::string(profileName(s.profile)) + " is not available in this build. " + availabilityNote(s.profile));
    const bool rtmp = s.profile == Profile::Rtmps || s.profile == Profile::Rtmp;
    const bool srt = s.profile == Profile::SrtCaller;
    if (s.name.empty() || s.name.size() > 64) add(v, "name", "The endpoint needs a name of 1 to 64 characters.");
    if (!validHost(s.host)) add(v, "host", "Enter a host name or IP address only (no rtmp://, user name, path or query).");
    if (srt) { if (s.port < 1 || s.port > 65535) add(v, "port", "SRT needs a port between 1 and 65535."); }
    else if (s.port > 65535) add(v, "port", "Port must be between 1 and 65535.");
    if (rtmp) {
        if (s.path.empty()) add(v, "path", "An application path is required (for example \"live\"); without it the key would be read as the application.");
        else if (!validPath(s.path)) add(v, "path", "The application path may only contain letters, digits and . _ ~ - /, without a leading or trailing slash.");
        if (s.streamKey.empty()) add(v, "streamKey", "A stream key reference is required.");
        else if (!s.streamKey.wellFormed()) add(v, "streamKey", "Store the key in the secret store and use its reference (ref:...). Keys must never be typed into settings.");
    }
    if (s.profile == Profile::Rtmp && !s.acknowledgeUnencrypted)
        add(v, "acknowledgeUnencrypted", "RTMP is not encrypted. Confirm that this is a trusted or local link.");
    if (srt) {
        if (!s.srtPassphrase.empty() && !s.srtPassphrase.wellFormed())
            add(v, "srtPassphrase", "Store the passphrase in the secret store and use its reference (ref:...).");
        if (s.srtPassphrase.empty() && !s.acknowledgeUnencrypted)
            add(v, "acknowledgeUnencrypted", "SRT without a passphrase is not encrypted. Set a passphrase or confirm.");
        if (s.srtKeyLenBytes != 16 && s.srtKeyLenBytes != 24 && s.srtKeyLenBytes != 32) add(v, "srtKeyLenBytes", "SRT key length must be 16, 24 or 32 bytes.");
        if (s.srtLatencyMs < 20 || s.srtLatencyMs > 8000) add(v, "srtLatencyMs", "SRT latency must be 20 to 8000 ms.");
        if (s.srtConnectTimeoutMs < 500 || s.srtConnectTimeoutMs > 30000) add(v, "srtConnectTimeoutMs", "SRT connect timeout must be 500 to 30000 ms.");
    }
    const auto& vd = s.video;
    if (vd.width < 160 || vd.height < 120 || vd.width > 1920 || vd.height > 1080 || (vd.width & 1) || (vd.height & 1))
        add(v, "video.size", "Video size must be even and between 160x120 and 1920x1080.");
    if (vd.fps < 5 || vd.fps > 60) add(v, "video.fps", "Frame rate must be 5 to 60 FPS.");
    if (vd.bitrateKbps < 300 || vd.bitrateKbps > 20000) add(v, "video.bitrate", "Video bitrate must be 300 to 20000 kb/s.");
    if (vd.keyframeSeconds < 1 || vd.keyframeSeconds > 10) add(v, "video.keyframe", "Keyframe interval must be 1 to 10 seconds.");
    const auto& a = s.audio;
    if (a.rateHz != 44100 && a.rateHz != 48000) add(v, "audio.rate", "Audio rate must be 44100 or 48000 Hz.");
    if (a.channels < 1 || a.channels > 2) add(v, "audio.channels", "Audio must be mono or stereo.");
    if (a.bitrateKbps < 64 || a.bitrateKbps > 320) add(v, "audio.bitrate", "AAC bitrate must be 64 to 320 kb/s.");
    if (baseTier && (vd.width > 800 || vd.height > 600 || vd.fps > 25))
        add(v, "video.size", "BASE tier: use 800x600 or 640x480 at 15-25 FPS if the receiver accepts it.", false);
    return v;
}

Result<BroadcastSettings> applyBaseFallback(const BroadcastSettings& s, bool use640, std::uint32_t fps, bool accepts) {
    if (!accepts)
        return makeError(ErrorCategory::UnsupportedCapability, "The receiver has not been confirmed to accept the BASE fallback format.", Recoverability::OperatorAction);
    if (fps < 15 || fps > 25) return makeError(ErrorCategory::InvalidArgument, "BASE fallback frame rate is 15 to 25 FPS.", Recoverability::OperatorAction);
    BroadcastSettings o = s;
    o.video.width = use640 ? 640 : 800;
    o.video.height = use640 ? 480 : 600;
    o.video.fps = fps;
    o.video.bitrateKbps = std::min<std::uint32_t>(o.video.bitrateKbps, use640 ? 1200 : 1800);
    return o;
}

std::uint32_t requiredUploadKbps(const BroadcastSettings& s) {
    return static_cast<std::uint32_t>((s.video.bitrateKbps + s.audio.bitrateKbps) * 1.10 + 0.5);
}
std::string uploadWarning(const BroadcastSettings& s, std::uint32_t measured) {
    const auto need = requiredUploadKbps(s);
    if (measured >= need) return std::string();
    return "Measured upload (" + std::to_string(measured) + " kb/s) is below the stream bitrate (about " + std::to_string(need) +
           " kb/s). The stream will stutter or drop; lower the bitrate or use a faster connection. Recording is not affected.";
}

// ---- summary -----------------------------------------------------------------------------------------------------
PublicationSummary makeSummary(const BroadcastSettings& s, const PublicationFacts& f) {
    PublicationSummary p;
    p.endpointLabel = s.name + " (" + profileName(s.profile) + " to " + s.host + ")";
    p.encryption = encryptionLabel(s);
    p.video = std::to_string(s.video.width) + "x" + std::to_string(s.video.height) + " @ " + std::to_string(s.video.fps) + " FPS, " +
              std::to_string(s.video.bitrateKbps) + " kb/s H.264, keyframe every " + std::to_string(s.video.keyframeSeconds) + " s";
    p.audio = "AAC " + std::string(s.audio.channels == 1 ? "mono" : "stereo") + " " + std::to_string(s.audio.rateHz) + " Hz, " + std::to_string(s.audio.bitrateKbps) + " kb/s";
    p.audioSource = f.audioSourceLabel;
    p.programOutput = f.programOutputLabel;
    p.cameraPublished = f.cameraPublished;
    p.recordingActive = f.recordingActive;
    return p;
}
std::string PublicationSummary::text() const {
    return "Destination: " + endpointLabel + "\nProtection: " + encryption + "\nVideo: " + video + "\nAudio: " + audio +
           "\nAudio source: " + audioSource + "\nProgram output: " + programOutput +
           "\nCamera: " + (cameraPublished ? "INCLUDED in the stream" : "not included") +
           "\nLocal recording: " + (recordingActive ? "running (independent of the stream)" : "not running");
}
std::string PublicationSummary::hash() const { return Sha256::hashHex(text()); }

// ---- JSON --------------------------------------------------------------------------------------------------------
namespace {
json common(const BroadcastSettings& s) {
    json j;
    j["name"] = s.name; j["profile"] = static_cast<int>(s.profile); j["host"] = s.host; j["port"] = s.port; j["path"] = s.path;
    j["srtKeyLenBytes"] = s.srtKeyLenBytes; j["srtLatencyMs"] = s.srtLatencyMs; j["srtConnectTimeoutMs"] = s.srtConnectTimeoutMs;
    j["video"] = {{"width", s.video.width}, {"height", s.video.height}, {"fps", s.video.fps}, {"bitrateKbps", s.video.bitrateKbps}, {"keyframeSeconds", s.video.keyframeSeconds}};
    j["audio"] = {{"rateHz", s.audio.rateHz}, {"channels", s.audio.channels}, {"bitrateKbps", s.audio.bitrateKbps}};
    j["acknowledgeUnencrypted"] = s.acknowledgeUnencrypted;
    j["reconnectEnabled"] = s.reconnectEnabled;
    return j;
}
template <class T> T num(const json& j, const char* k, T def) { auto it = j.find(k); return (it != j.end() && it->is_number()) ? it->get<T>() : def; }
std::string str(const json& j, const char* k) { auto it = j.find(k); return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string(); }
}  // namespace

std::string exportSettingsJson(const BroadcastSettings& s) {
    json j = common(s);
    j["encryption"] = encryptionLabel(s);
    j["streamKeySet"] = !s.streamKey.empty();            // a flag only: neither the key nor its reference leaves the machine
    j["srtPassphraseSet"] = !s.srtPassphrase.empty();
    return j.dump(2);
}
std::string serializeSettingsJson(const BroadcastSettings& s) {
    json j = common(s);
    j["streamKeyRef"] = s.streamKey.handle;
    j["srtPassphraseRef"] = s.srtPassphrase.handle;
    return j.dump(2);
}
Result<BroadcastSettings> parseSettingsJson(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return makeError(ErrorCategory::InvalidArgument, "broadcast settings are not valid JSON");
    BroadcastSettings s;
    s.name = str(j, "name"); s.host = str(j, "host"); s.path = str(j, "path");
    const int p = num<int>(j, "profile", 0);
    if (p < 0 || p > 4) return makeError(ErrorCategory::SchemaUnsupported, "unknown broadcast profile");
    s.profile = static_cast<Profile>(p);
    s.port = num<std::uint32_t>(j, "port", 0);
    s.srtKeyLenBytes = num<std::uint32_t>(j, "srtKeyLenBytes", 16); s.srtLatencyMs = num<std::uint32_t>(j, "srtLatencyMs", 200);
    s.srtConnectTimeoutMs = num<std::uint32_t>(j, "srtConnectTimeoutMs", 3000);
    if (j.contains("video") && j["video"].is_object()) {
        const auto& v = j["video"];
        s.video.width = num<std::uint32_t>(v, "width", 1280); s.video.height = num<std::uint32_t>(v, "height", 720);
        s.video.fps = num<std::uint32_t>(v, "fps", 30); s.video.bitrateKbps = num<std::uint32_t>(v, "bitrateKbps", 2500);
        s.video.keyframeSeconds = num<std::uint32_t>(v, "keyframeSeconds", 2);
    }
    if (j.contains("audio") && j["audio"].is_object()) {
        const auto& a = j["audio"];
        s.audio.rateHz = num<std::uint32_t>(a, "rateHz", 48000); s.audio.channels = num<std::uint32_t>(a, "channels", 2);
        s.audio.bitrateKbps = num<std::uint32_t>(a, "bitrateKbps", 128);
    }
    s.acknowledgeUnencrypted = j.value("acknowledgeUnencrypted", false);
    s.reconnectEnabled = j.value("reconnectEnabled", true);
    s.streamKey.handle = str(j, "streamKeyRef"); s.srtPassphrase.handle = str(j, "srtPassphraseRef");
    return s;
}

}  // namespace ss::broadcast
