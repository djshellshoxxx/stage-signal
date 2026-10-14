#include "persistence/show_json.h"
#include "nlohmann/json.hpp"
#include "persistence/hash.h"
#include <cmath>
#include <set>

namespace ss {
namespace persist {
using namespace show;
using json = nlohmann::json;

namespace {

Status bad(const std::string& where, const std::string& why) { return Status::Fail(Err::InvalidArgument, where + ": " + why, true); }

struct Ctx { bool lenient = false; std::vector<std::string>* warn = nullptr; };
#define TRY(x) do { Status _s = (x); if (!_s.ok()) return _s; } while (0)

Status unknownKeys(const json& j, std::initializer_list<const char*> allowed, const std::string& where, const Ctx& c, bool allowMeta = false) {
  std::set<std::string> ok(allowed.begin(), allowed.end());
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (ok.count(it.key())) continue;
    if (allowMeta && it.key().compare(0, 2, "x_") == 0) continue;
    if (c.lenient) { if (c.warn) c.warn->push_back("unsupported field ignored: " + where + "." + it.key()); continue; }
    return bad(where + "." + it.key(), "unknown field");
  }
  return Status::Ok();
}
Status needObj(const json& j, const std::string& w) { return j.is_object() ? Status::Ok() : bad(w, "expected object"); }
Status needArr(const json& j, const std::string& w, size_t maxN) {
  if (!j.is_array()) return bad(w, "expected array");
  if (j.size() > maxN) return bad(w, "too many entries (limit " + std::to_string(maxN) + ")");   // before allocation
  return Status::Ok();
}
Status getStr(const json& j, const char* k, std::string& out, const std::string& w, bool req, size_t maxLen = 1u << 20) {
  auto it = j.find(k);
  if (it == j.end()) return req ? bad(w + "." + k, "missing") : Status::Ok();
  if (!it->is_string()) return bad(w + "." + k, "expected string");
  if (it->get_ref<const std::string&>().size() > maxLen) return bad(w + "." + k, "string too long");
  out = it->get<std::string>(); return Status::Ok();
}
Status getInt(const json& j, const char* k, int64_t& out, const std::string& w, bool req) {
  auto it = j.find(k);
  if (it == j.end()) return req ? bad(w + "." + k, "missing") : Status::Ok();
  if (!it->is_number_integer()) return bad(w + "." + k, "expected integer");
  if (it->is_number_unsigned() && it->get<uint64_t>() > (uint64_t)INT64_MAX) return bad(w + "." + k, "out of range");
  out = it->get<int64_t>(); return Status::Ok();
}
Status getInt32(const json& j, const char* k, int& out, const std::string& w, bool req) {
  int64_t v = out; TRY(getInt(j, k, v, w, req));
  if (v < INT32_MIN || v > INT32_MAX) return bad(w + "." + k, "out of range");
  out = (int)v; return Status::Ok();
}
Status getBool(const json& j, const char* k, bool& out, const std::string& w, bool req) {
  auto it = j.find(k);
  if (it == j.end()) return req ? bad(w + "." + k, "missing") : Status::Ok();
  if (!it->is_boolean()) return bad(w + "." + k, "expected boolean");
  out = it->get<bool>(); return Status::Ok();
}
Status getNum(const json& j, const char* k, double& out, const std::string& w, bool req) {
  auto it = j.find(k);
  if (it == j.end()) return req ? bad(w + "." + k, "missing") : Status::Ok();
  if (!it->is_number()) return bad(w + "." + k, "expected number");
  out = it->get<double>(); return Status::Ok();
}

const char* quantName(Quantize q) { return q == Quantize::Beat ? "beat" : q == Quantize::Bar ? "bar" : "immediate"; }
Status parseQuant(const json& j, const char* k, Quantize& q, const std::string& w) {
  std::string s; TRY(getStr(j, k, s, w, false, 16));
  if (!j.contains(k)) return Status::Ok();
  if (s == "immediate") q = Quantize::Immediate; else if (s == "beat") q = Quantize::Beat; else if (s == "bar") q = Quantize::Bar;
  else return bad(w + "." + k, "unknown value");
  return Status::Ok();
}
const char* roleName(OutputRole r) { return r == OutputRole::Program ? "program" : r == OutputRole::Controls ? "controls" : "audience"; }

json transitionJson(const Transition& t) { return json{{"type", t.type}, {"ms", t.ms}}; }
Status parseTransition(const json& j, const char* k, Transition& t, const std::string& w, const Ctx& c) {
  auto it = j.find(k);
  if (it == j.end()) return Status::Ok();
  std::string ww = w + "." + k;
  TRY(needObj(*it, ww)); TRY(unknownKeys(*it, {"type", "ms"}, ww, c));
  TRY(getStr(*it, "type", t.type, ww, false, 32)); TRY(getInt32(*it, "ms", t.ms, ww, false));
  return Status::Ok();
}

Status opaqueList(const json& j, const char* k, size_t maxN, std::vector<std::string>& out, const Ctx&) {
  auto it = j.find(k);
  if (it == j.end()) return Status::Ok();
  std::string w = k;
  TRY(needArr(*it, w, maxN));
  for (size_t i = 0; i < it->size(); ++i) {
    const json& e = (*it)[i];
    if (!e.is_object()) return bad(w + "[" + std::to_string(i) + "]", "expected object");
    out.push_back(e.dump());
  }
  return Status::Ok();
}

Status convert(const json& s, ShowDocument& d, const Ctx& c) {
  TRY(needObj(s, "show"));
  TRY(unknownKeys(s, {"schema", "uuid", "name", "migrationHistory", "defaultMode", "scenes", "cues", "cueEnd", "outputs", "media", "texts",
                      "midiMappings", "fixtureProfiles", "patch", "qualityProfile", "auto", "idle", "overrides", "disableStrobing", "journal"}, "show", c, true));
  // Array counts first, before any element is materialised.
  if (s.contains("scenes")) TRY(needArr(s["scenes"], "scenes", limits::kMaxScenes));
  if (s.contains("cues")) TRY(needArr(s["cues"], "cues", limits::kMaxCues));
  if (s.contains("texts")) TRY(needArr(s["texts"], "texts", limits::kMaxMessages));
  if (s.contains("midiMappings")) TRY(needArr(s["midiMappings"], "midiMappings", limits::kMaxMidiMappings));
  if (s.contains("fixtureProfiles")) TRY(needArr(s["fixtureProfiles"], "fixtureProfiles", limits::kMaxFixtures));
  if (s.contains("patch")) TRY(needArr(s["patch"], "patch", limits::kMaxFixtures));
  if (s.contains("outputs")) TRY(needArr(s["outputs"], "outputs", limits::kMaxOutputs));
  if (s.contains("media")) TRY(needArr(s["media"], "media", limits::kMaxMedia));
  if (s.contains("journal")) TRY(needArr(s["journal"], "journal", limits::kMaxJournal));

  if (!s.contains("schema")) return bad("schema", "missing");
  TRY(needObj(s["schema"], "schema"));
  TRY(getInt32(s["schema"], "major", d.schemaMajor, "schema", true)); TRY(getInt32(s["schema"], "minor", d.schemaMinor, "schema", true));
  TRY(getStr(s, "uuid", d.uuid, "show", true, 64)); TRY(getStr(s, "name", d.name, "show", true, 1024));
  TRY(getStr(s, "qualityProfile", d.qualityProfile, "show", false, 128));
  TRY(getInt32(s, "defaultMode", d.defaultMode, "show", false));
  TRY(getBool(s, "disableStrobing", d.disableStrobing, "show", false));
  if (s.contains("migrationHistory")) {
    TRY(needArr(s["migrationHistory"], "migrationHistory", 256));
    for (const auto& e : s["migrationHistory"]) { if (!e.is_string()) return bad("migrationHistory", "expected strings"); d.migrationHistory.push_back(e.get<std::string>()); }
  }
  std::string ce; TRY(getStr(s, "cueEnd", ce, "show", false, 16));
  if (!ce.empty()) { if (ce == "hold") d.cueEnd = CueListEnd::Hold; else if (ce == "loop") d.cueEnd = CueListEnd::Loop; else return bad("cueEnd", "unknown value"); }

  if (s.contains("scenes")) for (size_t i = 0; i < s["scenes"].size(); ++i) {
    const json& e = s["scenes"][i]; std::string w = "scenes[" + std::to_string(i) + "]";
    TRY(needObj(e, w)); TRY(unknownKeys(e, {"id", "name", "rendererType", "params", "strobe", "cost"}, w, c));
    Scene sc;
    TRY(getStr(e, "id", sc.id, w, true, 128)); TRY(getStr(e, "name", sc.name, w, false, 1024)); TRY(getStr(e, "rendererType", sc.rendererType, w, true, 64));
    TRY(getBool(e, "strobe", sc.strobe, w, false)); TRY(getInt32(e, "cost", sc.cost, w, false));
    if (e.contains("params")) {
      TRY(needObj(e["params"], w + ".params"));
      if (e["params"].size() > limits::kMaxParams) return bad(w + ".params", "too many parameters");
      for (auto it = e["params"].begin(); it != e["params"].end(); ++it) {
        if (!it.value().is_number()) return bad(w + ".params." + it.key(), "expected number");
        sc.params[it.key()] = (float)it.value().get<double>();
      }
    }
    d.scenes.push_back(std::move(sc));
  }
  if (s.contains("cues")) for (size_t i = 0; i < s["cues"].size(); ++i) {
    const json& e = s["cues"][i]; std::string w = "cues[" + std::to_string(i) + "]";
    TRY(needObj(e, w)); TRY(unknownKeys(e, {"id", "sceneId", "duration", "transition", "quantize", "mediaRef", "textRef", "lightingRef"}, w, c));
    Cue cu;
    TRY(getStr(e, "id", cu.id, w, true, 128)); TRY(getStr(e, "sceneId", cu.sceneId, w, false, 128));
    TRY(getStr(e, "mediaRef", cu.mediaRef, w, false, 128)); TRY(getStr(e, "textRef", cu.textRef, w, false, 128)); TRY(getStr(e, "lightingRef", cu.lightingRef, w, false, 128));
    TRY(parseQuant(e, "quantize", cu.quantize, w)); TRY(parseTransition(e, "transition", cu.transition, w, c));
    if (e.contains("duration")) {
      const json& du = e["duration"]; TRY(needObj(du, w + ".duration")); TRY(unknownKeys(du, {"unit", "value"}, w + ".duration", c));
      std::string unit; TRY(getStr(du, "unit", unit, w + ".duration", true, 16));
      if (unit == "ms") { int64_t v = 0; TRY(getInt(du, "value", v, w + ".duration", true)); cu.duration.beats = false; cu.duration.ms = v; }
      else if (unit == "beats") { double v = 0; TRY(getNum(du, "value", v, w + ".duration", true)); cu.duration.beats = true; cu.duration.beatCount = v; }
      else return bad(w + ".duration.unit", "must be 'ms' or 'beats'");
    }
    d.cues.push_back(std::move(cu));
  }
  if (s.contains("outputs")) for (size_t i = 0; i < s["outputs"].size(); ++i) {
    const json& e = s["outputs"][i]; std::string w = "outputs[" + std::to_string(i) + "]";
    TRY(needObj(e, w)); TRY(unknownKeys(e, {"logicalId", "role", "overlay", "displayHint"}, w, c));
    Output o; std::string role;
    TRY(getStr(e, "logicalId", o.logicalId, w, true, 128)); TRY(getStr(e, "role", role, w, false, 16)); TRY(getStr(e, "displayHint", o.displayHint, w, false, 1024));
    if (!role.empty()) { if (role == "audience") o.role = OutputRole::Audience; else if (role == "program") o.role = OutputRole::Program; else if (role == "controls") o.role = OutputRole::Controls; else return bad(w + ".role", "unknown role"); }
    if (e.contains("overlay")) {
      const json& ov = e["overlay"]; TRY(needObj(ov, w + ".overlay")); TRY(unknownKeys(ov, {"text", "tempo", "key", "branding", "brandingRef"}, w + ".overlay", c));
      TRY(getBool(ov, "text", o.overlay.text, w + ".overlay", false)); TRY(getBool(ov, "tempo", o.overlay.tempo, w + ".overlay", false));
      TRY(getBool(ov, "key", o.overlay.key, w + ".overlay", false)); TRY(getBool(ov, "branding", o.overlay.branding, w + ".overlay", false));
      TRY(getStr(ov, "brandingRef", o.overlay.brandingRef, w + ".overlay", false, 128));
    }
    d.outputs.push_back(std::move(o));
  }
  if (s.contains("media")) for (size_t i = 0; i < s["media"].size(); ++i) {
    const json& e = s["media"][i]; std::string w = "media[" + std::to_string(i) + "]";
    TRY(needObj(e, w)); TRY(unknownKeys(e, {"hash", "relPath", "format", "bytes"}, w, c));
    MediaRecord m; int64_t b = 0;
    TRY(getStr(e, "hash", m.hash, w, true, 128)); TRY(getStr(e, "relPath", m.relPath, w, true, 1024)); TRY(getStr(e, "format", m.format, w, true, 16)); TRY(getInt(e, "bytes", b, w, false));
    if (b < 0) return bad(w + ".bytes", "negative");
    m.bytes = (uint64_t)b; d.media.push_back(std::move(m));
  }
  if (s.contains("texts")) for (size_t i = 0; i < s["texts"].size(); ++i) {
    const json& e = s["texts"][i]; std::string w = "texts[" + std::to_string(i) + "]";
    TRY(needObj(e, w)); TRY(unknownKeys(e, {"id", "text"}, w, c));
    TextMessage t;
    TRY(getStr(e, "id", t.id, w, true, 128)); TRY(getStr(e, "text", t.text, w, true, 4096 * 4 + 16));   // byte cap = 4096 cps * 4 bytes
    d.texts.push_back(std::move(t));
  }
  TRY(opaqueList(s, "midiMappings", limits::kMaxMidiMappings, d.midiMappings, c));
  TRY(opaqueList(s, "fixtureProfiles", limits::kMaxFixtures, d.fixtureProfiles, c));
  TRY(opaqueList(s, "patch", limits::kMaxFixtures, d.patch, c));
  if (s.contains("auto")) {
    const json& a = s["auto"]; TRY(needObj(a, "auto")); TRY(unknownKeys(a, {"seed", "pool", "minDwellMs", "maxDwellMs", "repeatHistory", "quantize", "beatsPerBar", "transition"}, "auto", c));
    if (a.contains("seed")) {
      if (a["seed"].is_string()) { const std::string& ss_ = a["seed"].get_ref<const std::string&>(); if (ss_.empty() || ss_.size() > 20 || ss_.find_first_not_of("0123456789") != std::string::npos) return bad("auto.seed", "invalid"); d.autoCfg.seed = std::stoull(ss_); }
      else if (a["seed"].is_number_unsigned()) d.autoCfg.seed = a["seed"].get<uint64_t>();
      else return bad("auto.seed", "expected unsigned integer or decimal string");
    }
    TRY(getInt32(a, "minDwellMs", d.autoCfg.minDwellMs, "auto", false)); TRY(getInt32(a, "maxDwellMs", d.autoCfg.maxDwellMs, "auto", false));
    TRY(getInt32(a, "repeatHistory", d.autoCfg.repeatHistory, "auto", false)); TRY(getInt32(a, "beatsPerBar", d.autoCfg.beatsPerBar, "auto", false));
    TRY(parseQuant(a, "quantize", d.autoCfg.quantize, "auto")); TRY(parseTransition(a, "transition", d.autoCfg.transition, "auto", c));
    if (a.contains("pool")) {
      TRY(needArr(a["pool"], "auto.pool", limits::kMaxPool));
      for (size_t i = 0; i < a["pool"].size(); ++i) {
        const json& e = a["pool"][i]; std::string w = "auto.pool[" + std::to_string(i) + "]";
        TRY(needObj(e, w)); TRY(unknownKeys(e, {"sceneId", "weight"}, w, c));
        PoolEntry p; TRY(getStr(e, "sceneId", p.sceneId, w, true, 128)); TRY(getInt32(e, "weight", p.weight, w, false));
        d.autoCfg.pool.push_back(std::move(p));
      }
    }
  }
  if (s.contains("idle")) {
    const json& a = s["idle"]; TRY(needObj(a, "idle")); TRY(unknownKeys(a, {"audioLoss", "screen", "sceneId", "audioLossMs"}, "idle", c));
    std::string pol; TRY(getStr(a, "audioLoss", pol, "idle", false, 16));
    if (!pol.empty()) { if (pol == "continue") d.idle.audioLoss = AudioLossPolicy::Continue; else if (pol == "idle") d.idle.audioLoss = AudioLossPolicy::Idle; else return bad("idle.audioLoss", "unknown value"); }
    TRY(getStr(a, "screen", d.idle.screen, "idle", false, 32)); TRY(getStr(a, "sceneId", d.idle.sceneId, "idle", false, 128)); TRY(getInt32(a, "audioLossMs", d.idle.audioLossMs, "idle", false));
  }
  if (s.contains("overrides")) {
    const json& a = s["overrides"]; TRY(needObj(a, "overrides")); TRY(unknownKeys(a, {"bpm", "key"}, "overrides", c));
    TRY(getNum(a, "bpm", d.manualBpm, "overrides", false)); TRY(getInt32(a, "key", d.manualKey, "overrides", false));
  }
  if (s.contains("journal")) for (size_t i = 0; i < s["journal"].size(); ++i) {
    const json& e = s["journal"][i]; std::string w = "journal[" + std::to_string(i) + "]";
    TRY(needObj(e, w)); TRY(unknownKeys(e, {"wallMs", "kind", "json"}, w, c));
    JournalEntry je; TRY(getInt(e, "wallMs", je.wallMs, w, false)); TRY(getStr(e, "kind", je.kind, w, true, 128)); TRY(getStr(e, "json", je.json, w, false, 8192));
    d.journal.push_back(std::move(je));
  }
  for (auto it = s.begin(); it != s.end(); ++it) if (it.key().compare(0, 2, "x_") == 0) d.meta[it.key()] = it.value().dump();
  return Status::Ok();
}

json toJson(const ShowDocument& d) {
  json s;
  s["schema"] = {{"major", d.schemaMajor}, {"minor", d.schemaMinor}};
  s["uuid"] = d.uuid; s["name"] = d.name; s["migrationHistory"] = d.migrationHistory; s["defaultMode"] = d.defaultMode;
  s["qualityProfile"] = d.qualityProfile; s["disableStrobing"] = d.disableStrobing;
  s["cueEnd"] = d.cueEnd == CueListEnd::Loop ? "loop" : "hold";
  s["scenes"] = json::array();
  for (const auto& sc : d.scenes) {
    json p = json::object(); for (const auto& kv : sc.params) p[kv.first] = kv.second;
    s["scenes"].push_back({{"id", sc.id}, {"name", sc.name}, {"rendererType", sc.rendererType}, {"params", p}, {"strobe", sc.strobe}, {"cost", sc.cost}});
  }
  s["cues"] = json::array();
  for (const auto& cu : d.cues) {
    json du = cu.duration.beats ? json{{"unit", "beats"}, {"value", cu.duration.beatCount}} : json{{"unit", "ms"}, {"value", cu.duration.ms}};
    s["cues"].push_back({{"id", cu.id}, {"sceneId", cu.sceneId}, {"duration", du}, {"transition", transitionJson(cu.transition)}, {"quantize", quantName(cu.quantize)},
                         {"mediaRef", cu.mediaRef}, {"textRef", cu.textRef}, {"lightingRef", cu.lightingRef}});
  }
  s["outputs"] = json::array();
  for (const auto& o : d.outputs)
    s["outputs"].push_back({{"logicalId", o.logicalId}, {"role", roleName(o.role)}, {"displayHint", o.displayHint},
      {"overlay", {{"text", o.overlay.text}, {"tempo", o.overlay.tempo}, {"key", o.overlay.key}, {"branding", o.overlay.branding}, {"brandingRef", o.overlay.brandingRef}}}});
  s["media"] = json::array();
  for (const auto& m : d.media) s["media"].push_back({{"hash", m.hash}, {"relPath", m.relPath}, {"format", m.format}, {"bytes", m.bytes}});
  s["texts"] = json::array();
  for (const auto& t : d.texts) s["texts"].push_back({{"id", t.id}, {"text", t.text}});
  auto opq = [&](const char* k, const std::vector<std::string>& v) { s[k] = json::array(); for (const auto& e : v) s[k].push_back(json::parse(e)); };
  opq("midiMappings", d.midiMappings); opq("fixtureProfiles", d.fixtureProfiles); opq("patch", d.patch);
  json pool = json::array();
  for (const auto& p : d.autoCfg.pool) pool.push_back({{"sceneId", p.sceneId}, {"weight", p.weight}});
  s["auto"] = {{"seed", std::to_string(d.autoCfg.seed)}, {"pool", pool}, {"minDwellMs", d.autoCfg.minDwellMs}, {"maxDwellMs", d.autoCfg.maxDwellMs},
               {"repeatHistory", d.autoCfg.repeatHistory}, {"quantize", quantName(d.autoCfg.quantize)}, {"beatsPerBar", d.autoCfg.beatsPerBar}, {"transition", transitionJson(d.autoCfg.transition)}};
  s["idle"] = {{"audioLoss", d.idle.audioLoss == AudioLossPolicy::Idle ? "idle" : "continue"}, {"screen", d.idle.screen}, {"sceneId", d.idle.sceneId}, {"audioLossMs", d.idle.audioLossMs}};
  s["overrides"] = {{"bpm", d.manualBpm}, {"key", d.manualKey}};
  s["journal"] = json::array();
  for (const auto& e : d.journal) s["journal"].push_back({{"wallMs", e.wallMs}, {"kind", e.kind}, {"json", e.json}});
  for (const auto& kv : d.meta) { json v = json::parse(kv.second, nullptr, false); if (!v.is_discarded()) s[kv.first] = v; }
  return s;
}

// Reject pathological nesting before the DOM parser sees it (cheap byte scan that honours strings).
bool depthOk(const uint8_t* d, size_t n, int maxDepth) {
  int depth = 0; bool inStr = false;
  for (size_t i = 0; i < n; ++i) {
    uint8_t c = d[i];
    if (inStr) { if (c == '\\') ++i; else if (c == '"') inStr = false; continue; }
    if (c == '"') inStr = true;
    else if (c == '{' || c == '[') { if (++depth > maxDepth) return false; }
    else if (c == '}' || c == ']') --depth;
  }
  return true;
}

// ---- migrations (each upgrades major N -> N+1 in place; callers back up the source first) ----------
Status migrate0to1(json& s) {
  if (!s.is_object()) return bad("show", "expected object");
  if (s.contains("title") && !s.contains("name")) { s["name"] = s["title"]; s.erase("title"); }
  if (s.contains("scenes") && s["scenes"].is_array())
    for (auto& sc : s["scenes"]) if (sc.is_object() && sc.contains("renderer") && !sc.contains("rendererType")) { sc["rendererType"] = sc["renderer"]; sc.erase("renderer"); }
  json hist = s.contains("migrationHistory") && s["migrationHistory"].is_array() ? s["migrationHistory"] : json::array();
  hist.push_back("0.0->1.0");
  s["migrationHistory"] = hist;
  s["schema"] = {{"major", 1}, {"minor", 0}};
  return Status::Ok();
}
} // namespace

std::string showObjectJson(const ShowDocument& d) { return toJson(d).dump(); }

std::string encodeShow(const ShowDocument& d) {
  json s = toJson(d);
  std::string compact = s.dump();
  json env; env["format"] = kShowFormat; env["checksum"] = "sha256:" + Sha256::hex(compact.data(), compact.size()); env["show"] = std::move(s);
  return env.dump();
}

bool peekSchema(const uint8_t* d, size_t n, int& major, int& minor) {
  if (n > limits::kMaxJsonBytes + 4096 || !depthOk(d, n, 64)) return false;
  json j = json::parse(d, d + n, nullptr, false);
  if (j.is_discarded() || !j.is_object() || !j.contains("show") || !j["show"].is_object() || !j["show"].contains("schema")) return false;
  const json& sc = j["show"]["schema"];
  if (!sc.is_object() || !sc.contains("major") || !sc["major"].is_number_integer()) return false;
  major = sc["major"].get<int>(); minor = sc.contains("minor") && sc["minor"].is_number_integer() ? sc["minor"].get<int>() : 0;
  return true;
}

Status decodeShow(const uint8_t* data, size_t n, ShowDocument& out, DecodeInfo* info) {
  if (n > limits::kMaxJsonBytes + 4096) return Status::Fail(Err::ResourceExhausted, "show file larger than 8 MiB JSON limit");
  if (!depthOk(data, n, 64)) return bad("show", "JSON nesting too deep");
  json env = json::parse(data, data + n, nullptr, false);
  if (env.is_discarded()) return bad("show", "not valid JSON");
  if (!env.is_object()) return bad("show", "expected object");
  if (!env.contains("format") || !env["format"].is_string() || env["format"].get<std::string>() != kShowFormat) return bad("format", "not a StageSignal show file");
  if (!env.contains("show") || !env["show"].is_object()) return bad("show", "missing");
  json& s = env["show"];
  int major = -1, minor = 0;
  if (!s.contains("schema") || !s["schema"].is_object() || !s["schema"].contains("major") || !s["schema"]["major"].is_number_integer()) return bad("schema", "missing or invalid");
  major = s["schema"]["major"].get<int>();
  if (s["schema"].contains("minor") && s["schema"]["minor"].is_number_integer()) minor = s["schema"]["minor"].get<int>();
  if (major > kSchemaMajor) {
    Status st = Status::Fail(Err::SchemaUnsupported, "show was written by a newer StageSignal (schema " + std::to_string(major) + "." + std::to_string(minor) + "); this build supports up to " + std::to_string(kSchemaMajor), false);
    return st;
  }
  if (major < 0) return bad("schema.major", "negative");
  // Checksum over the compact dump of the show object *as stored* (before migration).
  if (!env.contains("checksum") || !env["checksum"].is_string()) return bad("checksum", "missing");
  std::string compact = s.dump();
  if (env["checksum"].get<std::string>() != "sha256:" + Sha256::hex(compact.data(), compact.size())) return bad("checksum", "mismatch: file is corrupt or was edited");
  DecodeInfo local; DecodeInfo& inf = info ? *info : local;
  inf = DecodeInfo(); inf.fromMajor = major; inf.fromMinor = minor;
  while (major < kSchemaMajor) {
    Status ms = major == 0 ? migrate0to1(s) : Status::Fail(Err::SchemaUnsupported, "no migration path from schema " + std::to_string(major), false);
    if (!ms.ok()) return ms;
    ++major; inf.migrated = true;
  }
  ShowDocument d;
  Ctx c; c.lenient = (major == kSchemaMajor && minor > kSchemaMinor); c.warn = &inf.warnings;
  Status cs = convert(s, d, c);
  if (!cs.ok()) return cs;
  Status vs = validateShow(d);
  if (!vs.ok()) return vs;
  out = std::move(d);
  return Status::Ok();
}

// ---- export sanitising -------------------------------------------------------------------------------------
namespace {
bool physicalKey(const std::string& k) {
  static const std::set<std::string> ks = {"deviceid", "portid", "stableid", "physicalid", "endpointid", "hwid", "audiodeviceid", "displayid", "serial", "macaddress", "hostname", "nicaddr"};
  std::string l; for (char c : k) l.push_back((char)tolower((unsigned char)c));
  return ks.count(l) > 0;
}
bool secretKey(const std::string& k) {
  static const std::set<std::string> ks = {"secret", "secretref", "streamkey", "password", "passwd", "token", "apikey", "api_key", "credential", "credentials", "privatekey", "authorization"};
  std::string l; for (char c : k) l.push_back((char)tolower((unsigned char)c));
  return ks.count(l) > 0;
}
void scrub(json& j, const std::string& path, std::vector<std::string>& removed) {
  if (j.is_object()) {
    std::vector<std::string> kill;
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (physicalKey(it.key()) || secretKey(it.key())) kill.push_back(it.key());
      else scrub(it.value(), path + "." + it.key(), removed);
    }
    for (const auto& k : kill) { j.erase(k); removed.push_back(path + "." + k); }
  } else if (j.is_array()) {
    for (size_t i = 0; i < j.size(); ++i) scrub(j[i], path + "[" + std::to_string(i) + "]", removed);
  }
}
void scrubList(std::vector<std::string>& v, const char* name, std::vector<std::string>& removed) {
  for (size_t i = 0; i < v.size(); ++i) {
    json j = json::parse(v[i], nullptr, false);
    if (j.is_discarded()) continue;
    std::vector<std::string> r; scrub(j, std::string(name) + "[" + std::to_string(i) + "]", r);
    if (!r.empty()) { v[i] = j.dump(); removed.insert(removed.end(), r.begin(), r.end()); }
  }
}
} // namespace

std::vector<std::string> sanitizeForExport(ShowDocument& d) {
  std::vector<std::string> removed;
  scrubList(d.midiMappings, "midiMappings", removed);
  scrubList(d.fixtureProfiles, "fixtureProfiles", removed);
  scrubList(d.patch, "patch", removed);
  for (auto& kv : d.meta) {
    json j = json::parse(kv.second, nullptr, false);
    if (j.is_discarded()) continue;
    std::vector<std::string> r; scrub(j, "meta." + kv.first, r);
    if (!r.empty()) { kv.second = j.dump(); removed.insert(removed.end(), r.begin(), r.end()); }
  }
  for (auto& o : d.outputs) if (!o.displayHint.empty()) {
    // hints are non-secret matching text only; drop anything that looks like a path or URL
    if (o.displayHint.find('/') != std::string::npos || o.displayHint.find('\\') != std::string::npos || o.displayHint.find("://") != std::string::npos) { o.displayHint.clear(); removed.push_back("outputs." + o.logicalId + ".displayHint"); }
  }
  if (!d.journal.empty()) { d.journal.clear(); removed.push_back("journal"); }
  return removed;
}

} // namespace persist
} // namespace ss
