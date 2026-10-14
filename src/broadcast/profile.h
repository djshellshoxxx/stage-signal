// MOD-12 broadcast profiles and validation (spec §15, REQ-B01/B07). Settings carry a stream-key REFERENCE only; the key
// itself lives in ISecretStore and is fetched at connect time. Nothing in this header ever holds or prints a secret.
#pragma once
#include "core/types.h"
#include "core/ports.h"
#include "recording/video_encoder.h"
#include <string>
#include <vector>
namespace ss {

enum class BroadcastProtocol : uint8_t { RTMPS, RTMP, SRT, RTP };
const char* protocolName(BroadcastProtocol p);
bool parseProtocol(const std::string& s, BroadcastProtocol& out);

enum class SrtEncryption : uint8_t { None, Aes128, Aes192, Aes256 };

struct Endpoint {
  std::string name;                       // user label ("Studio ingest")
  BroadcastProtocol protocol = BroadcastProtocol::RTMPS;
  std::string host;                       // hostname or IP literal; no userinfo, no port, no path
  uint16_t port = 0;                      // 0 = protocol default (RTMPS 443, RTMP 1935; SRT/RTP require explicit)
  std::string path;                       // "/app" (RTMP), "" (SRT), "" (RTP); never contains a stream key or query
  std::string streamKeyRef;               // ISecretStore reference id for the stream key (RTMP/RTMPS), empty otherwise
  std::string passphraseRef;              // ISecretStore reference id for SRT passphrase when encryption != None
};

struct BroadcastSettings {
  Endpoint endpoint;
  VideoCodec videoCodec = VideoCodec::H264;     // RTMP/RTMPS/SRT: H.264 (+AAC). RTP in this build: MJPEG (+L16)
  int width = 1280, height = 720, fps = 30;     // defaults 720p30
  int videoKbps = 2500, maxVideoKbps = 0;       // 0 => 1.2 x target
  int audioKbps = 128; uint32_t audioRate = 48000; uint32_t audioChannels = 2;   // AAC stereo 128 kb/s
  int keyframeSec = 2;
  int srtLatencyMs = 120; SrtEncryption srtEncryption = SrtEncryption::None;
  bool reconnect = true;                        // default enabled for the explicitly started session only
  bool allowNonLanRtp = false;                  // RTP is unencrypted: public destinations need explicit opt-in
  int rtpTtl = 1; int rtpMtu = 1200;            // RTP payload budget
  bool baseFallback = false;                    // BASE tier 800x600 / 640x480 at 15-25 fps, only if receiver accepts it
};

BroadcastSettings defaultBroadcastSettings();                 // 720p30 / 2.5 Mb/s / AAC 128k / 2 s keyframes, RTMPS
BroadcastSettings baseFallbackSettings(bool vga640x480);      // 800x600 or 640x480 @ 20 fps, 1.0 Mb/s (receiver must accept)

// Validate scheme/host/port/path/key-reference and media settings. Returns Ok or an actionable InvalidArgument /
// UnsupportedCapability. `warnings` receives non-fatal notes (never contains secrets).
Status validateEndpoint(const Endpoint& e, bool allowNonLanRtp, std::vector<std::string>* warnings = nullptr);
Status validateSettings(const BroadcastSettings& s, std::vector<std::string>* warnings = nullptr);
// Parse "scheme://host[:port][/path]" into an Endpoint (rejects userinfo, query, fragment). Key must be given via ref.
Status parseEndpointUrl(const std::string& url, Endpoint& out);
std::string endpointUrl(const Endpoint& e);                   // "rtmps://host:443/app" (no key, no userinfo)
uint16_t effectivePort(const Endpoint& e);
std::string encryptionLabel(const BroadcastSettings& s);      // "TLS 1.x (certificate verified)" / "UNENCRYPTED" ...
bool isEncrypted(const BroadcastSettings& s);

// What leaves the machine; shown BEFORE Start (spec §15 "distinct camera/audio publication summary").
struct PublicationInputs {
  bool programVideo = true; bool cameraInProgram = false; std::string cameraLabel;
  bool audio = true; std::string audioBusLabel = "recording/broadcast bus"; bool dynamicsActive = true;
  bool localRecordingActive = false;
};
std::string publicationSummary(const BroadcastSettings& s, const PublicationInputs& in);

// Show-export JSON of settings: reference ids only, never a secret. importSettingsJson validates.
std::string exportSettingsJson(const BroadcastSettings& s);
Status importSettingsJson(const std::string& json, BroadcastSettings& out);

} // namespace ss
