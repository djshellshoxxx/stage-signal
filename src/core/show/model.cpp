#include "core/show/model.h"
#include "nlohmann/json.hpp"
#include <cmath>
#include <set>

namespace ss {
namespace show {

const std::vector<RendererInfo>& rendererPack() {
  static const std::vector<RendererInfo> pack = {
    {"spectrum_bars",   false, {{"bars", 4, 128, 32, false}, {"smoothing", 0, 1, 0.6f, false}, {"gain", 0.25f, 4, 1, false}}},
    {"waveform_ribbon", false, {{"thickness", 0.01f, 0.5f, 0.1f, false}, {"layers", 1, 6, 3, false}, {"smoothing", 0, 1, 0.5f, false}}},
    {"geometric_orbit", false, {{"orbits", 1, 12, 4, false}, {"radius", 0.1f, 1, 0.6f, false}, {"trail", 0, 1, 0.3f, false}}},
    {"radial_pulse",    false, {{"rings", 1, 16, 6, false}, {"pulse", 0, 1, 0.5f, false}}},
    {"particle_field",  false, {{"count", 50, 2000, 400, false}, {"size", 0.5f, 8, 2, false}, {"drift", 0, 2, 0.5f, false}}},
    {"grid_pulse",      false, {{"cells", 4, 64, 16, false}, {"pulse", 0, 1, 0.5f, false}, {"line", 0.5f, 4, 1, false}}},
    {"tunnel",          false, {{"depth", 2, 32, 12, false}, {"twist", -1, 1, 0.2f, false}}},
    {"color_field",     false, {{"saturation", 0, 1, 0.8f, false}, {"blend", 0, 1, 0.5f, false}, {"drift_rate", 0, 2, 0.25f, false}}},
    {"plasma",          false, {{"scale", 0.5f, 8, 2, false}, {"octaves", 1, 4, 2, false}, {"palette", 0, 1, 0, false}}},
    {"starburst",       true,  {{"rays", 3, 64, 16, false}, {"length", 0.1f, 1, 0.7f, false}, {"flash_rate", 0, 20, 0, true}}},
    {"scope_lissajous", false, {{"a", 1, 9, 3, false}, {"b", 1, 9, 2, false}, {"phase", 0, 6.2832f, 0, false}}},
    {"ripple_rings",    false, {{"rings", 1, 24, 8, false}, {"decay", 0.1f, 4, 1, false}}},
  };
  return pack;
}
const RendererInfo* findRenderer(const std::string& id) {
  for (const auto& r : rendererPack()) if (id == r.id) return &r;
  return nullptr;
}
const ParamDef* findParam(const RendererInfo& r, const std::string& name) {
  for (const auto& p : r.params) if (name == p.name) return &p;
  return nullptr;
}

bool utf8Valid(const std::string& s, size_t* cps) {
  size_t i = 0, n = s.size(), count = 0;
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp; size_t len;
    if (c < 0x80) { cp = c; len = 1; }
    else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
    else return false;
    if (i + len > n) return false;
    for (size_t k = 1; k < len; ++k) {
      unsigned char cc = (unsigned char)s[i + k];
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3F);
    }
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000)) return false;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    i += len; ++count;
  }
  if (cps) *cps = count;
  return true;
}

