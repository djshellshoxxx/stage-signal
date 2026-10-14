#include "broadcast/profile.h"
#include "broadcast/redact.h"
#include "nlohmann/json.hpp"
#include <cctype>
#include <cstdio>
#include <algorithm>
namespace ss {
using nlohmann::json;

const char* protocolName(BroadcastProtocol p) { switch (p) { case BroadcastProtocol::RTMPS: return "RTMPS"; case BroadcastProtocol::RTMP: return "RTMP"; case BroadcastProtocol::SRT: return "SRT"; case BroadcastProtocol::RTP: return "RTP"; } return "?"; }
bool parseProtocol(const std::string& s, BroadcastProtocol& o) {
  std::string l; for (char c : s) l.push_back((char)std::tolower((unsigned char)c));
  if (l == "rtmps") o = BroadcastProtocol::RTMPS; else if (l == "rtmp") o = BroadcastProtocol::RTMP; else if (l == "srt") o = BroadcastProtocol::SRT; else if (l == "rtp") o = BroadcastProtocol::RTP; else return false;
  return true;
}
uint16_t effectivePort(const Endpoint& e) {
  if (e.port) return e.port;
  if (e.protocol == BroadcastProtocol::RTMPS) return 443;
  if (e.protocol == BroadcastProtocol::RTMP) return 1935;
  return 0;
}
std::string endpointUrl(const Endpoint& e) {
  std::string h = e.host; if (h.find(':') != std::string::npos && h[0] != '[') h = "[" + h + "]";
  uint16_t p = effectivePort(e);
  std::string lp = e.protocol == BroadcastProtocol::RTMPS ? "rtmps" : e.protocol == BroadcastProtocol::RTMP ? "rtmp" : e.protocol == BroadcastProtocol::SRT ? "srt" : "rtp";
  return lp + "://" + h + (p ? ":" + std::to_string(p) : "") + e.path;
}
BroadcastSettings defaultBroadcastSettings() { return BroadcastSettings(); }
BroadcastSettings baseFallbackSettings(bool vga) {
  BroadcastSettings s; s.baseFallback = true; s.width = vga ? 640 : 800; s.height = vga ? 480 : 600; s.fps = 20; s.videoKbps = 1000; s.audioKbps = 96; return s;
}

static bool validRef(const std::string& r) {
  if (r.empty() || r.size() > 64) return false;
  for (char c : r) if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == ':')) return false;
  return true;
}
static bool validHostname(const std::string& h) {
  if (h.empty() || h.size() > 253) return false;
  size_t label = 0;
  for (char c : h) {
    if (c == '.') { if (label == 0) return false; label = 0; continue; }
    if (!(std::isalnum((unsigned char)c) || c == '-' || c == '_')) return false;
    if (++label > 63) return false;
  }
  return label > 0 && h.front() != '-' && h.back() != '-';
}
static bool parseIPv4(const std::string& h, int o[4]) {
  int idx = 0; size_t i = 0;
  while (idx < 4) { if (i >= h.size() || !std::isdigit((unsigned char)h[i])) return false; int v = 0, n = 0; while (i < h.size() && std::isdigit((unsigned char)h[i])) { v = v * 10 + (h[i] - '0'); if (++n > 3 || v > 255) return false; ++i; } o[idx++] = v; if (idx < 4) { if (i >= h.size() || h[i] != '.') return false; ++i; } }
  return i == h.size();
}
static bool validIPv6(const std::string& h) {
  if (h.empty() || h.size() > 45) return false;
  int colons = 0;
  for (char c : h) { if (c == ':') ++colons; else if (!std::isxdigit((unsigned char)c) && c != '.') return false; }
  return colons >= 2;
}
static bool lanHost(const std::string& h) {
  int o[4];
  if (parseIPv4(h, o)) return o[0] == 10 || o[0] == 127 || (o[0] == 192 && o[1] == 168) || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 169 && o[1] == 254) || (o[0] >= 224 && o[0] <= 239);
  if (validIPv6(h)) { std::string l; for (char c : h) l.push_back((char)std::tolower((unsigned char)c)); return l == "::1" || l.rfind("fe80:", 0) == 0 || l.rfind("fc", 0) == 0 || l.rfind("fd", 0) == 0 || l.rfind("ff", 0) == 0; }
  std::string l; for (char c : h) l.push_back((char)std::tolower((unsigned char)c));
  auto ends = [&](const char* s) { size_t n = std::char_traits<char>::length(s); return l.size() > n && l.compare(l.size() - n, n, s) == 0; };
  return l == "localhost" || ends(".local") || ends(".lan") || ends(".internal");
}

