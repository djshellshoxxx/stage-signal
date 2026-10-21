// StageSignal broadcast (MOD-12 session logic): profiles, settings, validation and the pre-start publication summary
// (spec section 15). Secrets are never held here: settings carry only opaque references (SecretRef) that the
// security/app layer resolves at the last moment (ISecretResolver). No I/O, no network, no ffmpeg calls.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ss/base/error.hpp"

namespace ss::broadcast {

enum class Profile : std::uint8_t { Rtmps, Rtmp, SrtCaller, Rtp, WebRtc };
enum class Availability : std::uint8_t { Beta, Optional, Later, Deferred };
enum class EncryptionState : std::uint8_t {
    TlsVerified,        // RTMPS: TLS with mandatory certificate and host validation
    SrtEncrypted,       // SRT with passphrase (AES-128/192/256)
    Unencrypted         // RTMP, SRT without passphrase, RTP: visibly labelled as such
};

const char* profileName(Profile p);
Availability availability(Profile p);                    // Rtmps/SrtCaller Beta, Rtmp Optional, Rtp Later, WebRtc Deferred
const char* availabilityNote(Profile p);

// Opaque reference to a machine-local secret. Must look like "ref:<id>" so that a pasted key or URL is rejected
// instead of being stored. The app/security module creates and resolves these; this module only checks the form.
struct SecretRef {
    std::string handle;
    bool empty() const { return handle.empty(); }
    bool wellFormed() const;
};

// Resolves a reference at the moment a process is spawned. Implemented by the security/app layer.
class ISecretResolver {
public:
    virtual ~ISecretResolver() = default;
    virtual bool exists(const SecretRef& ref) const = 0;                      // cheap, no secret material returned
    virtual Result<std::string> resolve(const SecretRef& ref) const = 0;     // the secret itself; callers wipe it after use
};

struct VideoSettings {
    std::uint32_t width = 1280, height = 720, fps = 30;     // defaults: 720p30
    std::uint32_t bitrateKbps = 2500;                        // 2.5 Mb/s
    std::uint32_t keyframeSeconds = 2;                       // 2 s keyframe interval
};
struct AudioSettings {
    std::uint32_t rateHz = 48000, channels = 2, bitrateKbps = 128;     // AAC stereo 128 kb/s
};

struct BroadcastSettings {
    std::string name = "Main stream";            // endpoint label shown to the operator (not secret)
    Profile profile = Profile::Rtmps;
    std::string host;                            // hostname, IPv4 or [IPv6]; no scheme, userinfo, path or query
    std::uint32_t port = 0;                      // 0 = protocol default (RTMPS 443, RTMP 1935); SRT requires a port
    std::string path;                            // RTMP application path, e.g. "live" or "live2/app"; never the key
    SecretRef streamKey;                         // RTMP/RTMPS: required
    SecretRef srtPassphrase;                     // SRT: empty = unencrypted (needs acknowledgement)
    std::uint32_t srtKeyLenBytes = 16;           // 16 / 24 / 32 when a passphrase is set
    std::uint32_t srtLatencyMs = 200;            // 20..8000; rule of thumb 4x RTT
    std::uint32_t srtConnectTimeoutMs = 3000;    // 500..30000
    VideoSettings video;
    AudioSettings audio;
    bool acknowledgeUnencrypted = false;         // required for RTMP and for SRT without passphrase
    bool reconnectEnabled = true;                // default on, only ever for an explicitly started session
};

BroadcastSettings defaultSettings();

struct SettingIssue {
    std::string field;
    std::string message;
    bool blocking = true;
};
// baseTier=true adds BASE-tier advice (800x600 / 640x480 at 15..25 FPS fallbacks, section 15).
std::vector<SettingIssue> validateSettings(const BroadcastSettings& s, bool baseTier = false);
bool hasBlocking(const std::vector<SettingIssue>& issues);

// BASE fallback (section 15): 800x600 or 640x480 at 15..25 FPS "only when the receiver accepts it".
Result<BroadcastSettings> applyBaseFallback(const BroadcastSettings& s, bool use640x480, std::uint32_t fps, bool receiverAcceptsFallback);

// Effective network port after applying the profile default.
std::uint32_t effectivePort(const BroadcastSettings& s);
EncryptionState encryptionState(const BroadcastSettings& s);
std::string encryptionLabel(const BroadcastSettings& s);      // e.g. "RTMPS: encrypted (TLS, certificate checked)", "RTMP: NOT encrypted"
// Total upload needed: video + audio + ~10% container/transport overhead.
std::uint32_t requiredUploadKbps(const BroadcastSettings& s);
// "If upload capacity remains below bitrate, warn" - returns a warning text or an empty string.
std::string uploadWarning(const BroadcastSettings& s, std::uint32_t measuredUploadKbps);

// What the operator confirms before Start ("Distinct camera/audio publication summary appears before Start").
struct PublicationFacts {
    bool cameraPublished = false;                // is the camera layer part of the program output?
    std::string audioSourceLabel;                // e.g. "Master bus (after compressor/limiter)"
    std::string programOutputLabel;              // e.g. "Output 1 - Main"
    bool recordingActive = false;
};
struct PublicationSummary {
    std::string endpointLabel, encryption, video, audio, audioSource, programOutput;
    bool cameraPublished = false, recordingActive = false;
    std::string text() const;                    // multi-line, ready for a confirmation dialog
    std::string hash() const;                    // SHA-256 of the canonical text; bound into the start token
};
PublicationSummary makeSummary(const BroadcastSettings& s, const PublicationFacts& facts);

// Settings as portable JSON: label, endpoint, encoder settings and encryption state, never secret references.
std::string exportSettingsJson(const BroadcastSettings& s);
// Local-store round trip including the opaque references (still no secret material).
std::string serializeSettingsJson(const BroadcastSettings& s);
Result<BroadcastSettings> parseSettingsJson(const std::string& text);

}  // namespace ss::broadcast