bool validId(const std::string& s) {
  if (s.empty() || s.size() > 64) return false;
  for (char c : s) if (!(isalnum((unsigned char)c) || c == '_' || c == '.' || c == '-')) return false;
  return true;
}
bool isHex(const std::string& s, size_t mn, size_t mx) {
  if (s.size() < mn || s.size() > mx) return false;
  for (char c : s) if (!isxdigit((unsigned char)c)) return false;
  return true;
}
bool validUuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < 36; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) { if (s[i] != '-') return false; }
    else if (!isxdigit((unsigned char)s[i])) return false;
  }
  return true;
}
bool safeRelPath(const std::string& p) {
  if (p.empty() || p.size() > 240) return false;
  if (p[0] == '/' || p[0] == '\\') return false;
  size_t segStart = 0;
  for (size_t i = 0; i <= p.size(); ++i) {
    if (i == p.size() || p[i] == '/') {
      size_t len = i - segStart;
      if (len == 0) return false;
      std::string seg = p.substr(segStart, len);
      if (seg == "." || seg == "..") return false;
      if (seg.back() == '.' || seg.back() == ' ') return false;
      segStart = i + 1;
    } else {
      unsigned char c = (unsigned char)p[i];
      if (c < 0x20 || c == 0x7f || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
  }
  return true;
}

static const char* const kFormats[] = {"png", "jpg", "jpeg", "bmp", "gif", "mp4", "mov", "mkv"};
const char* const* mediaFormats(size_t* n) { if (n) *n = sizeof(kFormats) / sizeof(*kFormats); return kFormats; }
static bool formatOk(const std::string& f) { for (auto* x : kFormats) if (f == x) return true; return false; }

bool sceneIsStrobe(const Scene& s) { return s.strobe; }

std::map<std::string, float> resolvedParams(const Scene& s) {
  std::map<std::string, float> out;
  if (const RendererInfo* r = findRenderer(s.rendererType)) for (const auto& p : r->params) out[p.name] = p.def;
  for (const auto& kv : s.params) out[kv.first] = kv.second;
  return out;
}

namespace {
Status bad(const std::string& where, const std::string& why) { return Status::Fail(Err::InvalidArgument, where + ": " + why, true); }

// Opaque JSON items: must be an object, bounded, no executable/network behaviour keys, bounded depth.
bool scanOpaque(const nlohmann::json& j, int depth, std::string& why) {
  if (depth > 16) { why = "nesting too deep"; return false; }
  static const std::set<std::string> forbidden = {"script", "exec", "eval", "shell", "command", "cmd", "url", "uri", "href", "plugin", "dll", "include"};
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      std::string k = it.key();
      for (auto& ch : k) ch = (char)tolower((unsigned char)ch);
      if (forbidden.count(k)) { why = "forbidden key '" + it.key() + "'"; return false; }
      if (!scanOpaque(it.value(), depth + 1, why)) return false;
    }
  } else if (j.is_array()) {
    for (const auto& e : j) if (!scanOpaque(e, depth + 1, why)) return false;
  }
  return true;
}
Status checkOpaque(const char* what, size_t idx, const std::string& text) {
  std::string where = std::string(what) + "[" + std::to_string(idx) + "]";
  if (text.size() > limits::kMaxOpaqueBytes) return bad(where, "too large");
  nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) return bad(where, "must be a JSON object");
  std::string why;
  if (!scanOpaque(j, 0, why)) return bad(where, why);
  return Status::Ok();
}
Status checkTransition(const std::string& where, const Transition& t) {
  if (t.type != "cut" && t.type != "fade" && t.type != "dip_black" && t.type != "wipe") return bad(where, "unknown transition type");
  if (t.ms < 0 || t.ms > 60000) return bad(where, "transition ms out of range 0..60000");
  return Status::Ok();
}
} // namespace