Status validateEndpoint(const Endpoint& e, bool allowNonLan, std::vector<std::string>* warn) {
  auto bad = [](const std::string& m) { return Status::Fail(Err::InvalidArgument, cast::redactText(m)); };
  if (e.name.size() > 80) return bad("Endpoint name is longer than 80 characters.");
  if (e.host.empty()) return bad("Endpoint host is empty. Enter the receiver host name or IP address.");
  if (e.host.find('@') != std::string::npos || e.host.find('/') != std::string::npos || e.host.find('?') != std::string::npos || e.host.find(' ') != std::string::npos || e.host.find('#') != std::string::npos)
    return bad("Endpoint host must be a bare host name or IP address (no user name, path, query or spaces). Credentials belong in the credential store, not the address.");
  int o[4];
  if (!parseIPv4(e.host, o) && !validIPv6(e.host) && !validHostname(e.host)) return bad("Endpoint host is not a valid host name or IP address.");
  bool needPort = e.protocol == BroadcastProtocol::SRT || e.protocol == BroadcastProtocol::RTP;
  if (needPort && e.port == 0) return bad(std::string(protocolName(e.protocol)) + " requires an explicit port.");
  if (e.path.size() > 128) return bad("Endpoint path is too long.");
  if (!e.path.empty() && e.path[0] != '/') return bad("Endpoint path must start with '/'.");
  if (e.path.find('?') != std::string::npos || e.path.find('#') != std::string::npos || e.path.find("..") != std::string::npos || e.path.find('\\') != std::string::npos || e.path.find('@') != std::string::npos || e.path.find("//") != std::string::npos)
    return bad("Endpoint path must not contain a query, fragment, '..', '@' or empty segments. The stream key is entered separately and stored in the credential store.");
  for (char c : e.path) if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7e) return bad("Endpoint path contains spaces or non-ASCII characters.");
  switch (e.protocol) {
    case BroadcastProtocol::RTMPS: case BroadcastProtocol::RTMP:
      if (e.path.empty() || e.path == "/") return bad("RTMP/RTMPS needs an application path such as /live (the stream key is separate).");
      if (e.path.find('/', 1) != std::string::npos) return bad("RTMP/RTMPS application path must be a single segment such as /live; the stream key is not part of the address.");
      if (!validRef(e.streamKeyRef)) return bad("A stream-key reference is required (1-64 characters from A-Z a-z 0-9 _ - . :). Store the key in the credential store and enter its reference, not the key itself.");
      if (e.protocol == BroadcastProtocol::RTMP && warn) warn->push_back("RTMP is unencrypted: the stream key and media cross the network in clear text. Use only on a trusted/local link, or choose RTMPS.");
      break;
    case BroadcastProtocol::SRT:
      if (!e.path.empty()) return bad("SRT does not use a path; leave it empty.");
      if (!e.streamKeyRef.empty() && !validRef(e.streamKeyRef)) return bad("Invalid stream-key reference.");
      break;
    case BroadcastProtocol::RTP:
      if (!e.path.empty() || !e.streamKeyRef.empty() || !e.passphraseRef.empty()) return bad("RTP has neither path nor keys; leave them empty.");
      if (e.port < 1024 || (e.port & 1)) return bad("RTP needs an even UDP port >= 1024 (RTCP uses port+1; audio uses port+2).");
      if (!lanHost(e.host) && !allowNonLan) return bad("RTP is unencrypted and intended for controlled LAN receivers; '" + e.host + "' is not a private, loopback or multicast address. Enable the explicit non-LAN RTP option only if you really intend this.");
      if (warn) warn->push_back("RTP is UNENCRYPTED (plain RTP/AVP): anyone on the path can see audio and video.");
      break;
  }
  if (e.passphraseRef.size() && !validRef(e.passphraseRef)) return bad("Invalid passphrase reference.");
  return Status::Ok();
}

