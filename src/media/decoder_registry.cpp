#include "media/decoder_registry.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cstring>

namespace ss { namespace media {

void DecoderRegistry::registerDecoder(DecoderEntry e) { entries_.push_back(std::move(e)); }

Status DecoderRegistry::loadManifestAvailability(const std::string& text) {
  try {
    nlohmann::json j = nlohmann::json::parse(text);
    const nlohmann::json* arr = nullptr;
    if (j.is_object() && j.contains("decoders")) arr = &j["decoders"]; else if (j.is_array()) arr = &j;
    if (!arr || !arr->is_array()) return Status::Fail(Err::SchemaUnsupported, "manifest has no decoders array");
    std::vector<std::string> ids;
    for (const auto& d : *arr) {
      if (!d.is_object() || !d.contains("id") || !d["id"].is_string()) return Status::Fail(Err::SchemaUnsupported, "decoder entry without id");
      if (d.value("enabled", false)) ids.push_back(d["id"].get<std::string>());
    }
    enabledIds_ = ids; manifestLoaded_ = true;
    return Status::Ok();
  } catch (...) {
    return Status::Fail(Err::SchemaUnsupported, "manifest decoder section unreadable");
  }
}

const DecoderEntry* DecoderRegistry::find(const std::string& container, const std::string& codec) const {
  if (!manifestLoaded_) return nullptr;                                   // fail closed
  for (const DecoderEntry& e : entries_) {
    if (!e.factory) continue;
    if (std::find(enabledIds_.begin(), enabledIds_.end(), e.id) == enabledIds_.end()) continue;
    if (std::find(e.containers.begin(), e.containers.end(), container) == e.containers.end()) continue;
    if (std::find(e.codecs.begin(), e.codecs.end(), codec) == e.codecs.end()) continue;
    return &e;
  }
  return nullptr;
}
bool DecoderRegistry::isAvailable(const std::string& c, const std::string& k) const { return find(c, k) != nullptr; }
std::unique_ptr<IVideoDecoder> DecoderRegistry::create(const std::string& c, const std::string& k) const {
  const DecoderEntry* e = find(c, k);
  if (!e) return nullptr;
  try { return e->factory(); } catch (...) { return nullptr; }
}

std::vector<DecoderAvailability> DecoderRegistry::report() const {
  std::vector<DecoderAvailability> out;
  for (const DecoderEntry& e : entries_) {
    DecoderAvailability a; a.id = e.id; a.compiled = (bool)e.factory; a.containers = e.containers; a.codecs = e.codecs;
    a.enabledByManifest = std::find(enabledIds_.begin(), enabledIds_.end(), e.id) != enabledIds_.end();
    a.available = a.compiled && a.enabledByManifest && manifestLoaded_;
    a.reason = !manifestLoaded_ ? "no manifest loaded (fail closed)" : !a.enabledByManifest ? "not enabled in package manifest" : !a.compiled ? "no factory" : "";
    out.push_back(a);
  }
  for (const std::string& id : enabledIds_) {
    bool known = false; for (const DecoderEntry& e : entries_) if (e.id == id) known = true;
    if (!known) { DecoderAvailability a; a.id = id; a.enabledByManifest = true; a.reason = "listed in manifest but not compiled into this build"; out.push_back(a); }
  }
  return out;
}

std::string videoContainerKey(const std::string& path) {
  std::string b = baseName(path); size_t dot = b.rfind('.');
  if (dot == std::string::npos) return "";
  std::string e = b.substr(dot + 1); for (auto& c : e) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return (e == "mp4" || e == "mov" || e == "mkv") ? e : "";
}

static bool contains(const uint8_t* d, size_t n, const char* s) {
  size_t m = std::strlen(s); if (n < m) return false;
  for (size_t i = 0; i + m <= n; ++i) if (d[i] == (uint8_t)s[0] && !std::memcmp(d + i, s, m)) return true;
  return false;
}
std::string probeVideoCodec(const uint8_t* d, size_t n, Container c) {
  if (c == Container::Mp4Mov) {
    if (contains(d, n, "avc1") || contains(d, n, "avc3")) return "h264";
    if (contains(d, n, "hvc1") || contains(d, n, "hev1")) return "h265";
    if (contains(d, n, "av01")) return "av1";
    if (contains(d, n, "vp09")) return "vp9";
  } else if (c == Container::Matroska) {
    if (contains(d, n, "V_MPEG4/ISO/AVC")) return "h264";
    if (contains(d, n, "V_MPEGH/ISO/HEVC")) return "h265";
    if (contains(d, n, "V_VP9")) return "vp9";
    if (contains(d, n, "V_VP8")) return "vp8";
    if (contains(d, n, "V_AV1")) return "av1";
  }
  return "";
}

}} // namespace ss::media