Status validateShow(const ShowDocument& d) {
  // Counts first (cheap; before any per-item work).
  if (d.scenes.size() > limits::kMaxScenes) return bad("scenes", "too many (max 1000)");
  if (d.cues.size() > limits::kMaxCues) return bad("cues", "too many (max 10000)");
  if (d.texts.size() > limits::kMaxMessages) return bad("texts", "too many (max 256)");
  if (d.midiMappings.size() > limits::kMaxMidiMappings) return bad("midiMappings", "too many (max 512)");
  if (d.fixtureProfiles.size() > limits::kMaxFixtures) return bad("fixtureProfiles", "too many (max 256)");
  if (d.patch.size() > limits::kMaxFixtures) return bad("patch", "too many (max 256)");
  if (d.outputs.size() > limits::kMaxOutputs) return bad("outputs", "too many (max 16)");
  if (d.media.size() > limits::kMaxMedia) return bad("media", "too many");
  if (d.autoCfg.pool.size() > limits::kMaxPool) return bad("autoCfg.pool", "too many");
  if (d.journal.size() > limits::kMaxJournal) return bad("journal", "too many");

  if (d.schemaMajor != kSchemaMajor) return Status::Fail(Err::SchemaUnsupported, "schema major " + std::to_string(d.schemaMajor) + " unsupported", false);
  if (!validUuid(d.uuid)) return bad("uuid", "not a canonical UUID");
  size_t cps = 0;
  if (!utf8Valid(d.name, &cps) || cps > 128) return bad("name", "invalid UTF-8 or longer than 128 code points");
  if (d.defaultMode < 0 || d.defaultMode > 2) return bad("defaultMode", "out of range");
  if (!d.qualityProfile.empty() && !validId(d.qualityProfile)) return bad("qualityProfile", "invalid reference id");
  if (d.manualBpm != 0 && !(d.manualBpm >= 20 && d.manualBpm <= 300)) return bad("manualBpm", "must be 0 or 20..300");
  if (d.manualKey < -1 || d.manualKey > 23) return bad("manualKey", "must be -1..23");

  std::set<std::string> sceneIds;
  for (size_t i = 0; i < d.scenes.size(); ++i) {
    const Scene& s = d.scenes[i];
    std::string w = "scenes[" + std::to_string(i) + "]";
    if (!validId(s.id)) return bad(w + ".id", "invalid id");
    if (!sceneIds.insert(s.id).second) return bad(w + ".id", "duplicate id");
    if (!utf8Valid(s.name, &cps) || cps > 128) return bad(w + ".name", "invalid or too long");
    const RendererInfo* r = findRenderer(s.rendererType);
    if (!r) return bad(w + ".rendererType", "unknown renderer '" + s.rendererType + "'");
    if (s.cost < 1 || s.cost > 10) return bad(w + ".cost", "must be 1..10");
    if (s.params.size() > limits::kMaxParams) return bad(w + ".params", "too many");
    bool flashing = false;
    for (const auto& kv : s.params) {
      const ParamDef* p = findParam(*r, kv.first);
      if (!p) return bad(w + ".params." + kv.first, "not declared by " + s.rendererType);
      if (!std::isfinite(kv.second) || kv.second < p->min || kv.second > p->max) return bad(w + ".params." + kv.first, "out of declared range");
      if (p->strobe && kv.second > 0) flashing = true;
    }
    if (flashing && !s.strobe) return bad(w + ".strobe", "scene has flashing params but strobe flag is false");
  }

  std::set<std::string> mediaHashes;
  for (size_t i = 0; i < d.media.size(); ++i) {
    const MediaRecord& m = d.media[i];
    std::string w = "media[" + std::to_string(i) + "]";
    if (!isHex(m.hash, 64, 64)) return bad(w + ".hash", "must be 64 hex chars (sha256)");
    if (!mediaHashes.insert(m.hash).second) return bad(w + ".hash", "duplicate");
    if (!safeRelPath(m.relPath)) return bad(w + ".relPath", "unsafe or invalid relative path");
    if (m.relPath.find("://") != std::string::npos) return bad(w + ".relPath", "network location not allowed");
    if (!formatOk(m.format)) return bad(w + ".format", "format not in allowlist");
  }

  std::set<std::string> textIds;
  for (size_t i = 0; i < d.texts.size(); ++i) {
    const TextMessage& t = d.texts[i];
    std::string w = "texts[" + std::to_string(i) + "]";
    if (!validId(t.id)) return bad(w + ".id", "invalid id");
    if (!textIds.insert(t.id).second) return bad(w + ".id", "duplicate");
    if (!utf8Valid(t.text, &cps)) return bad(w + ".text", "invalid UTF-8");
    if (cps > limits::kMaxTextCodePoints) return bad(w + ".text", "longer than 4096 code points");
    if (t.text.find('\0') != std::string::npos) return bad(w + ".text", "NUL not allowed");
  }

  std::set<std::string> outIds;
  for (size_t i = 0; i < d.outputs.size(); ++i) {
    const Output& o = d.outputs[i];
    std::string w = "outputs[" + std::to_string(i) + "]";
    if (!validId(o.logicalId)) return bad(w + ".logicalId", "invalid id");
    if (!outIds.insert(o.logicalId).second) return bad(w + ".logicalId", "duplicate");
    if (o.overlay.branding && !o.overlay.brandingRef.empty() && !mediaHashes.count(o.overlay.brandingRef)) return bad(w + ".overlay.brandingRef", "unknown media");
    if (o.displayHint.size() > 128 || !utf8Valid(o.displayHint)) return bad(w + ".displayHint", "invalid or too long");
  }

  std::set<std::string> cueIds;
  for (size_t i = 0; i < d.cues.size(); ++i) {
    const Cue& c = d.cues[i];
    std::string w = "cues[" + std::to_string(i) + "]";
    if (!validId(c.id)) return bad(w + ".id", "invalid id");
    if (!cueIds.insert(c.id).second) return bad(w + ".id", "duplicate");
    if (!c.sceneId.empty() && !sceneIds.count(c.sceneId)) return bad(w + ".sceneId", "unknown scene");
    if (!c.mediaRef.empty() && !mediaHashes.count(c.mediaRef)) return bad(w + ".mediaRef", "unknown media");
    if (!c.textRef.empty() && !textIds.count(c.textRef)) return bad(w + ".textRef", "unknown text message");
    if (c.lightingRef.size() > 64 || !utf8Valid(c.lightingRef)) return bad(w + ".lightingRef", "invalid");
    if (c.duration.beats) {
      if (!std::isfinite(c.duration.beatCount) || c.duration.beatCount < 0 || c.duration.beatCount > 4096) return bad(w + ".duration", "beats out of range 0..4096");
    } else if (c.duration.ms < 0 || c.duration.ms > 86400000LL) return bad(w + ".duration", "ms out of range");
    Status ts = checkTransition(w + ".transition", c.transition);
    if (!ts.ok()) return ts;
  }

  for (size_t i = 0; i < d.midiMappings.size(); ++i) { Status s = checkOpaque("midiMappings", i, d.midiMappings[i]); if (!s.ok()) return s; }
  for (size_t i = 0; i < d.fixtureProfiles.size(); ++i) { Status s = checkOpaque("fixtureProfiles", i, d.fixtureProfiles[i]); if (!s.ok()) return s; }
  for (size_t i = 0; i < d.patch.size(); ++i) { Status s = checkOpaque("patch", i, d.patch[i]); if (!s.ok()) return s; }

  const AutoConfig& a = d.autoCfg;
  if (a.minDwellMs < 500 || a.minDwellMs > 3600000) return bad("autoCfg.minDwellMs", "500..3600000");
  if (a.maxDwellMs < a.minDwellMs || a.maxDwellMs > 3600000) return bad("autoCfg.maxDwellMs", "must be >= minDwellMs and <= 3600000");
  if (a.repeatHistory < 0 || a.repeatHistory > 64) return bad("autoCfg.repeatHistory", "0..64");
  if (a.beatsPerBar < 1 || a.beatsPerBar > 16) return bad("autoCfg.beatsPerBar", "1..16");
  { Status ts = checkTransition("autoCfg.transition", a.transition); if (!ts.ok()) return ts; }
  std::set<std::string> poolSeen;
  for (size_t i = 0; i < a.pool.size(); ++i) {
    std::string w = "autoCfg.pool[" + std::to_string(i) + "]";
    if (!sceneIds.count(a.pool[i].sceneId)) return bad(w + ".sceneId", "unknown scene");
    if (!poolSeen.insert(a.pool[i].sceneId).second) return bad(w + ".sceneId", "duplicate pool entry");
    if (a.pool[i].weight < 0 || a.pool[i].weight > 1000) return bad(w + ".weight", "0..1000");
  }
  const IdleConfig& id = d.idle;
  if (id.screen != "black" && id.screen != "logo" && id.screen != "color_field" && id.screen != "scene") return bad("idle.screen", "unknown idle screen");
  if (id.screen == "scene" && !sceneIds.count(id.sceneId)) return bad("idle.sceneId", "unknown scene");
  if (id.audioLossMs < 250 || id.audioLossMs > 1500) return bad("idle.audioLossMs", "250..1500");

  for (size_t i = 0; i < d.journal.size(); ++i) {
    const JournalEntry& e = d.journal[i];
    if (e.kind.size() > 64 || e.json.size() > 4096 || !utf8Valid(e.kind) || !utf8Valid(e.json)) return bad("journal[" + std::to_string(i) + "]", "invalid entry");
  }
  for (const auto& kv : d.meta) {
    if (kv.first.compare(0, 2, "x_") != 0 || kv.first.size() > 64) return bad("meta." + kv.first, "metadata keys must start with x_");
    if (kv.second.size() > limits::kMaxOpaqueBytes) return bad("meta." + kv.first, "too large");
    nlohmann::json j = nlohmann::json::parse(kv.second, nullptr, false);
    if (j.is_discarded()) return bad("meta." + kv.first, "not JSON");
    std::string why;
    if (!scanOpaque(j, 0, why)) return bad("meta." + kv.first, why);
  }
  return Status::Ok();
}

} // namespace show
} // namespace ss