Status validateSettings(const BroadcastSettings& s, std::vector<std::string>* warn) {
  auto bad = [](const std::string& m) { return Status::Fail(Err::InvalidArgument, m); };
  Status e = validateEndpoint(s.endpoint, s.allowNonLanRtp, warn); if (!e.ok()) return e;
  bool rtp = s.endpoint.protocol == BroadcastProtocol::RTP;
  if (rtp) { if (s.videoCodec != VideoCodec::MJPEG) return Status::Fail(Err::UnsupportedCapability, "RTP in this build carries MJPEG video (RFC 2435) and L16 audio only; H.264/RTP is not implemented."); }
  else if (s.videoCodec != VideoCodec::H264) return Status::Fail(Err::UnsupportedCapability, std::string(protocolName(s.endpoint.protocol)) + " contribution uses H.264 + AAC; other codecs are not part of the receiver profile.");
  if (s.width < 160 || s.height < 120 || s.width > 3840 || s.height > 2160 || (s.width & 1) || (s.height & 1)) return bad("Video size must be even, between 160x120 and 3840x2160.");
  if (rtp && (s.width % 8 || s.height % 8 || s.width > 2040 || s.height > 2040)) return bad("RTP/JPEG (RFC 2435) needs width and height that are multiples of 8 and at most 2040.");
  if (s.fps < 5 || s.fps > 60) return bad("Frame rate must be 5-60 fps.");
  if (s.videoKbps < 100 || s.videoKbps > 50000) return bad("Video bitrate must be 100-50000 kb/s.");
  if (s.maxVideoKbps && s.maxVideoKbps < s.videoKbps) return bad("Max video bitrate must be >= the target bitrate.");
  if (s.audioKbps < 32 || s.audioKbps > 320) return bad("Audio bitrate must be 32-320 kb/s.");
  if (s.audioRate != 44100 && s.audioRate != 48000) return bad("Audio rate must be 44100 or 48000 Hz.");
  if (s.audioChannels < 1 || s.audioChannels > 2) return bad("Audio must be mono or stereo.");
  if (s.keyframeSec < 1 || s.keyframeSec > 10) return bad("Keyframe interval must be 1-10 s.");
  if (s.srtLatencyMs < 20 || s.srtLatencyMs > 8000) return bad("SRT latency must be 20-8000 ms.");
  if (s.rtpTtl < 1 || s.rtpTtl > 16) return bad("RTP TTL must be 1-16.");
  if (s.rtpMtu < 400 || s.rtpMtu > 1400) return bad("RTP payload budget must be 400-1400 bytes.");
  if (s.endpoint.protocol == BroadcastProtocol::SRT && s.srtEncryption != SrtEncryption::None && s.endpoint.passphraseRef.empty()) return bad("SRT encryption is enabled but no passphrase reference is set.");
  if (s.endpoint.protocol != BroadcastProtocol::SRT && s.srtEncryption != SrtEncryption::None) return bad("SRT encryption applies only to SRT endpoints.");
  if (s.endpoint.protocol == BroadcastProtocol::SRT && s.srtEncryption == SrtEncryption::None && warn) warn->push_back("SRT encryption is OFF: media is sent unencrypted.");
  bool std720 = s.width == 1280 && s.height == 720;
  bool base = (s.width == 800 && s.height == 600) || (s.width == 640 && s.height == 480);
  if (s.baseFallback) {
    if (!base) return bad("BASE fallback is 800x600 or 640x480.");
    if (s.fps < 15 || s.fps > 25) return bad("BASE fallback frame rate is 15-25 fps.");
    if (warn) warn->push_back("BASE fallback resolution/frame rate works only if the receiver accepts it; verify with your receiver.");
  } else if (base && warn) warn->push_back("800x600/640x480 is the BASE fallback; mark it as such and confirm the receiver accepts it.");
  (void)std720;
  return Status::Ok();
}

Status parseEndpointUrl(const std::string& url, Endpoint& out) {
  size_t sch = url.find("://");
  if (sch == std::string::npos) return Status::Fail(Err::InvalidArgument, "Address must look like scheme://host:port/path (rtmps, rtmp, srt or rtp).");
  Endpoint e; if (!parseProtocol(url.substr(0, sch), e.protocol)) return Status::Fail(Err::InvalidArgument, "Unsupported scheme '" + cast::redactText(url.substr(0, sch)) + "'. Use rtmps, rtmp, srt or rtp.");
  std::string rest = url.substr(sch + 3);
  if (rest.find('?') != std::string::npos || rest.find('#') != std::string::npos) return Status::Fail(Err::InvalidArgument, "Address must not contain a query or fragment; enter the stream key separately (it is stored in the credential store).");
  size_t slash = rest.find('/'); std::string auth = slash == std::string::npos ? rest : rest.substr(0, slash); e.path = slash == std::string::npos ? "" : rest.substr(slash);
  if (auth.find('@') != std::string::npos) return Status::Fail(Err::InvalidArgument, "Address must not contain a user name or password.");
  std::string host = auth; std::string port;
  if (!auth.empty() && auth[0] == '[') { size_t rb = auth.find(']'); if (rb == std::string::npos) return Status::Fail(Err::InvalidArgument, "Unterminated IPv6 literal."); host = auth.substr(1, rb - 1); if (rb + 1 < auth.size()) { if (auth[rb + 1] != ':') return Status::Fail(Err::InvalidArgument, "Bad port separator."); port = auth.substr(rb + 2); } }
  else { size_t c = auth.rfind(':'); if (c != std::string::npos) { host = auth.substr(0, c); port = auth.substr(c + 1); } }
  if (!port.empty()) { int v = 0; for (char c : port) { if (!std::isdigit((unsigned char)c) || v > 65535) return Status::Fail(Err::InvalidArgument, "Port must be 1-65535."); v = v * 10 + (c - '0'); } if (v < 1 || v > 65535) return Status::Fail(Err::InvalidArgument, "Port must be 1-65535."); e.port = (uint16_t)v; }
  e.host = host; out = e; return Status::Ok();
}

bool isEncrypted(const BroadcastSettings& s) {
  switch (s.endpoint.protocol) { case BroadcastProtocol::RTMPS: return true; case BroadcastProtocol::RTMP: return false; case BroadcastProtocol::SRT: return s.srtEncryption != SrtEncryption::None; case BroadcastProtocol::RTP: return false; }
  return false;
}
std::string encryptionLabel(const BroadcastSettings& s) {
  switch (s.endpoint.protocol) {
    case BroadcastProtocol::RTMPS: return "Encrypted: TLS (server certificate must validate; no override)";
    case BroadcastProtocol::RTMP: return "UNENCRYPTED (plain RTMP)";
    case BroadcastProtocol::SRT: switch (s.srtEncryption) { case SrtEncryption::None: return "UNENCRYPTED (SRT encryption off)"; case SrtEncryption::Aes128: return "Encrypted: SRT AES-128"; case SrtEncryption::Aes192: return "Encrypted: SRT AES-192"; case SrtEncryption::Aes256: return "Encrypted: SRT AES-256"; } break;
    case BroadcastProtocol::RTP: return "UNENCRYPTED (plain RTP/AVP; SRTP is not implemented)";
  }
  return "unknown";
}

std::string publicationSummary(const BroadcastSettings& s, const PublicationInputs& in) {
  std::string t;
  t += "About to publish to \"" + s.endpoint.name + "\" - " + std::string(protocolName(s.endpoint.protocol)) + " " + endpointUrl(s.endpoint) + "\n";
  t += "Security: " + encryptionLabel(s) + "\n";
  if (s.endpoint.protocol == BroadcastProtocol::RTMP || s.endpoint.protocol == BroadcastProtocol::RTMPS) t += "Stream key: credential reference '" + s.endpoint.streamKeyRef + "' (the key is read from the credential store at connect and is never shown or logged)\n";
  char b[160];
  std::snprintf(b, sizeof b, "Video: %s %dx%d @ %d fps, %d kb/s, keyframe every %d s%s\n", codecName(s.videoCodec), s.width, s.height, s.fps, s.videoKbps, s.keyframeSec, s.baseFallback ? " (BASE fallback)" : ""); t += b;
  std::snprintf(b, sizeof b, "Audio: %s %u Hz %s, %d kb/s\n", s.videoCodec == VideoCodec::MJPEG ? "PCM L16" : "AAC", s.audioRate, s.audioChannels == 2 ? "stereo" : "mono", s.videoCodec == VideoCodec::MJPEG ? (int)(s.audioRate * s.audioChannels * 16 / 1000) : s.audioKbps); t += b;
  t += "This will send: ";
  t += in.programVideo ? "the program video output" : "no video";
  t += in.cameraInProgram ? ", INCLUDING the camera" + (in.cameraLabel.empty() ? std::string() : " (" + in.cameraLabel + ")") + " as composited into the program" : " (camera NOT included)";
  t += in.audio ? "; audio from " + in.audioBusLabel + (in.dynamicsActive ? " (compressor/limiter on)" : " (dynamics bypassed)") : "; no audio";
  t += ".\n";
  t += in.localRecordingActive ? "Local recording is running separately and will continue if the network fails.\n" : "Local recording is not running.\n";
  t += s.reconnect ? "Reconnect: on (retries 1/2/4/8/15 s with jitter for up to 120 s, then stops)\n" : "Reconnect: off\n";
  t += "Nothing is sent until you press Start; \"Live\" will mean the connection is up and media is being delivered, not that a public service is displaying it.";
  return cast::redactText(t);
}

std::string exportSettingsJson(const BroadcastSettings& s) {
  json j; j["schema"] = 1;
  j["endpoint"] = {{"name", s.endpoint.name}, {"protocol", protocolName(s.endpoint.protocol)}, {"host", s.endpoint.host}, {"port", s.endpoint.port}, {"path", s.endpoint.path}, {"stream_key_ref", s.endpoint.streamKeyRef}, {"passphrase_ref", s.endpoint.passphraseRef}};
  j["video"] = {{"codec", codecName(s.videoCodec)}, {"width", s.width}, {"height", s.height}, {"fps", s.fps}, {"kbps", s.videoKbps}, {"max_kbps", s.maxVideoKbps}, {"keyframe_sec", s.keyframeSec}};
  j["audio"] = {{"kbps", s.audioKbps}, {"rate", s.audioRate}, {"channels", s.audioChannels}};
  j["srt"] = {{"latency_ms", s.srtLatencyMs}, {"encryption", (int)s.srtEncryption}};
  j["reconnect"] = s.reconnect; j["allow_non_lan_rtp"] = s.allowNonLanRtp; j["rtp"] = {{"ttl", s.rtpTtl}, {"mtu", s.rtpMtu}}; j["base_fallback"] = s.baseFallback;
  j["note"] = "secrets are stored in the machine-local credential store and are never exported";
  return j.dump(2);
}
Status importSettingsJson(const std::string& text, BroadcastSettings& out) {
  json j = json::parse(text, nullptr, false);
  if (!j.is_object() || !j.contains("endpoint")) return Status::Fail(Err::SchemaUnsupported, "Broadcast settings JSON is malformed.");
  BroadcastSettings s;
  try {
    auto& e = j["endpoint"];
    s.endpoint.name = e.value("name", std::string()); s.endpoint.host = e.value("host", std::string()); s.endpoint.port = (uint16_t)e.value("port", 0); s.endpoint.path = e.value("path", std::string());
    s.endpoint.streamKeyRef = e.value("stream_key_ref", std::string()); s.endpoint.passphraseRef = e.value("passphrase_ref", std::string());
    if (!parseProtocol(e.value("protocol", std::string()), s.endpoint.protocol)) return Status::Fail(Err::SchemaUnsupported, "Unknown protocol in settings.");
    auto& v = j["video"]; std::string c = v.value("codec", std::string("H.264")); s.videoCodec = c == "MJPEG" ? VideoCodec::MJPEG : VideoCodec::H264;
    s.width = v.value("width", 1280); s.height = v.value("height", 720); s.fps = v.value("fps", 30); s.videoKbps = v.value("kbps", 2500); s.maxVideoKbps = v.value("max_kbps", 0); s.keyframeSec = v.value("keyframe_sec", 2);
    auto& a = j["audio"]; s.audioKbps = a.value("kbps", 128); s.audioRate = a.value("rate", 48000u); s.audioChannels = a.value("channels", 2u);
    auto& r = j["srt"]; s.srtLatencyMs = r.value("latency_ms", 120); s.srtEncryption = (SrtEncryption)r.value("encryption", 0);
    s.reconnect = j.value("reconnect", true); s.allowNonLanRtp = j.value("allow_non_lan_rtp", false); s.baseFallback = j.value("base_fallback", false);
    auto& p = j["rtp"]; s.rtpTtl = p.value("ttl", 1); s.rtpMtu = p.value("mtu", 1200);
  } catch (...) { return Status::Fail(Err::SchemaUnsupported, "Broadcast settings JSON has wrong field types."); }
  Status v = validateSettings(s); if (!v.ok()) return v;
  out = s; return Status::Ok();
}

} // namespace ss
